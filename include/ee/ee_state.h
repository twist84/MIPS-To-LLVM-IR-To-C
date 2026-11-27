#pragma once
#include "ee_instruction.h"
#include <cstdint>
#include <cstddef>

namespace ee {

struct alignas(16) s_ee_u128
{
    uint64_t m_lo;
    uint64_t m_hi;
};

// VU0 state not already represented by the EE CPU's m_vu0[32] VF register
// file. The VU scheduler is host-owned; this structure carries the architectural
// VI/ACC/Q/P and visible execution state that the scheduler must preserve.
struct alignas(16) s_ee_vu0_state
{
    uint32_t m_vi[32];             // VI integer/control register bank

    s_ee_u128 m_acc;               // VU0 vector accumulator
    s_ee_u128 m_q;                 // divide/EFU result pipeline register
    s_ee_u128 m_p;                 // EFU result pipeline register

    uint32_t m_mac_flags;
    uint32_t m_clip_flags;
    uint32_t m_status_flags;       // VI[STATUS_FLAG]
    uint32_t m_vpu_status;         // VI[VPU_STAT]
    uint32_t m_fbrst;
    uint32_t m_cmsar1;
    uint32_t m_tpc;                 // VI[TPC], measured in 8-byte instruction slots
    uint32_t m_micro_pc;            // byte offset used by the active micro fetch loop

    // Cycle accounting is maintained by the host scheduler. m_running and
    // the E/M bits are exposed so synchronization helpers can implement the
    // PCSX2-observed catch-up, finish-to-E, and wait-to-E-or-M policies.
    uint64_t m_cycle;
    uint64_t m_ee_cycle;

    uint32_t m_running;
    uint32_t m_e_bit;
    uint32_t m_m_bit;

    // VU0 has distinct 4 KB data and micro-program memories. The micro-engine
    // fetches paired 32-bit lower/upper words from m_micro_memory.
    alignas(16) uint8_t m_data_memory[0x1000];
    alignas(16) uint8_t m_micro_memory[0x1000];

    // Integer-ALU writes are visible in VI immediately in the interpreter,
    // but their destination registers remain interlocked for the instruction
    // latency. This four-entry ring tracks those reservations.
    struct s_ialu_pipe_entry
    {
        uint32_t m_register_mask;
        uint32_t m_latency;
        uint64_t m_start_cycle;
    };

    s_ialu_pipe_entry m_ialu[4];
    uint32_t m_ialu_read_pos;
    uint32_t m_ialu_write_pos;
    uint32_t m_ialu_count;

    // Interpreter-compatible lower-branch pipeline state. A branch issued
    // with m_branch=2 has one delay pair remaining; a branch in that delay
    // pair is held in m_delay_branch_pc until the first target is entered.
    uint32_t m_branch;
    uint32_t m_branch_pc;
    uint32_t m_delay_branch_pc;
    uint32_t m_take_delay_branch;

    // PCSX2's VIBackup window preserves the pre-write low halfword for two
    // cycles so a following branch can observe the value from before an IALU
    // write while the dependency pipeline is settling.
    uint32_t m_vi_backup_cycles;
    uint32_t m_vi_backup_register;
    uint32_t m_vi_backup_old_value;

    // PCSX2 issues lower LQ/SQ through its four-cycle FMAC pipeline,
    // including stores with no VF destination. Track the latest completion
    // for E-bit flush accounting in this conservative memory-only slice.
    uint64_t m_vf_memory_ready_cycle;

    // Four in-flight lower-memory FMAC issues. VF writes are immediately
    // visible, but subsequent reads of overlapping lanes must wait until
    // the writing issue's four-cycle reservation matures.
    struct s_vf_memory_issue
    {
        uint64_t m_ready_cycle;
        uint32_t m_register;
        uint32_t m_lane_mask;
    };

    s_vf_memory_issue m_vf_memory_issues[4];
    uint32_t m_vf_memory_issue_index;
    uint32_t m_acc_overflow_mask; // Per-lane ACC exception latch, in former alignment padding

    // Scalar Q pipeline is appended to preserve the earlier ABI prefix.
    // VU0 virtual cycles do not establish EE hardware cycle timing.
    uint64_t m_q_ready_cycle;
    uint32_t m_q_pending_bits;
    uint32_t m_q_pending;

