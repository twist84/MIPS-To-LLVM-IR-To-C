#pragma once

#include "ee_runtime.h"
#include "ee_iop_clock.h"

#include <array>
#include <cstdint>
#include <limits>

namespace ee
{
// A deadline is a guest timestamp, not a host time estimate. UINT64_MAX
// is reserved for the absence of a representable future event.
constexpr uint64_t k_ee_no_deadline = std::numeric_limits<uint64_t>::max();

enum class e_ee_horizon_source : uint8_t
{
    barrier,
    timer,
    scanout,
    cop0,
    iop,
    none,
    count
};

struct s_ee_execution_horizon
{
    uint64_t m_cycle = k_ee_no_deadline;
    e_ee_horizon_source m_source = e_ee_horizon_source::none;
};

// Endpoint observation of scheduler-relevant COP0 state. This also
// detects an in-block Count synchronization that moves the clock anchor.
// It cannot detect a write restored before return, an access that leaves
// identical state, or other COP0 registers; it is not a pre-access guard.
using t_ee_horizon_cop0_snapshot = std::array<uint64_t, 13>;

inline t_ee_horizon_cop0_snapshot ee_horizon_cop0_snapshot(
    const s_ee_cpu_state& cpu, const s_ee_runtime& runtime)
{
    return {{
        cpu.m_cop0[COP0_Status],
        cpu.m_cop0[COP0_Cause],
        cpu.m_cop0[COP0_Count],
        cpu.m_cop0[COP0_Compare],
        cpu.m_cop0[COP0_EPC],
        cpu.m_cop0[COP0_ErrorEPC],
        runtime.m_cop0_clock_last,
        runtime.m_cop0_clock_count,
        runtime.m_cop0_clock_compare,
        runtime.m_cop0_clock_initialized,
        runtime.m_cop0_clock_ip7,
        runtime.m_perf_control,
        runtime.m_perf_compat_fault
    }};
}

// Count advances modulo 2^32 in the existing compatibility model.
// IP7 is sticky; pending delivery is handled by the barrier check.
// Equality at the anchor means the next full wrap, not an immediate edge.
inline uint64_t ee_cop0_compare_deadline(const s_ee_runtime& runtime,
                                        uint64_t now)
{
    if (!runtime.m_cop0_clock_initialized ||
        runtime.m_ee_cycle_scheduler.source() == e_ee_cycle_source::UNAVAILABLE ||
        runtime.m_cop0_clock_last > now)
    {
        return now; // No trustworthy Count anchor: require service.
    }
    if (runtime.m_cop0_clock_ip7)
    {
        return k_ee_no_deadline;
    }
    const uint32_t distance = runtime.m_cop0_clock_compare -
        runtime.m_cop0_clock_count;
    const uint64_t until = distance != 0u ? distance : (uint64_t{1u} << 32u);
    const uint64_t elapsed = now - runtime.m_cop0_clock_last;
    if (elapsed >= until)
    {
        return now;
    }
    const uint64_t remaining = until - elapsed;
    return remaining < k_ee_no_deadline - now ? now + remaining : now;
}

// This is a bound on known scheduled events, not proof that arbitrary
// translated code cannot perform MMIO, take a trap, or change a deadline.
// A caller must independently establish that execution is safe to chain.
inline s_ee_execution_horizon ee_execution_horizon(
    uint64_t now, uint64_t timer, uint64_t scanout, uint64_t cop0,
    uint64_t iop, bool barrier)
{
    if (barrier)
    {
        return {now, e_ee_horizon_source::barrier};
    }
    s_ee_execution_horizon result{};
    const uint64_t deadlines[] = {timer, scanout, cop0, iop};
    constexpr e_ee_horizon_source sources[] = {
        e_ee_horizon_source::timer, e_ee_horizon_source::scanout,
        e_ee_horizon_source::cop0, e_ee_horizon_source::iop
    };
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        if (deadlines[i] < result.m_cycle)
        {
            result.m_cycle = deadlines[i];
            result.m_source = sources[i];
        }
    }
    if (result.m_cycle < now)
    {
        result.m_cycle = now;
    }
    return result;
}
} // namespace ee
