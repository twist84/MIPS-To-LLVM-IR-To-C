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
                                       s_ee_runtime* rt,
                                       uint32_t excode,
                                       uint32_t pc,
                                       uint32_t in_bd_slot,
                                       uint32_t badvaddr)
{
    if (!cpu) return 0;

    // Charge the elapsed interval before entering exception level.
    ee_cop0_clock_sync(cpu, rt);
    uint32_t& status = cpu->m_cop0[COP0_Status];
    uint32_t& cause  = cpu->m_cop0[COP0_Cause];
    uint32_t& epc    = cpu->m_cop0[COP0_EPC];
    // Preserve the actual trapping instruction PC for the host HLE kernel.
    // Architectural EPC must remain unchanged when Status.EXL is already
    // set (e.g. a syscall within a genuine DMA5 guest callback). Reuse the
    // previously unused CPU state pad; the guest cannot observe this field.
    cpu->m_pad = pc;
    if (rt != nullptr)
    {
        rt->m_last_exception_in_bd_slot = in_bd_slot != 0u;
    }
    // Vector selection depends on the exception level *before* this entry.
    // In particular, the first TLBL/TLBS must use the TLB refill vector.
    const uint32_t pre_entry_status = status;

    const s_ee_exception_info* info = ee_exception_info(excode);

    // Manual section 4.1.2 pseudocode: nested Level 1 entry updates
    // ExcCode but must preserve the EPC/BD restart pair. BD2/EXC2
    // belong to Level 2 and are not overwritten by this entry.
    constexpr uint32_t level2_cause = (1u << 30u) | (7u << 16u);
    uint32_t new_cause = cause & (CAUSE_IP_MASK | level2_cause);
    new_cause |= (excode & 0x1fu) << 2;
    const bool nested = (pre_entry_status & STATUS_EXL) != 0u;
    if (nested)
    {
        new_cause |= cause & CAUSE_BD;
    }
    else if (in_bd_slot)
    {
        new_cause |= CAUSE_BD;
    }
    cause = new_cause;

    if (info && info->sets_badvaddr)
        cpu->m_cop0[COP0_BadVAddr] = badvaddr;

    // EPC only updated when not already in exception level.
    if (!nested)
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
// Cause is read-only (manual p. 75); guest writes cannot acknowledge or
// manufacture an interrupt or change the saved exception restart state.
// Other unaudited COP0 registers retain their existing assignment semantics.
extern "C" void ee_cop0_write(s_ee_cpu_state* cpu,
                              uint32_t reg,
                              uint32_t value)
{
    if (!cpu || reg >= 32u)
    {
        return;
    }

    // Section 3.2, pp. 64, 69, 75, 77: these registers are read-only.
    if (reg == COP0_Cause || reg == COP0_Random ||
        reg == COP0_BadVAddr || reg == COP0_PRId)
    {
        return;
    }
    cpu->m_cop0[reg] = value;

    if (reg == COP0_Compare)
    {
        ee_interrupt_clear(cpu, INT_TIMER);
    }
    else if (reg == COP0_Wired && value < 48u)
    {
        // A defined Wired write resets Random to the upper bound. Its
        // subsequent cycle-driven decrement remains unimplemented.
        cpu->m_cop0[COP0_Random] = 47u;
    }
}

namespace
{
bool perf_compat_sync(s_ee_cpu_state* cpu, s_ee_runtime* rt);
}

extern "C" void ee_cop0_clock_sync(s_ee_cpu_state* cpu, s_ee_runtime* rt)
{
    if (cpu == nullptr || rt == nullptr ||
        rt->m_ee_cycle_scheduler.source() == e_ee_cycle_source::UNAVAILABLE)
    {
        return;
    }
    uint64_t now = rt->m_ee_cycle_scheduler.current_cycle();
    if (rt->m_ee_cycle_scheduler.source() == e_ee_cycle_source::REFERENCE_COMPATIBILITY)
    {
        // Within a translated function the host's last publication can
        // lag the reached-instruction budget. Observe the same budget
        // directly at Count/Compare accesses, never host wall time.
        if (!ee_reference_compatibility_cycle(*rt, now))
        {
            rt->m_perf_compat_fault = true;
            return;
        }
        // This path does not turn the reference budget into a validated
        // hardware clock. Faults are observed by the runner/next transfer.
        (void)perf_compat_sync(cpu, rt);
    }
    if (now < rt->m_cop0_clock_last)
    {
        return;
    }
    constexpr uint32_t ip7 = 1u << 15u;
    if (!rt->m_cop0_clock_initialized)
    {
        rt->m_cop0_clock_count = cpu->m_cop0[COP0_Count];
        rt->m_cop0_clock_compare = cpu->m_cop0[COP0_Compare];
        rt->m_cop0_clock_ip7 = (cpu->m_cop0[COP0_Cause] & ip7) != 0u;
        rt->m_cop0_clock_initialized = true;
    }
    const uint64_t elapsed = now - rt->m_cop0_clock_last;
    const uint32_t distance = rt->m_cop0_clock_compare - rt->m_cop0_clock_count;
    // Count increments modulo 2^32. Equality at the starting point does
    // not raise a new interrupt until the next full wrap.
    if (elapsed >= (1ull << 32u) || (distance != 0u && elapsed >= distance))
    {
        rt->m_cop0_clock_ip7 = true;
    }
    rt->m_cop0_clock_count += static_cast<uint32_t>(elapsed);
    rt->m_cop0_clock_last = now;
    cpu->m_cop0[COP0_Count] = rt->m_cop0_clock_count;
    cpu->m_cop0[COP0_Compare] = rt->m_cop0_clock_compare;
    cpu->m_cop0[COP0_Cause] = (cpu->m_cop0[COP0_Cause] & ~ip7) |
        (rt->m_cop0_clock_ip7 ? ip7 : 0u);
}

extern "C" void ee_cop0_clock_write(s_ee_cpu_state* cpu, s_ee_runtime* rt,
                                     uint32_t reg, uint32_t value)
{
    ee_cop0_clock_sync(cpu, rt);
    ee_cop0_write(cpu, reg, value);
    if (cpu == nullptr || rt == nullptr || !rt->m_cop0_clock_initialized)
    {
        return;
    }
    if (reg == COP0_Count)
    {
        rt->m_cop0_clock_count = value;
    }
    else if (reg == COP0_Compare)
    {
        rt->m_cop0_clock_compare = value;
        rt->m_cop0_clock_ip7 = false;
    }
}

namespace
{
constexpr uint32_t k_perf_control_mask = 0x800ffbfeu;
constexpr uint32_t k_perf_cte = 0x80000000u;

bool perf_event_supported(uint32_t control, unsigned index, bool has_clock)
{
    const uint32_t modes = (control >> (1u + index * 10u)) & 15u;
    const uint32_t event = (control >> (5u + index * 10u)) & 31u;
    // EE Core User's Manual 7.2.2, table 7-1: event zero is
    // reserved only for PCR0; PCR1 selects low-order branch issue.
    // Never silently report zero for an active, unmodeled branch event.
    if ((control & k_perf_cte) == 0u || modes == 0u ||
        (event == 0u && index == 0u) || event >= 16u)
    {
        return true;
    }
    return event == 1u && has_clock;
}

// Common counter update with transactional overflow rejection. Only the
// processor-cycle event is implemented; no other events are synthesized.
bool perf_advance_interval(s_ee_runtime* rt, uint32_t status, uint64_t cycles);

bool perf_compat_sync(s_ee_cpu_state* cpu, s_ee_runtime* rt)
{
    if (rt->m_ee_cycle_scheduler.source() != e_ee_cycle_source::REFERENCE_COMPATIBILITY)
    {
        return true;
    }
    uint64_t now = 0u;
    if (rt->m_perf_compat_fault || !ee_reference_compatibility_cycle(*rt, now))
    {
        rt->m_perf_compat_fault = true;
        return false;
    }
    if (now < rt->m_perf_compat_last ||
        !perf_advance_interval(rt, cpu->m_cop0[COP0_Status],
                               now - rt->m_perf_compat_last))
    {
        rt->m_perf_compat_fault = true;
        return false;
    }
    rt->m_perf_compat_last = now;
    return true;
}
}

extern "C" uint32_t ee_cop0_perf_transfer(s_ee_cpu_state* cpu,
                                          s_ee_runtime* rt,
                                          uint32_t word)
{
    if (cpu == nullptr || rt == nullptr || (word >> 26u) != 0x10u ||
        ((word >> 11u) & 31u) != 25u || (word & 0x7c0u) != 0u)
    {
        return 4u;
    }
    if (!perf_compat_sync(cpu, rt))
    {
        return 4u;
    }
    const uint32_t fmt = (word >> 21u) & 31u;
    const uint32_t gpr = (word >> 16u) & 31u;
    const uint32_t selector = word & 63u;
    const bool counter = (selector & 1u) != 0u;
    const uint32_t index = (selector >> 1u) & 1u;
    if (fmt == 0u)
    {
        const uint32_t value = counter ? rt->m_perf_counter[index] : rt->m_perf_control;
        if (gpr != 0u)
        {
            cpu->m_gpr[gpr].m_lo = static_cast<uint64_t>(
                static_cast<int64_t>(static_cast<int32_t>(value)));
        }
        return 0u;
    }
    if (fmt != 4u)
    {
        return 4u;
    }
    const uint32_t value = gpr == 0u ? 0u :
        static_cast<uint32_t>(cpu->m_gpr[gpr].m_lo);
    if (counter)
    {
        rt->m_perf_counter[index] = value;
    }
    else if (selector == 0u)
    {
        const uint32_t control = value & k_perf_control_mask;
        const bool has_clock = rt->m_ee_cycle_scheduler.source() !=
            e_ee_cycle_source::UNAVAILABLE;
        if (!perf_event_supported(control, 0u, has_clock) ||
            !perf_event_supported(control, 1u, has_clock))
        {
            return 4u;
        }
        rt->m_perf_control = control;
    }
    // Nonzero MTPS selectors are architecturally ignored.
    return 0u;
}

namespace
{
bool perf_advance_interval(s_ee_runtime* rt, uint32_t status, uint64_t cycles)
{
    const uint32_t control = rt->m_perf_control;
    if ((control & k_perf_cte) == 0u || (status & STATUS_ERL) != 0u)
    {
        return true;
    }
    uint32_t next[2] = {rt->m_perf_counter[0], rt->m_perf_counter[1]};
    for (unsigned index = 0; index < 2; ++index)
    {
        if (!perf_event_supported(control, index, true))
        {
            return false;
        }
        const uint32_t shift = index * 10u;
        const uint32_t event = (control >> (5u + shift)) & 31u;
        const uint32_t ksu = (status >> 3u) & 3u;
        // EE Core User's Manual 7.1.1: K counts kernel mode
        // *except* while executing an exception handler. EXL is
        // selected instead of the saved KSU mode, never alongside it.
        const uint32_t mode_mask = (status & STATUS_EXL) != 0u ? 2u :
            (ksu < 3u ? (1u << (ksu + 2u)) : 0u);
        if (event == 1u && ((control >> shift) & mode_mask) != 0u && cycles != 0u)
        {
            // Level-2 performance overflow exception is not implemented.
            // Reject before either counter changes rather than lose it.
            if (next[index] >= 0x80000000u ||
                cycles >= 0x80000000ull - next[index])
            {
                return false;
            }
            next[index] += static_cast<uint32_t>(cycles);
        }
    }
    rt->m_perf_counter[0] = next[0];
    rt->m_perf_counter[1] = next[1];
    return true;
}
}

extern "C" bool ee_cop0_perf_advance(s_ee_runtime* rt,
                                     uint32_t status, uint64_t cycles)
{
    // Only a validated producer can supply externally authored deltas.
    // The compatibility path is restricted to its real existing budget.
    return rt != nullptr && rt->m_ee_cycle_scheduler.source() ==
        e_ee_cycle_source::VALIDATED_R5900 &&
        perf_advance_interval(rt, status, cycles);
}

// ---- Interrupts ----------------------------------------------------------

extern "C" void ee_interrupt_request(s_ee_cpu_state* cpu, uint32_t line)
{
    if (!cpu || !ee_is_hardware_interrupt_line(line)) return;
    cpu->m_cop0[COP0_Cause] |= (1u << (CAUSE_IP_SHIFT + line));
}

extern "C" void ee_interrupt_clear(s_ee_cpu_state* cpu, uint32_t line)
{
    if (!cpu || !ee_is_hardware_interrupt_line(line)) return;
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
    return (cause & status & k_ee_hardware_ip_mask);
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
