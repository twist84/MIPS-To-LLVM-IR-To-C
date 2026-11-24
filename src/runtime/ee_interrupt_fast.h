#pragma once

// Host-only interrupt eligibility; keep guest IR inputs unchanged.
#include "ee_exceptions.h"

namespace ee
{

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
        (cpu->m_cop0[COP0_Cause] & status & CAUSE_IP_MASK) != 0u;
}

inline uint32_t ee_check_interrupts_inline(s_ee_cpu_state* cpu,
    s_ee_runtime* runtime, uint32_t pc, uint32_t in_bd_slot)
{
    return ee_interrupts_enabled_inline(cpu)
        ? ee_raise_exception(cpu, runtime, EX_Int, pc, in_bd_slot, 0u)
        : 0u;
}

} // namespace ee
