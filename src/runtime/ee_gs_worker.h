#pragma once

#include "ee_gif.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ee
{
// One committed render-observer batch, never a guest transaction. Packed
// and reglist values already live in the events; only IMAGE data needs
// independent backing bytes. Non-image byte offsets are provenance only.
class c_ee_gs_worker
{
public:
    using t_consume = void (*)(void*, const s_ee_gif_event*, size_t,
                               const uint8_t*, uint32_t);
    enum class e_submit
    {
        accepted,
        too_large,
        invalid,
        failed
    };
    struct s_statistics
    {
        uint64_t m_submitted = 0u;
        uint64_t m_completed = 0u;
        uint64_t m_execution_ns = 0u;
        uint64_t m_submit_copy_ns = 0u;
        uint64_t m_fence_wait_ns = 0u;
        uint64_t m_fence_calls = 0u;
        uint64_t m_backpressure_ns = 0u;
        uint64_t m_backpressure_waits = 0u;
        uint64_t m_peak_bytes = 0u;
        uint64_t m_peak_jobs = 0u;
        uint64_t m_event_bytes = 0u;
        uint64_t m_image_bytes = 0u;
    };

    static constexpr size_t k_default_bytes = 64u * 1024u * 1024u;
    static constexpr size_t k_default_jobs = 4u;

    explicit c_ee_gs_worker(void* user, t_consume consume,
        size_t byte_limit = k_default_bytes, size_t job_limit = k_default_jobs)
        : m_user(user), m_consume(consume), m_byte_limit(byte_limit),
          m_job_limit(job_limit), m_thread(&c_ee_gs_worker::run, this)
    {
    }

    ~c_ee_gs_worker()
    {
        (void)drain();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_changed.notify_all();
        m_thread.join();
    }

    c_ee_gs_worker(const c_ee_gs_worker&) = delete;
    c_ee_gs_worker& operator=(const c_ee_gs_worker&) = delete;

    // Single producer. A successful submit owns all bytes before returning.
    // Backpressure includes the executing batch; no queue slot is reused
    // while the consumer still owns its data. A too-large batch may be
    // consumed synchronously by the caller only AFTER a successful drain.
    e_submit submit(const s_ee_gif_event* events, size_t count,
                    const uint8_t* data, uint32_t qwc)
    {
        // Unsigned multiplication is defined even on 32-bit hosts. A
        // round-trip detects overflow without an impossible x64 comparison.
        const size_t input_bytes = static_cast<size_t>(qwc) * 16u;
        if (m_consume == nullptr || (events == nullptr && count != 0u) ||
            (data == nullptr && qwc != 0u) ||
            input_bytes / 16u != qwc ||
            count > std::numeric_limits<size_t>::max() / sizeof(s_ee_gif_event))
        {
            return e_submit::invalid;
        }
        const uint64_t prepare_start = now_ns();
        const size_t event_bytes = count * sizeof(s_ee_gif_event);
        size_t image_bytes = 0u;
        // IMAGE events are usually sparse among committed register
        // writes. Retain a bounded index list while validating the
        // source. Dense batches use the original full-scan fallback.
        constexpr size_t k_image_index_capacity = 128u;
        std::array<size_t, k_image_index_capacity> image_indices;
        size_t image_count = 0u;
        for (size_t i = 0u; i < count; ++i)
        {
            const auto& event = events[i];
            if (event.m_type != e_ee_gif_event_type::image || event.m_qwc == 0u)
            {
                continue;
            }
            if (event.m_byte_offset > input_bytes ||
                event.m_qwc > (input_bytes - event.m_byte_offset) / 16u ||
                event.m_qwc > (std::numeric_limits<size_t>::max() - image_bytes) / 16u)
            {
                return e_submit::invalid;
            }
            image_bytes += static_cast<size_t>(event.m_qwc) * 16u;
            if (image_count < image_indices.size())
            {
                image_indices[image_count] = i;
            }
            ++image_count;
        }
        if (m_job_limit == 0u || event_bytes > m_byte_limit ||
            image_bytes > m_byte_limit - event_bytes ||
            image_bytes > std::numeric_limits<uint32_t>::max())
        {
            return e_submit::too_large;
        }
        const size_t bytes = event_bytes + image_bytes;
        const uint64_t preflight_ns = now_ns() - prepare_start;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            const auto can_submit = [this, bytes]()
            {
                return m_failed.load(std::memory_order_relaxed) || m_stopping ||
                    (m_jobs < m_job_limit && bytes <= m_byte_limit - m_bytes);
            };
            if (!can_submit())
            {
                const uint64_t begin = now_ns();
                m_changed.wait(lock, can_submit);
                ++m_statistics.m_backpressure_waits;
                m_statistics.m_backpressure_ns += now_ns() - begin;
            }
            if (m_failed.load(std::memory_order_relaxed) || m_stopping)
            {
                return e_submit::failed;
            }
        }

        // Only this producer can reserve another slot; the worker can
        // merely free slots while this owned copy is constructed.
        const uint64_t copy_start = now_ns();
        s_batch batch{};
        try
        {
            if (count != 0u)
            {
                batch.m_events.assign(events, events + count);
            }
            batch.m_images.reserve(image_bytes);
            const auto copy_image = [&](s_ee_gif_event& event)
            {
                const size_t offset = batch.m_images.size();
                const auto* begin = data + event.m_byte_offset;
                batch.m_images.insert(batch.m_images.end(), begin,
                    begin + static_cast<size_t>(event.m_qwc) * 16u);
                event.m_byte_offset = static_cast<uint32_t>(offset);
            };
            if (image_count <= image_indices.size())
            {
                // Includes the no-IMAGE case: the event copy above is
                // sufficient and there is no second event traversal.
                for (size_t i = 0u; i < image_count; ++i)
                {
                    copy_image(batch.m_events[image_indices[i]]);
                }
            }
            else
            {
                // The stack index list is only an acceleration hint,
                // not a capacity limit on valid guest GIF transactions.
                for (auto& event : batch.m_events)
                {
                    if (event.m_type == e_ee_gif_event_type::image &&
                        event.m_qwc != 0u)
                    {
                        copy_image(event);
                    }
                }
            }
            batch.m_bytes = bytes;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_failed.load(std::memory_order_relaxed) || m_stopping)
            {
                return e_submit::failed;
            }
            m_queue.push_back(std::move(batch));
            m_bytes += bytes;
            ++m_jobs;
            ++m_statistics.m_submitted;
            m_statistics.m_submit_copy_ns += preflight_ns + now_ns() - copy_start;
            m_statistics.m_event_bytes += event_bytes;
            m_statistics.m_image_bytes += image_bytes;
            if (m_bytes > m_statistics.m_peak_bytes)
            {
                m_statistics.m_peak_bytes = m_bytes;
            }
            if (m_jobs > m_statistics.m_peak_jobs)
            {
                m_statistics.m_peak_jobs = m_jobs;
            }
        }
        catch (...)
        {
            return e_submit::failed;
        }
        m_changed.notify_all();
        return e_submit::accepted;
    }

    bool drain()
    {
        const uint64_t begin = now_ns();
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this]()
        {
            return m_jobs == 0u || m_failed.load(std::memory_order_relaxed);
        });
        ++m_statistics.m_fence_calls;
        m_statistics.m_fence_wait_ns += now_ns() - begin;
        return !m_failed.load(std::memory_order_relaxed);
    }

    bool healthy() const
    {
        return !m_failed.load(std::memory_order_acquire);
    }

    s_statistics statistics() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_statistics;
    }

