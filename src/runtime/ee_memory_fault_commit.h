#pragma once

#include "ee_memory_fault.h"

namespace ee
{
enum class e_ee_memory_request_kind : uint8_t
{
    UNAVAILABLE,
    DATA,
    PARTIAL,
    FETCH
};

struct s_ee_memory_fault_request
{
    e_ee_memory_request_kind m_kind = e_ee_memory_request_kind::UNAVAILABLE;
    e_ee_data_access_kind m_data_kind = e_ee_data_access_kind::BYTE;
    e_ee_partial_access_kind m_partial_kind = e_ee_partial_access_kind::WORD_LEFT;
    uint32_t m_address = 0u;
    uint32_t m_faulting_pc = 0u;
    uint32_t m_branch_pc = 0u;
    bool m_write_access = false;
    bool m_in_delay_slot = false;
    // Explicit caller evidence, not something the PC comparison proves.
    bool m_architectural_state_restored = false;
};

enum class e_ee_memory_fault_commit_result : uint8_t
{
    INVALID_REQUEST,
    UNSAFE_BOUNDARY,
    UNMODELED_LEVEL,
    NO_MODELED_FAULT,
    COMMITTED
};

// Caller owns the EE execution thread, has accounted preceding clock and
// device work, and has prevented/restored all effects of the faulting
// instruction (including a branch's effects if it must be restarted).
// The TLB must be the current table. This is not a rollback mechanism.
inline constexpr e_ee_memory_fault_commit_result ee_commit_memory_fault_at_boundary(
    s_ee_cpu_state& cpu, bool& actual_delay_slot, const c_ee_tlb& tlb,
    const s_ee_memory_fault_request& request)
{
    using kind = e_ee_memory_request_kind;
    using result = e_ee_memory_fault_commit_result;
    const bool fetch = request.m_kind == kind::FETCH;
    if ((request.m_kind != kind::DATA && request.m_kind != kind::PARTIAL && !fetch) ||
        (fetch && (request.m_write_access || request.m_address != request.m_faulting_pc)) ||
        (!fetch && (request.m_faulting_pc & 3u) != 0u))
    {
        return result::INVALID_REQUEST;
    }

    // A misaligned ordinary fetch is itself a modeled address error.
    // A delay slot must instead have an aligned, adjacent branch PC.
    if (!request.m_architectural_state_restored ||
        cpu.m_exit_reason != static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH) ||
        (request.m_in_delay_slot &&
         ((request.m_branch_pc & 3u) != 0u ||
          request.m_branch_pc + 4u != request.m_faulting_pc)) ||
        (!request.m_in_delay_slot && request.m_branch_pc != 0u) ||
        cpu.m_pc != (request.m_in_delay_slot ? request.m_branch_pc : request.m_faulting_pc))
    {
        return result::UNSAFE_BOUNDARY;
    }

    const uint32_t status = cpu.m_cop0[COP0_Status];
    // Do not invent priority/restart behavior while a Level 2 handler
    // is active. Ordinary nested Level 1 (EXL) entry is supported.
    if ((status & STATUS_ERL) != 0u)
    {
        return result::UNMODELED_LEVEL;
    }
    const uint32_t config = cpu.m_cop0[COP0_Config];
    const uint32_t context = cpu.m_cop0[COP0_Context];
    const uint32_t entry_hi = cpu.m_cop0[COP0_EntryHi];
    s_ee_memory_fault fault{};
    switch (request.m_kind)
    {
    case kind::DATA:
        fault = ee_plan_data_access(tlb, request.m_data_kind, request.m_write_access,
            request.m_address, status, config, context, entry_hi).m_fault;
        break;
    case kind::PARTIAL:
        fault = ee_plan_partial_access(tlb, request.m_partial_kind, request.m_write_access,
            request.m_address, status, config, context, entry_hi).m_fault;
        break;
    case kind::FETCH:
        fault = ee_plan_instruction_fetch(tlb, request.m_address, status,
            config, context, entry_hi).m_fault;
        break;
    default:
        break;
    }
    if (!fault.m_valid)
    {
        return result::NO_MODELED_FAULT;
    }

    // Commit the freshly computed plan, not a cached vector or fault.
    // Unlike the legacy code-only vector helper, TLBL/TLBS distinguish
    // invalid matched entries from misses. Preserve pending hardware
    // interrupts and independent Level 2 Cause bits as in Level 1 entry.
    constexpr uint32_t level2_cause = (1u << 30u) | (7u << 16u);
    const uint32_t old_cause = cpu.m_cop0[COP0_Cause];
    uint32_t cause = (old_cause & (CAUSE_IP_MASK | level2_cause)) |
        ((fault.m_code & 0x1fu) << 2u);
    if ((status & STATUS_EXL) != 0u)
    {
        cause |= old_cause & CAUSE_BD;
    }
    else
    {
        if (request.m_in_delay_slot)
        {
            cause |= CAUSE_BD;
        }
        cpu.m_cop0[COP0_EPC] = cpu.m_pc;
    }
    cpu.m_cop0[COP0_Cause] = cause;
    cpu.m_cop0[COP0_BadVAddr] = fault.m_bad_vaddr;
    if (fault.m_update_tlb_registers)
    {
        cpu.m_cop0[COP0_Context] = fault.m_context;
        cpu.m_cop0[COP0_EntryHi] = fault.m_entry_hi;
    }
    cpu.m_cop0[COP0_Status] = status | STATUS_EXL;
    cpu.m_pad = request.m_faulting_pc;
    actual_delay_slot = request.m_in_delay_slot;
    cpu.m_pc = fault.m_vector;
    cpu.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_EXCEPTION);
    return result::COMMITTED;
}

// Host-only opt-in entry point. It does not synchronize clocks, access
// memory, dispatch a handler or decide whether the front end may resume.
// Caller must establish all prerequisites before invoking it.
inline e_ee_memory_fault_commit_result ee_commit_strict_memory_fault(
    s_ee_cpu_state& cpu, s_ee_runtime& runtime, const c_ee_tlb& tlb,
    const s_ee_memory_fault_request& request)
{
    return ee_commit_memory_fault_at_boundary(cpu,
        runtime.m_last_exception_in_bd_slot, tlb, request);
}

} // namespace ee
