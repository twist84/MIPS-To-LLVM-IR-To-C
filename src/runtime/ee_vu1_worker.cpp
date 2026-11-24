#include "ee_vu1_worker.h"
#include "ee_graphics_batch.h"
#include "ee_runtime_profile.h"

#include <chrono>
#include <utility>

namespace ee
{
c_ee_vu1_worker::c_ee_vu1_worker(std::unique_ptr<c_ee_vu1_aot> backend,
    bool detailed_profile)
    : m_backend(std::move(backend)),
      m_detailed_profile(detailed_profile),
      m_thread(&c_ee_vu1_worker::run, this)
{
}

c_ee_vu1_worker::~c_ee_vu1_worker()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_changed.notify_all();
    m_thread.join();
}

bool c_ee_vu1_worker::submit(std::unique_ptr<s_ee_vu1_job>& job,
    uint64_t* submitted_ns)
{
    bool submitted = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stopping && !m_busy && job != nullptr &&
            (job->m_path1.size() & 15u) == 0u &&
            job->m_path1.size() <= static_cast<size_t>(k_max_graphics_batch_qwc) * 16u)
        {
            m_started_ns.store(0u, std::memory_order_relaxed);
            m_finished_ns.store(0u, std::memory_order_relaxed);
            job->m_submitted_ns = clock_ns();
            if (submitted_ns != nullptr)
            {
                *submitted_ns = job->m_submitted_ns;
            }
            m_pending = std::move(job);
            m_busy = true;
            submitted = true;
        }
    }
    if (submitted)
    {
        m_changed.notify_all();
    }
    return submitted;
}

std::unique_ptr<s_ee_vu1_job> c_ee_vu1_worker::take_locked()
{
    auto result = std::move(m_finished);
    if (result != nullptr)
    {
        m_busy = false;
        m_ready.store(false, std::memory_order_relaxed);
    }
    return result;
}

std::unique_ptr<s_ee_vu1_job> c_ee_vu1_worker::try_take()
{
    if (!m_ready.load(std::memory_order_acquire))
    {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return take_locked();
}

std::unique_ptr<s_ee_vu1_job> c_ee_vu1_worker::wait_take()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_changed.wait(lock, [this]()
    {
        return !m_busy || m_finished != nullptr;
    });
    return take_locked();
}

bool c_ee_vu1_worker::busy() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_busy;
}

void c_ee_vu1_worker::discard()
{
    (void)wait_take();
}

uint64_t c_ee_vu1_worker::clock_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::pair<uint64_t, uint64_t> c_ee_vu1_worker::execution_interval() const
{
    return {m_started_ns.load(std::memory_order_acquire),
        m_finished_ns.load(std::memory_order_acquire)};
}

void c_ee_vu1_worker::write_aot_json(FILE* output) const
{
    // A report may sample while a program is active. Serialize only
    // the host-side backend counters; never publish guest state here.
    std::lock_guard<std::mutex> lock(m_backend_mutex);
    if (m_backend != nullptr)
    {
        m_backend->write_json(output);
    }
    else
    {
        std::fputs("{\"enabled\":false}", output);
    }
}

void c_ee_vu1_worker::run()
{
    c_vu1_aot_binding binding(m_backend.get());
    runtime_profile_begin(m_detailed_profile);
    for (;;)
    {
        std::unique_ptr<s_ee_vu1_job> job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_changed.wait(lock, [this]()
            {
                return m_stopping || m_pending != nullptr;
            });
            if (m_stopping)
            {
                break;
            }
            job = std::move(m_pending);
        }

        job->m_issue = {};
        job->m_accepted = false;
        job->m_host_failed = false;
        {
            // The AOT backend's mutable diagnostics are worker-owned.
            // Only report serialization shares this short ownership lock.
            std::lock_guard<std::mutex> lock(m_backend_mutex);
            const auto& compiled = g_runtime_profile.m_counters[
                static_cast<size_t>(e_runtime_profile_id::vu1_compiled)];
            const auto& kick = g_runtime_profile.m_counters[
                static_cast<size_t>(e_runtime_profile_id::vu1_xgkick)];
            const auto compiled_before = compiled;
            const auto kick_before = kick;
            job->m_started_ns = clock_ns();
            m_started_ns.store(job->m_started_ns, std::memory_order_release);
            try
            {
                auto& resident = job->m_resident;
                job->m_accepted = c_ee_vu1::run(
                    resident.m_state, resident.m_code, resident.m_data,
                    job->m_address, job->m_top, job->m_itop,
                    job->m_path1, &job->m_issue);
            }
            catch (...)
            {
                job->m_accepted = false;
                job->m_host_failed = true;
            }
            job->m_finished_ns = clock_ns();
            job->m_execution_ns = job->m_finished_ns - job->m_started_ns;
            m_finished_ns.store(job->m_finished_ns, std::memory_order_release);
            job->m_compiled_ns = compiled.m_exclusive_ns -
                compiled_before.m_exclusive_ns;
            job->m_compiled_pairs = compiled.m_work - compiled_before.m_work;
            job->m_xgkick_ns = kick.m_exclusive_ns - kick_before.m_exclusive_ns;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_finished = std::move(job);
            m_ready.store(true, std::memory_order_release);
        }
        m_changed.notify_all();
    }
}
} // namespace ee
