#pragma once
#include "ee_instruction.h"
#include "ee_cfg.h"
#include "llvm_context.h"
#include <string>
#include <vector>
#include <map>

namespace llvm {

// Emits target-independent LLVM IR text for a lifted guest function, organized
// into basic blocks with real terminator instructions (br / ret). Follows the
// runtime ABI (ptr to s_ee_cpu_state, ptr to s_ee_runtime, returns i32 exit
// reason) and uses only the stable memory-helper declarations, so the IR can be
// verified/compiled by a full LLVM toolchain without host-specific intrinsics
// (PLAN.md sections 3.2, 3.4, 3.5, 8).
class module_builder
{
public:
    // opt_level: 0 = none, 1 = dead-store elim, 2 = + CSE, 3 = + dead-temp DCE.
    // Clamped to [0, 3]. Default 0 preserves raw lift for debugging.
    explicit module_builder(llvm_context& ctx, int opt_level = 0);

    // Emit a complete module containing one lifted function built from `insns`
    // (already ordered into executed blocks). `entry_pc` is the guest entry pc.
    void emit_function(const std::vector<ee::s_ee_instruction>& insns,
                       uint32_t entry_pc, const char* func_name);

    // Emit a function from an already-built CFG (function_analyzer output).
    void emit_function_cfg(const ee::s_ee_function& fn,
                           const std::map<uint32_t, ee::s_ee_instruction>& insn_map,
                           const char* func_name);

    void set_opt_level(int level);
    int  opt_level() const { return m_opt_level; }

    std::string str() const { return m_ctx.ir(); }

private:
    llvm_context& m_ctx;
    std::string m_out;
    std::map<uint32_t, std::string> m_block_labels;   // pc -> LLVM label
    int m_temp_counter = 0;                           // unique temp SSA names
    int m_opt_level = 0;                              // 0..3

    // Per-block register-value tracking (register-state synchronisation
    // optimisation, Phase 7). current_lo[r]/current_hi[r] hold the SSA temp
    // name that holds GPR r's current value within the block being emitted,
    // or are empty if the value has not been loaded/computed yet. dirty_regs
    // tracks which GPRs differ from the cpu_state backing store and must be
    // written back at block exit.
    std::map<uint8_t, std::string> m_cur_lo, m_cur_hi;
    std::set<uint8_t> m_dirty_regs;

    // Scope of the guest instruction currently being emitted. Only used to
    // annotate read-only, SSA-based register provenance events.
    const ee::s_ee_instruction* m_current_guest_insn = nullptr;

    void header();
    void declare_memory_helpers();
    void emit_guest_store_trace(const ee::s_ee_instruction& ins,
                                const std::string& effective_address,
                                const std::string& affected_address,
                                uint32_t width, const std::string& source_value,
                                uint32_t source_lane);
    void emit_quadword_read(const std::string& address,
                            const std::string& lo, const std::string& hi);
    void emit_quadword_store(const ee::s_ee_instruction& ins,
                             const std::string& effective,
                             const std::string& affected,
                             const std::string& lo, const std::string& hi);
    std::string::size_type m_quadword_entry_offset = 0;
    void emit_guest_v0_write(const std::string& old_value,
                             const std::string& new_value);
    void begin_function(const char* func_name);
    void end_function();

    std::string gptr_lo(uint8_t reg);
    std::string gptr_hi(uint8_t reg);
    std::string src_lo(uint8_t reg);
    std::string src_hi(uint8_t reg);
    std::string block_label(uint32_t pc);
    std::string newtemp();
    std::string cop0_ptr(uint8_t reg);
    std::string fpr_ptr(uint8_t reg);
    std::string fcsr_ptr();
    std::string fpu_acc_ptr();
    std::string vu0_lo_ptr(uint8_t reg);
    std::string vu0_hi_ptr(uint8_t reg);
    std::string state_field(int field);
    // COP1 helpers: load/store FPR as float / i32 bits.
    std::string load_fpr_f32(uint8_t freg);
    std::string load_fpr_i32(uint8_t freg);
    void store_fpr_f32(uint8_t freg, const std::string& fval);
    void store_fpr_i32(uint8_t freg, const std::string& ival);

    // Register tracking helpers.
    std::string load_gpr_temp(uint8_t reg);                    // load into SSA temp, track it
    void    set_gpr_temp(uint8_t reg, const std::string& lo, const std::string& hi); // full 128-bit write
    // Scalar write: update only the low 64 bits; upper 64 is preserved (PCSX2 model).
    // Most R5900 integer ALU / load / link ops write UD[0] only and leave UD[1] intact.
    void    set_gpr_lo64(uint8_t reg, const std::string& lo);
    // 32-bit result sign-extended into the low 64, upper 64 preserved (ADDU/ADDIU/etc).
    std::string trunc32_sext64(const std::string& val);
    void    set_gpr_lo32_sext(uint8_t reg, const std::string& lo32_or_lo64);
    void    store_dirty_regs(const std::set<uint8_t>& live_out); // write back dirty+live-out regs
    void    reset_tracking();                                  // clear per-block state

    void emit_gpr_loads(const std::vector<uint8_t>& regs);
    void emit_gpr_stores(const std::vector<uint8_t>& regs);
    void emit_block(const std::vector<ee::s_ee_instruction>& block,
                    bool is_last, uint32_t entry_pc,
                    const std::set<uint8_t>& live_in,
                    const std::set<uint8_t>& live_out,
                    uint32_t fallthrough_pc = 0,
                    bool is_terminator = false,
                    bool guarded_loop_header = false);
    void emit_insn(const ee::s_ee_instruction& ins, bool is_last_in_block,
                   uint32_t fallthrough_pc, bool in_delay_slot = false);
    void emit_mmi_insn(const ee::s_ee_instruction& ins);

    // IR text optimisation passes (Phase 7). Operates on the emitted LLVM IR
    // text before it is written to disk.
    //   level 0: no optimisation
    //   level 1: dead store elimination, redundant load elimination
    //   level 2: adds common subexpression elimination
    static std::string optimize_ir(const std::string& ir, int level);
};

} // namespace llvm
