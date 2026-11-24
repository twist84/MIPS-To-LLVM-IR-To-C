#pragma once

#include "ee_vu1_aot.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <utility>
#include <memory>
#include <mutex>
#include <thread>

namespace ee
{
// Host-only owning slot. Copying snapshots the complete resident machine;
// swapping transfers storage without copying either 16 KiB memory bank.
// Copy assignment preserves the destination address (committed MMIO view).
class c_ee_vu1_resident_slot
{
public:
    c_ee_vu1_resident_slot() = default;
    c_ee_vu1_resident_slot(const c_ee_vu1_resident_slot& other)
        : m_value(std::make_unique<s_ee_vu1_resident>(*other.m_value))
    {
    }

    c_ee_vu1_resident_slot& operator=(const c_ee_vu1_resident_slot& other)
    {
        *m_value = *other.m_value;
        return *this;
    }

    s_ee_vu1_resident& operator*() { return *m_value; }
    const s_ee_vu1_resident& operator*() const { return *m_value; }
    s_ee_vu1_resident* operator->() { return m_value.get(); }
    const s_ee_vu1_resident* operator->() const { return m_value.get(); }

    void swap(c_ee_vu1_resident_slot& other) noexcept
    {
        m_value.swap(other.m_value);
    }

private:
    std::unique_ptr<s_ee_vu1_resident> m_value =
        std::make_unique<s_ee_vu1_resident>();
};

// Host-owned job, not a committed VIF transaction. Only the worker
// mutates it between submit and take. PATH1 can contain an existing
// staged prefix so XGKICK retains the aggregate packet bound.
struct s_ee_vu1_job
{
    c_ee_vu1_resident_slot m_resident{};
    uint32_t m_address = 0u;
    uint32_t m_top = 0u;
    uint32_t m_itop = 0u;
    std::vector<uint8_t> m_path1;
    s_ee_vu1_issue m_issue{};
    bool m_accepted = false;
    bool m_host_failed = false;
    uint64_t m_execution_ns = 0u;
    uint64_t m_submitted_ns = 0u;
    uint64_t m_started_ns = 0u;
    uint64_t m_finished_ns = 0u;
    uint64_t m_compiled_ns = 0u;
    uint64_t m_compiled_pairs = 0u;
    uint64_t m_xgkick_ns = 0u;
};

// Single producer/consumer controller, one outstanding job (including
// an untaken result). Never publishes GS or DMAC state from the worker.
// The optional AOT backend transfers exclusive ownership at construction;
// the worker installs its own thread-local binding. No runner-owned
// counters, DLL lifetime or guest state are borrowed.
class c_ee_vu1_worker
{
public:
    explicit c_ee_vu1_worker(std::unique_ptr<c_ee_vu1_aot> backend = {},
        bool detailed_profile = false);
    ~c_ee_vu1_worker();

    c_ee_vu1_worker(const c_ee_vu1_worker&) = delete;
    c_ee_vu1_worker& operator=(const c_ee_vu1_worker&) = delete;

    // On failure the caller retains the job. On success it must not
    // keep references into the transferred object.
    bool submit(std::unique_ptr<s_ee_vu1_job>& job,
        uint64_t* submitted_ns = nullptr);
    std::unique_ptr<s_ee_vu1_job> try_take();
    std::unique_ptr<s_ee_vu1_job> wait_take();
    bool busy() const;
    // A single acquire observes the same publication as try_take().
    // It neither consumes work nor changes guest-visible state.
    bool ready() const { return m_ready.load(std::memory_order_acquire); }
    static uint64_t clock_ns()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    uint64_t started_ns() const { return m_started_ns.load(std::memory_order_acquire); }
    uint64_t finished_ns() const { return m_finished_ns.load(std::memory_order_acquire); }
    std::pair<uint64_t, uint64_t> execution_interval() const
    {
        return {started_ns(), finished_ns()};
    }
    void write_aot_json(FILE* output) const;

    // Full reset can discard a result, but cannot allow old work to
    // race a new generation. No timeout or fabricated completion.
    void discard();

private:
    void run();
    std::unique_ptr<s_ee_vu1_job> take_locked();

    std::unique_ptr<c_ee_vu1_aot> m_backend;
    mutable std::mutex m_backend_mutex;
    bool m_detailed_profile = false;
    std::atomic<bool> m_ready{false};
    std::atomic<uint64_t> m_started_ns{0u};
    std::atomic<uint64_t> m_finished_ns{0u};
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::unique_ptr<s_ee_vu1_job> m_pending;
    std::unique_ptr<s_ee_vu1_job> m_finished;
    bool m_busy = false;
    bool m_stopping = false;
    std::thread m_thread;
};
} // namespace ee