    // Upper CLIP results become visible to lower flag reads four VU
    // cycles after issue. Keep the history of in-flight clip results.
    struct s_clip_issue
    {
        uint64_t m_ready_cycle;
        uint32_t m_value;
        uint32_t m_padding;
    };
    s_clip_issue m_clip_issues[4];

    // Upper VF dependencies and delayed MAC/status results, independently
    // of the lower half's four-entry vector-memory reservation ring.
    struct s_upper_fmac_issue
    {
        uint64_t m_ready_cycle;
        uint32_t m_register;
        uint32_t m_lane_mask;
        uint32_t m_mac;
        uint32_t m_product_sticky;
    };
    s_upper_fmac_issue m_upper_fmac_issues[4];
    // In former tail padding: micro FDIV flags retire with Q.
    uint32_t m_q_pending_flags;
};

static_assert(offsetof(s_ee_vu0_state, m_q_ready_cycle) == 8616);
static_assert(offsetof(s_ee_vu0_state, m_clip_issues) == 8632);
static_assert(offsetof(s_ee_vu0_state, m_upper_fmac_issues) == 8696);
static_assert(sizeof(s_ee_vu0_state) == 8800);

// Architectural EE CPU state. GPRs are 128 bits wide in the lifter (i128);
// the runtime stores them as two 64-bit halves. Scalar instructions may write
// only the low word, low doubleword, or the full quadword depending on the op.
struct alignas(16) s_ee_cpu_state
{
    s_ee_u128 m_gpr[32];

    uint64_t m_hi;
    uint64_t m_lo;
    uint64_t m_hi1;
    uint64_t m_lo1;

    uint32_t m_sa;
    uint32_t m_pc;

    uint32_t m_exit_reason;
    uint32_t m_pad;

    // Coprocessor register files.
    uint32_t m_cop0[32];   // COP0 system control registers (Status, Cause, EPC, ...)
    uint64_t m_fpr[32];    // COP1 floating-point registers (32-bit or 64-bit values)
    s_ee_u128 m_vu0[32];   // COP2 VU0 vector registers (128-bit each)
    uint32_t m_fcsr;       // COP1 floating-point control/status (condition bit in FCC)
    uint32_t m_fpu_acc;    // COP1 accumulator (ADDA/MULA/MADD family)
    uint32_t m_fpu_acc_overflow; // Independent overflow latch in existing tail padding
};

static_assert(offsetof(s_ee_cpu_state, m_fpu_acc_overflow) ==
              offsetof(s_ee_cpu_state, m_fpu_acc) + sizeof(uint32_t));
static_assert(sizeof(s_ee_cpu_state) == 1472);

enum class e_ee_exit_reason : uint32_t
{
    EE_EXIT_RETURN = 0,
    EE_EXIT_EXCEPTION = 1,
    EE_EXIT_EXTERNAL_CALL = 2,
    EE_EXIT_DISPATCH = 3,
    EE_EXIT_UNSUPPORTED = 4,
    // Diagnostic fail-closed exit at an instrumented CFG loop header.
    // Not an architectural EE exception or a completed guest boot.
    EE_EXIT_LOOP_BUDGET = 5,
    // Host-cooperative yield at a fully committed guest loop-header PC.
    // This does not advance guest time or imply a device event.
    EE_EXIT_HOST_YIELD = 6,
    // A committed Level 2 architectural exception, not a Level 1
    // ExcCode/EPC event. Do not route through the Level 1 HLE bridge.
    EE_EXIT_LEVEL2_EXCEPTION = 7
};

static_assert(static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_RETURN) == 0u &&
              static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_EXCEPTION) == 1u &&
              static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_EXTERNAL_CALL) == 2u &&
              static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH) == 3u &&
              static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_UNSUPPORTED) == 4u &&
              static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_LOOP_BUDGET) == 5u &&
              static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_HOST_YIELD) == 6u &&
              static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_LEVEL2_EXCEPTION) == 7u);

inline constexpr bool ee_is_level2_exception_exit(uint32_t reason)
{
    return reason == static_cast<uint32_t>(
        e_ee_exit_reason::EE_EXIT_LEVEL2_EXCEPTION);
}


// Forward declaration; the runtime owns the memory/peripheral subsystems.
struct s_ee_runtime;

// Stable C ABI entry point for a translated guest function.
// Returns an exit reason telling the host what to do next.
extern "C" uint32_t ee_entry(s_ee_cpu_state* cpu_state, s_ee_runtime* runtime);

} // namespace ee
