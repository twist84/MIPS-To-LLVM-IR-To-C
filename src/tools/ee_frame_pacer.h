#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ee
{
// Host-only absolute steady-clock pacing. No guest cycles or audio state.
class c_ee_frame_pacer
{
public:
    using t_clock = std::chrono::steady_clock;

    explicit c_ee_frame_pacer(bool enabled)
    {
#if defined(_WIN32)
        if (enabled)
        {
            // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+).
            // Older systems may reject the flag; use the portable fallback.
            m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002u,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
#else
        (void)enabled;
#endif
    }

    ~c_ee_frame_pacer()
    {
#if defined(_WIN32)
        if (m_timer != nullptr)
        {
            CloseHandle(m_timer);
        }
#endif
    }

    c_ee_frame_pacer(const c_ee_frame_pacer&) = delete;
    c_ee_frame_pacer& operator=(const c_ee_frame_pacer&) = delete;

    void wait_until(t_clock::time_point deadline)
    {
        const auto start = t_clock::now();
        if (deadline <= start)
        {
            return;
        }
        ++m_calls;
        m_requested_ns += ns(deadline - start);
        bool waited = false;
        constexpr auto k_spin_window = std::chrono::microseconds(500);
        const auto sleep_deadline = deadline - k_spin_window;
#if defined(_WIN32)
        if (m_timer != nullptr)
        {
            using t_ticks = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
            // Negative values are relative 100 ns units; round up, not down.
            LARGE_INTEGER due{};
            const auto timer_deadline = std::max(start, sleep_deadline);
            due.QuadPart = -std::chrono::ceil<t_ticks>(timer_deadline - start).count();
            if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE) &&
                WaitForSingleObject(m_timer, INFINITE) == WAIT_OBJECT_0)
            {
                ++m_high_res_calls;
                waited = true;
            }
            else
            {
                // Avoid repeatedly attempting a failed timer.
                CloseHandle(m_timer);
                m_timer = nullptr;
                ++m_timer_failures;
            }
        }
#endif
        if (!waited || t_clock::now() < sleep_deadline)
        {
            ++m_fallback_calls;
            if (sleep_deadline > t_clock::now())
            {
                std::this_thread::sleep_until(sleep_deadline);
            }
        }
        // General-purpose OS sleeps can overshoot a video deadline by a
        // scheduling quantum. Sleep most of the interval, then use a short
        // final spin to keep locked presentation close to the guest field
        // cadence (60 or 60000/1001 Hz). The caller skips this wait when
        // presentation is unlocked.
        while (t_clock::now() < deadline)
        {
        }
        const auto end = t_clock::now();
        m_waited_ns += ns(end - start);
        if (end > deadline)
        {
            const auto overshoot = ns(end - deadline);
            m_overshoot_ns += overshoot;
            m_max_overshoot_ns = std::max(m_max_overshoot_ns, overshoot);
        }
    }

    uint64_t calls() const { return m_calls; }
    uint64_t high_res_calls() const { return m_high_res_calls; }
    uint64_t fallback_calls() const { return m_fallback_calls; }
    uint64_t timer_failures() const { return m_timer_failures; }
    uint64_t requested_ns() const { return m_requested_ns; }
    uint64_t waited_ns() const { return m_waited_ns; }
    uint64_t overshoot_ns() const { return m_overshoot_ns; }
    uint64_t max_overshoot_ns() const { return m_max_overshoot_ns; }

private:
    static uint64_t ns(t_clock::duration duration)
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
    }

#if defined(_WIN32)
    HANDLE m_timer = nullptr;
#endif
    uint64_t m_calls = 0u;
    uint64_t m_high_res_calls = 0u;
    uint64_t m_fallback_calls = 0u;
    uint64_t m_timer_failures = 0u;
    uint64_t m_requested_ns = 0u;
    uint64_t m_waited_ns = 0u;
    uint64_t m_overshoot_ns = 0u;
    uint64_t m_max_overshoot_ns = 0u;
};
}
