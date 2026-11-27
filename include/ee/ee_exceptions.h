#pragma once
#include "ee_state.h"
#include "ee_runtime.h"
#include <cstdint>

namespace ee {

// R5900 / MIPS COP0 register indices.
enum e_ee_cop0_reg : uint32_t
{
    COP0_Index    = 0,
    COP0_Random   = 1,
    COP0_EntryLo0 = 2,
    COP0_EntryLo1 = 3,
    COP0_Context  = 4,
    COP0_PageMask = 5,
    COP0_Wired    = 6,
    COP0_BadVAddr = 8,
    COP0_Count    = 9,
    COP0_EntryHi  = 10,
    COP0_Compare  = 11,
    COP0_Status   = 12,
    COP0_Cause    = 13,
    COP0_EPC      = 14,
    COP0_PRId     = 15,
    COP0_Config   = 16,
    COP0_ErrorEPC = 30,
};

// Exception codes in Cause.ExcCode (bits 6..2).
enum e_ee_excode : uint32_t
{
    EX_Int  = 0,   // interrupt
    EX_Mod  = 1,   // TLB modified
    EX_TLBL = 2,   // TLB load/fetch
    EX_TLBS = 3,   // TLB store
    EX_AdEL = 4,   // address error load/fetch
    EX_AdES = 5,   // address error store
    EX_IBE  = 6,   // bus error (instruction)
    EX_DBE  = 7,   // bus error (data)
    EX_Sys  = 8,   // syscall
    EX_Bp   = 9,   // breakpoint
    EX_RI   = 10,  // reserved instruction
    EX_CpU  = 11,  // coprocessor unusable
    EX_Ov   = 12,  // overflow
    EX_Tr   = 13,  // trap
    EX_CODE_COUNT = 14
};

// Which vector class an exception uses.
enum e_ee_vector_kind : uint32_t
{
    VEC_KIND_COMMON = 0,   // general exception handler
    VEC_KIND_TLB    = 1,   // TLB refill (only if !EXL)
    VEC_KIND_DEBUG  = 2,
};

// One row of the architectural exception table.
struct s_ee_exception_info
{
    uint32_t     code;           // e_ee_excode
    const char*  name;           // "Sys", "Int", ...
    e_ee_vector_kind vector_kind;
    bool         sets_badvaddr;  // writes BadVAddr
    bool         uses_error_epc; // NMI / cache error style (not used yet)
};

// Full table indexed by ExcCode (0 .. EX_CODE_COUNT-1).
const s_ee_exception_info* ee_exception_table();
const s_ee_exception_info* ee_exception_info(uint32_t excode);

// Host-only read-only preflight for a validated typed-event producer.
// Never commits a counter, enters an exception or advances a clock.
s_ee_perf_crossing_entry_plan ee_cop0_perf_preflight_crossing(
    const s_ee_cpu_state& cpu, const s_ee_runtime& rt,
    const s_ee_perf_crossing_request& request);

// The caller must own the EE execution thread, account preceding device
// and clock work, and restore all architectural side effects to the
// actual restart PC (branch PC for an ordinary delay-slot cancellation).
// Matching PC/exit reason is necessary, not proof of this restoration.
inline constexpr bool ee_perf_crossing_commit_boundary_matches(
    uint32_t pc, uint32_t exit_reason, const s_ee_perf_crossing_entry_plan& plan)
{
    return plan.m_result == e_ee_perf_crossing_entry_result::READY &&
           exit_reason == static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH) &&
           pc == plan.m_entry.m_entry.m_error_epc;
}

// Host-only transaction for an independently established precise boundary.
// Recompute against live state, never accept a cached plan. COMMITTED means
// both PCRs and Level 2 entry state changed; every other result is read-only.
// No production caller is installed. Do not call from compatibility timing.
e_ee_perf_crossing_entry_result ee_cop0_perf_commit_crossing(
    s_ee_cpu_state& cpu, s_ee_runtime& rt,
    const s_ee_perf_crossing_request& request);

// Status bits (R5900 subset).
constexpr uint32_t STATUS_IE   = 1u << 0;
constexpr uint32_t STATUS_EXL  = 1u << 1;
constexpr uint32_t STATUS_ERL  = 1u << 2;
constexpr uint32_t STATUS_EIE  = 1u << 16; // EE interrupt enable
constexpr uint32_t STATUS_BEV  = 1u << 22;
// Interrupt mask in Status bits 15..8 (IM7..IM0).
constexpr uint32_t STATUS_IM_MASK = 0x0000FF00u;

// Cause bits.
constexpr uint32_t CAUSE_BD       = 1u << 31;
constexpr uint32_t CAUSE_CE_SHIFT = 28;
constexpr uint32_t CAUSE_IP_MASK  = 0x0000FF00u; // pending IP7..IP0
constexpr uint32_t CAUSE_IP_SHIFT = 8;

// Software / hardware interrupt lines (Cause.IP / Status.IM bit index 0..7).
enum e_ee_int_line : uint32_t
{
    INT_SW0    = 0,  // software interrupt 0
    INT_SW1    = 1,  // software interrupt 1
    INT_INTC   = 2,  // INTC (devices) — conventional mapping for host
    INT_DMAC   = 3,  // DMAC
    INT_TIMER  = 7,  // timer (Count == Compare) — MIPS IP7
};

// Standard R5900 exception vectors.
constexpr uint32_t VEC_TLB_REFILL_BEV0 = 0x80000000u;
constexpr uint32_t VEC_COMMON_BEV0     = 0x80000180u;
constexpr uint32_t VEC_TLB_REFILL_BEV1 = 0xBFC00200u;
constexpr uint32_t VEC_COMMON_BEV1     = 0xBFC00380u;
constexpr uint32_t VEC_DEBUG          = 0xBFC00480u;

#ifdef __cplusplus
extern "C" {
#endif

// Resolve vector address for an exception given Status and ExcCode.
uint32_t ee_exception_vector(uint32_t status, uint32_t excode);

// Perform full exception entry into `cpu`. Returns the vector PC written.
uint32_t ee_raise_exception(s_ee_cpu_state* cpu,
                            s_ee_runtime* rt,
                            uint32_t excode,
                            uint32_t pc,
                            uint32_t in_bd_slot,
                            uint32_t badvaddr);

// Exception return: clear EXL, PC <- EPC, exit DISPATCH.
uint32_t ee_exception_return(s_ee_cpu_state* cpu);

// ---- Interrupt handling --------------------------------------------------

// Set or clear a pending interrupt line (0..7) in Cause.IP.
void ee_interrupt_request(s_ee_cpu_state* cpu, uint32_t line);
void ee_interrupt_clear(s_ee_cpu_state* cpu, uint32_t line);

// True if interrupts are architecturally unmasked right now:
//   Status.IE && Status.EIE && !EXL && !ERL && (Cause.IP & Status.IM) != 0
bool ee_interrupts_enabled(const s_ee_cpu_state* cpu);
uint32_t ee_pending_interrupts(const s_ee_cpu_state* cpu); // Cause.IP & Status.IM

// If an interrupt is pending and enabled, raise EX_Int at `pc`.
// Returns vector PC if taken, 0 if no interrupt delivered.
uint32_t ee_check_interrupts(s_ee_cpu_state* cpu,
                             s_ee_runtime* rt,
                             uint32_t pc,
                             uint32_t in_bd_slot);

// Advance COP0 Count by `cycles`. If Count wraps past Compare, assert IP7.
void ee_timer_tick(s_ee_cpu_state* cpu, uint32_t cycles);

#ifdef __cplusplus
}
#endif

} // namespace ee