private:
    struct s_batch
    {
        std::vector<s_ee_gif_event> m_events;
        std::vector<uint8_t> m_images;
        size_t m_bytes = 0u;
    };

    static uint64_t now_ns()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void run()
    {
        for (;;)
        {
            s_batch batch{};
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_changed.wait(lock, [this]()
                {
                    return m_stopping || !m_queue.empty();
                });
                if (m_stopping && m_queue.empty())
                {
                    break;
                }
                batch = std::move(m_queue.front());
                m_queue.pop_front();
            }
            const uint64_t begin = now_ns();
            bool success = true;
            try
            {
                m_consume(m_user, batch.m_events.data(), batch.m_events.size(),
                    batch.m_images.data(),
                    static_cast<uint32_t>(batch.m_images.size() / 16u));
            }
            catch (...)
            {
                success = false;
            }
            const uint64_t elapsed = now_ns() - begin;
            // Release owned backing before advertising capacity again.
            const size_t bytes = batch.m_bytes;
            batch = {};
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_statistics.m_execution_ns += elapsed;
                --m_jobs;
                m_bytes -= bytes;
                if (success)
                {
                    ++m_statistics.m_completed;
                }
                else
                {
                    m_failed.store(true, std::memory_order_release);
                    m_queue.clear();
                    m_jobs = 0u;
                    m_bytes = 0u;
                }
            }
            m_changed.notify_all();
        }
    }

    void* m_user;
    t_consume m_consume;
    const size_t m_byte_limit;
    const size_t m_job_limit;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<s_batch> m_queue;
    size_t m_bytes = 0u;
    size_t m_jobs = 0u;
    bool m_stopping = false;
    std::atomic<bool> m_failed{false};
    s_statistics m_statistics{};
    std::thread m_thread;
};
} // namespace ee
