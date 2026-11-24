#include "ee_vu1_worker.h"
#include "ee_graphics_batch.h"

#include <chrono>
#include <utility>

namespace ee
{
c_ee_vu1_worker::c_ee_vu1_worker(std::unique_ptr<c_ee_vu1_aot> backend)
    : m_backend(std::move(backend)),
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

bool c_ee_vu1_worker::submit(std::unique_ptr<s_ee_vu1_job>& job)
{
    bool submitted = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stopping && !m_busy && job != nullptr &&
            (job->m_path1.size() & 15u) == 0u &&
            job->m_path1.size() <= static_cast<size_t>(k_max_graphics_batch_qwc) * 16u)
        {
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
    }
    return result;
}

std::unique_ptr<s_ee_vu1_job> c_ee_vu1_worker::try_take()
{
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

void c_ee_vu1_worker::run()
{
    c_vu1_aot_binding binding(m_backend.get());
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
        const auto begin = std::chrono::steady_clock::now();
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
            // A host allocation/IO failure cannot become a successful
            // guest DMA or kill the process from the worker thread.
            job->m_accepted = false;
            job->m_host_failed = true;
        }
        job->m_execution_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_finished = std::move(job);
        }
        m_changed.notify_all();
    }
}
} // namespace ee
