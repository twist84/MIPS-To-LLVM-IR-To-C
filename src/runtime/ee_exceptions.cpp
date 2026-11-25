#include "ee_interrupt_fast.h"

namespace ee {

namespace {

const s_ee_exception_info k_exception_table[EX_CODE_COUNT] = {
    // Legacy common-vector class describes the nested fallback.
    // Normal interrupt entry is selected separately below.
    { EX_Int,  "Int",  VEC_KIND_COMMON, false, false },
    { EX_Mod,  "Mod",  VEC_KIND_COMMON, true,  false },
    { EX_TLBL, "TLBL", VEC_KIND_TLB,    true,  false },
    { EX_TLBS, "TLBS", VEC_KIND_TLB,    true,  false },
    { EX_AdEL, "AdEL", VEC_KIND_COMMON, true,  false },
    { EX_AdES, "AdES", VEC_KIND_COMMON, true,  false },
    { EX_IBE,  "IBE",  VEC_KIND_COMMON, false, false },
    { EX_DBE,  "DBE",  VEC_KIND_COMMON, false, false },
    { EX_Sys,  "Sys",  VEC_KIND_COMMON, false, false },
    { EX_Bp,   "Bp",   VEC_KIND_COMMON, false, false },
    { EX_RI,   "RI",   VEC_KIND_COMMON, false, false },
    { EX_CpU,  "CpU",  VEC_KIND_COMMON, false, false },
    { EX_Ov,   "Ov",   VEC_KIND_COMMON, false, false },
    { EX_Tr,   "Tr",   VEC_KIND_COMMON, false, false },
};

} // namespace

const s_ee_exception_info* ee_exception_table()
{
    return k_exception_table;
}

const s_ee_exception_info* ee_exception_info(uint32_t excode)
{
    if (excode >= EX_CODE_COUNT)
        return nullptr;
    return &k_exception_table[excode];
}

extern "C" uint32_t ee_exception_vector(uint32_t status, uint32_t excode)
{
    const bool bev = (status & STATUS_BEV) != 0;
    const s_ee_exception_info* info = ee_exception_info(excode);
    const e_ee_vector_kind kind =
        info ? info->vector_kind : VEC_KIND_COMMON;

    if (kind == VEC_KIND_DEBUG)
        return VEC_DEBUG;

    // EE Core User's Manual v6.0, section 4.2.5 (printed p.99):
    // level-1 hardware interrupts have their own vector, not V_COMMON.
    // The interrupt eligibility helper already masks EXL and ERL.
    // Preserve the common-vector fallback for explicit nested entry.
    if (excode == EX_Int && (status & STATUS_EXL) == 0u)
    {
        return bev ? 0xBFC00400u : 0x80000200u;
    }

    // TLB refill vector only when not already in exception level.
    if (kind == VEC_KIND_TLB && (status & STATUS_EXL) == 0)
        return bev ? VEC_TLB_REFILL_BEV1 : VEC_TLB_REFILL_BEV0;

    return bev ? VEC_COMMON_BEV1 : VEC_COMMON_BEV0;
}

extern "C" uint32_t ee_raise_exception(s_ee_cpu_state* cpu,
                                       s_ee_runtime* /*rt*/,
                                       uint32_t excode,
                                       uint32_t pc,
                                       uint32_t in_bd_slot,
                                       uint32_t badvaddr)
{
    if (!cpu) return 0;

    uint32_t& status = cpu->m_cop0[COP0_Status];
    uint32_t& cause  = cpu->m_cop0[COP0_Cause];
    uint32_t& epc    = cpu->m_cop0[COP0_EPC];
    // Preserve the actual trapping instruction PC for the host HLE kernel.
    // Architectural EPC must remain unchanged when Status.EXL is already
    // set (e.g. a syscall within a genuine DMA5 guest callback). Reuse the
    // previously unused CPU state pad; the guest cannot observe this field.
    cpu->m_pad = pc;
    // Vector selection depends on the exception level *before* this entry.
    // In particular, the first TLBL/TLBS must use the TLB refill vector.
    const uint32_t pre_entry_status = status;

    const s_ee_exception_info* info = ee_exception_info(excode);

    // Cause: preserve IP bits (15..8), set ExcCode and BD.
    uint32_t new_cause = cause & CAUSE_IP_MASK;
    new_cause |= (excode & 0x1fu) << 2;
    if (in_bd_slot)
        new_cause |= CAUSE_BD;
    else
        new_cause &= ~CAUSE_BD;
    cause = new_cause;

    if (info && info->sets_badvaddr)
        cpu->m_cop0[COP0_BadVAddr] = badvaddr;

    // EPC only updated when not already in exception level.
    if ((status & STATUS_EXL) == 0)
    {
        if (in_bd_slot)
            epc = pc - 4;
        else
            epc = pc;
    }

    const uint32_t vec = ee_exception_vector(pre_entry_status, excode);
    status |= STATUS_EXL;
    cpu->m_pc = vec;
    cpu->m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_EXCEPTION);
    return vec;
}

