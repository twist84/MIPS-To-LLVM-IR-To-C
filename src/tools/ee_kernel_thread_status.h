#pragma once

#include <cstdint>

// PS2 EE thread status transitions, independent of host scheduling.
// ps2sdk/kernel.h: RUN=1, READY=2, WAIT=4, SUSPEND=8,
// WAITSUSPEND=12, DORMANT=16. Do not introduce a synthetic
// scheduling event during SuspendThread: the retail EE kernel does not
// force one, even when suspending its currently running thread.
namespace ee_kernel_thread
{

constexpr uint32_t k_run = 1u;
constexpr uint32_t k_ready = 2u;
constexpr uint32_t k_wait = 4u;
constexpr uint32_t k_suspend = 8u;
constexpr uint32_t k_wait_suspend = 12u;
constexpr uint32_t k_dormant = 16u;

struct s_status_transition
{
    uint32_t m_status = 0u;
    bool m_changed = false;
};

inline s_status_transition suspend(uint32_t current_status)
{
    s_status_transition result{current_status, false};
    if (current_status == k_run || current_status == k_ready)
    {
        result.m_status = k_suspend;
        result.m_changed = true;
    }
    else if (current_status == k_wait)
    {
        result.m_status = k_wait_suspend;
        result.m_changed = true;
    }
    return result;
}

inline s_status_transition resume(
    uint32_t current_status, bool is_current_thread)
{
    s_status_transition result{current_status, false};
    if (current_status == k_suspend)
    {
        result.m_status = is_current_thread ? k_run : k_ready;
        result.m_changed = true;
    }
    else if (current_status == k_wait_suspend)
    {
        result.m_status = k_wait;
        result.m_changed = true;
    }
    return result;
}

inline bool can_delete(uint32_t status, bool is_current_thread)
{
    return !is_current_thread && status == k_dormant;
}

inline bool can_terminate(uint32_t status, bool is_current_thread)
{
    return !is_current_thread && (status == k_ready || status == k_wait ||
        status == k_suspend || status == k_wait_suspend);
}

inline bool can_wakeup(uint32_t status, bool is_current_thread)
{
    return !is_current_thread && (status == k_run || status == k_ready ||
        status == k_wait || status == k_suspend || status == k_wait_suspend);
}

inline s_status_transition release_wait(uint32_t current_status)
{
    s_status_transition result{current_status, false};
    if (current_status == k_wait)
    {
        result.m_status = k_ready;
        result.m_changed = true;
    }
    else if (current_status == k_wait_suspend)
    {
        result.m_status = k_suspend;
        result.m_changed = true;
    }
    return result;
}

} // namespace ee_kernel_thread
