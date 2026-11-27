#pragma once

// Host-only interrupt eligibility; keep guest IR inputs unchanged.
#include "ee_exceptions.h"

namespace ee
{

// EE Core manual pp. 74–75: only IP2, IP3 and IP7 exist. The legacy
// public enum retains software line names for source compatibility, but
// those lines cannot become EE interrupt sources.
inline constexpr uint32_t k_ee_hardware_ip_mask = 0x00008c00u;

inline constexpr bool ee_is_hardware_interrupt_line(uint32_t line)
{
    return line == INT_INTC || line == INT_DMAC || line == INT_TIMER;
}

inline bool ee_interrupts_enabled_inline(const s_ee_cpu_state* cpu)
{
    if (cpu == nullptr)
    {
        return false;
    }

    constexpr uint32_t required = STATUS_IE | STATUS_EIE;
    constexpr uint32_t control = required | STATUS_EXL | STATUS_ERL;
    const uint32_t status = cpu->m_cop0[COP0_Status];
    return (status & control) == required &&
        (cpu->m_cop0[COP0_Cause] & status & k_ee_hardware_ip_mask) != 0u;
}

inline uint32_t ee_check_interrupts_inline(s_ee_cpu_state* cpu,
    s_ee_runtime* runtime, uint32_t pc, uint32_t in_bd_slot)
{
    return ee_interrupts_enabled_inline(cpu)
        ? ee_raise_exception(cpu, runtime, EX_Int, pc, in_bd_slot, 0u)
        : 0u;
}

} // namespace ee