extern "C" uint32_t ee_exception_return(s_ee_cpu_state* cpu)
{
    if (!cpu) return 0;

    uint32_t& status = cpu->m_cop0[COP0_Status];
    uint32_t resume_pc = 0u;

    // R5900 ERET prioritizes ErrorEPC when ERL is set. Clearing EXL
    // unconditionally would instead resume at the previous exception
    // (often a syscall) and skip the guest's explicit ErrorEPC return.
    if ((status & STATUS_ERL) != 0u)
    {
        resume_pc = cpu->m_cop0[COP0_ErrorEPC];
        status &= ~STATUS_ERL;
    }
    else
    {
        resume_pc = cpu->m_cop0[COP0_EPC];
        status &= ~STATUS_EXL;
    }

    cpu->m_pc = resume_pc;
    cpu->m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);
    return resume_pc;
}

// ---- Guest COP0 writes -----------------------------------------------------
//
// EE Core User's Manual v6.0, section 3.2, Compare (printed p.72):
// writing Compare acknowledges the timer interrupt by clearing Cause.IP7.
// This action is independent of a Count tick or a scheduler deadline.
// Until individually audited, all other guest-writable COP0 registers keep
// their prior raw assignment semantics instead of inventing bit masks.
extern "C" void ee_cop0_write(s_ee_cpu_state* cpu,
                              uint32_t reg,
                              uint32_t value)
{
    if (!cpu || reg >= 32u)
    {
        return;
    }

    cpu->m_cop0[reg] = value;

    if (reg == COP0_Compare)
    {
        ee_interrupt_clear(cpu, INT_TIMER);
    }
}

// ---- Interrupts ----------------------------------------------------------

extern "C" void ee_interrupt_request(s_ee_cpu_state* cpu, uint32_t line)
{
    if (!cpu || line > 7) return;
    cpu->m_cop0[COP0_Cause] |= (1u << (CAUSE_IP_SHIFT + line));
}

extern "C" void ee_interrupt_clear(s_ee_cpu_state* cpu, uint32_t line)
{
    if (!cpu || line > 7) return;
    cpu->m_cop0[COP0_Cause] &= ~(1u << (CAUSE_IP_SHIFT + line));
}

extern "C" bool ee_interrupts_enabled(const s_ee_cpu_state* cpu)
{
    return ee_interrupts_enabled_inline(cpu);
}

extern "C" uint32_t ee_pending_interrupts(const s_ee_cpu_state* cpu)
{
    if (!cpu) return 0;
    const uint32_t status = cpu->m_cop0[COP0_Status];
    const uint32_t cause  = cpu->m_cop0[COP0_Cause];
    return (cause & status & CAUSE_IP_MASK);
}

extern "C" uint32_t ee_check_interrupts(s_ee_cpu_state* cpu,
                                        s_ee_runtime* rt,
                                        uint32_t pc,
                                        uint32_t in_bd_slot)
{
    return ee_check_interrupts_inline(cpu, rt, pc, in_bd_slot);
}

extern "C" void ee_timer_tick(s_ee_cpu_state* cpu, uint32_t cycles)
{
    if (!cpu || cycles == 0) return;

    uint32_t& count   = cpu->m_cop0[COP0_Count];
    const uint32_t compare = cpu->m_cop0[COP0_Compare];

    // Detect Count crossing Compare (unsigned wrap-safe).
    const uint32_t before = count;
    count = before + cycles;
    // Interrupt when the interval [before, before+cycles) contains Compare.
    // Equivalent: (compare - before) <= cycles in modular arithmetic when
    // cycles is small; for general cycles use:
    if (cycles >= 0xFFFFFFFFu) {
        ee_interrupt_request(cpu, INT_TIMER);
        return;
    }
    const uint32_t dist = compare - before; // mod 2^32
    if (dist != 0 && dist <= cycles)
        ee_interrupt_request(cpu, INT_TIMER);
    // Also if Compare == Count after add (exact match).
    if (count == compare)
        ee_interrupt_request(cpu, INT_TIMER);
}

} // namespace ee
