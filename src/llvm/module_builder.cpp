#include "module_builder.h"
#include "ee_cop1_rsqrt_ir.h"
#include "ee_cop1_add_ir.h"
#include "ee_cop1_mul_ir.h"
#include "ee_runtime.h"
#include <cstddef>
#include "ee_opcode.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <algorithm>
#include <map>

namespace llvm {

namespace
{
std::string u32hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

std::string reg_lo(uint8_t r)  { char b[16]; std::snprintf(b, sizeof(b), "%%g%d_lo", r); return b; }
std::string reg_hi(uint8_t r)  { char b[16]; std::snprintf(b, sizeof(b), "%%g%d_hi", r); return b; }
} // namespace

module_builder::module_builder(llvm_context& ctx, int opt_level)
    : m_ctx(ctx)
{
    set_opt_level(opt_level);
}

void module_builder::set_opt_level(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    m_opt_level = level;
}

void module_builder::header()
{
    m_out += "; ModuleID = 'ee_module'\n";
    m_out += "target datalayout = \"e-m:w-p:64:64-i64:64-f80:128-n8:16:32:64-S128\"\n";
    m_out += "target triple = \"x86_64-pc-windows-msvc\"\n";
    m_out += "\n";
    // Layout must match s_ee_cpu_state:
    // 0 gpr[32]{lo,hi}, 1 hi, 2 lo, 3 hi1, 4 lo1, 5 sa, 6 pc, 7 exit, 8 pad,
    // 9 cop0[32], 10 fpr[32], 11 vu0[32]{lo,hi}, 12 fcsr, 13 fpu_acc, 14 fpu_acc_overflow
    m_out += "%ee_cpu_state = type { "
             "[32 x { i64, i64 }], "
             "i64, i64, i64, i64, "
             "i32, i32, i32, i32, "
             "[32 x i32], "
             "[32 x i64], "
             "[32 x { i64, i64 }], "
             "i32, i32, i32 }\n";
    m_out += "\n";
}

void module_builder::declare_memory_helpers()
{
    m_out += "declare i8   @ee_mem_read8 (ptr, i32)\n";
    m_out += "declare i16  @ee_mem_read16(ptr, i32)\n";
    m_out += "declare i32  @ee_mem_read32(ptr, i32)\n";
    m_out += "declare i64  @ee_mem_read64(ptr, i32)\n";
    m_out += "declare void @ee_mem_read128(ptr, i32, ptr)\n";
    m_out += "declare void @ee_mem_write8 (ptr, i32, i8 )\n";
    m_out += "declare void @ee_mem_write16(ptr, i32, i16)\n";
    m_out += "declare void @ee_mem_write32(ptr, i32, i32)\n";
    m_out += "declare void @ee_mem_write64(ptr, i32, i64)\n";
    m_out += "declare void @ee_mem_write128(ptr, i32, ptr)\n";
    m_out += "declare void @ee_trace_guest_store(ptr, i32, i32, i32, i32, i32, i64, i32, i64, i64, i64, i64)\n";
    m_out += "declare void @ee_trace_guest_v0_write(ptr, i32, i32, i64, i64)\n";
    m_out += "declare i64 @ee_mem_lwl(ptr, i32, i64)\n";
    m_out += "declare i64 @ee_mem_lwr(ptr, i32, i64)\n";
    m_out += "declare i64 @ee_mem_ldl(ptr, i32, i64)\n";
    m_out += "declare i64 @ee_mem_ldr(ptr, i32, i64)\n";
    m_out += "declare void @ee_mem_swl(ptr, i32, i64)\n";
    m_out += "declare void @ee_mem_swr(ptr, i32, i64)\n";
    m_out += "declare void @ee_mem_sdl(ptr, i32, i64)\n";
    m_out += "declare void @ee_mem_sdr(ptr, i32, i64)\n";
    m_out += "declare i32 @llvm.ctlz.i32(i32, i1)\n";
    m_out += "declare float @llvm.sqrt.f32(float)\n";
    m_out += "declare float @llvm.fabs.f32(float)\n";
    m_out += "declare i32 @ee_raise_exception(ptr, ptr, i32, i32, i32, i32)\n";
    m_out += "declare i32 @ee_exception_return(ptr)\n";
    m_out += "declare void @ee_cop0_write(ptr, i32, i32)\n";
    m_out += "declare i32 @ee_vu0_qmfc2(ptr, ptr, i32, i32, i32, i32)\n";
    m_out += "declare i32 @ee_vu0_qmtc2(ptr, ptr, i32, i32, i32, i32)\n";
    m_out += "declare i32 @ee_vu0_cfc2(ptr, ptr, i32, i32, i32, i32)\n";
    m_out += "declare i32 @ee_vu0_ctc2(ptr, ptr, i32, i32, i32, i32)\n";
    m_out += "declare i32 @ee_vu0_macro_fmac(ptr, ptr, i32, i32)\n";
    m_out += "declare i32 @ee_vu0_macro_scalar(ptr, ptr, i32, i32)\n";
    m_out += "declare i32 @ee_vu0_macro_indexed_memory(ptr, ptr, i32, i32)\n";
    m_out += "declare i32 @ee_vu0_get_branch_condition(ptr)\n";
    m_out += "\n";

    // The trace is a bounded, host-only stream for reference timing
    // calibration. Sequence numbers are reference instruction *slots*,
    // not validated R5900 cycles. Native reached instructions, annulled
    // branch-likely slots and RAM-interpreted commits use one schema.
    m_out += "define internal void @ee_trace_instruction_observation(ptr %rt, i32 %pc, i32 %opcode, i32 %kind, i64 %attempt, i64 %reference_slot) {\n";
    m_out += "entry:\n";
    m_out += "  %trace_ptr_field = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime, m_instruction_trace)) + "\n";
    m_out += "  %trace_ptr = load ptr, ptr %trace_ptr_field\n";
    m_out += "  %trace_enabled = icmp ne ptr %trace_ptr, null\n";
    m_out += "  br i1 %trace_enabled, label %trace_window, label %done\n";
    m_out += "trace_window:\n";
    m_out += "  %first_field = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime, m_instruction_trace_first_pc)) + "\n";
    m_out += "  %last_field = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime, m_instruction_trace_last_pc)) + "\n";
    m_out += "  %first = load i32, ptr %first_field\n";
    m_out += "  %last = load i32, ptr %last_field\n";
    m_out += "  %above = icmp uge i32 %pc, %first\n";
    m_out += "  %below = icmp ule i32 %pc, %last\n";
    m_out += "  %in_window = and i1 %above, %below\n";
    m_out += "  br i1 %in_window, label %trace_capacity, label %done\n";
    m_out += "trace_capacity:\n";
    m_out += "  %count_field = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime, m_instruction_trace_count)) + "\n";
    m_out += "  %capacity_field = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime, m_instruction_trace_capacity)) + "\n";
    m_out += "  %used = load i32, ptr %count_field\n";
    m_out += "  %capacity = load i32, ptr %capacity_field\n";
    m_out += "  %has_space = icmp ult i32 %used, %capacity\n";
    m_out += "  br i1 %has_space, label %trace_write, label %trace_overflow\n";
    m_out += "trace_write:\n";
    m_out += "  %index = zext i32 %used to i64\n";
    m_out += "  %offset = mul i64 %index, " +
             std::to_string(sizeof(ee::s_ee_instruction_trace_entry)) + "\n";
    m_out += "  %slot = getelementptr i8, ptr %trace_ptr, i64 %offset\n";
    m_out += "  store i64 %attempt, ptr %slot\n";
    m_out += "  %slot_pc = getelementptr i8, ptr %slot, i64 " +
             std::to_string(offsetof(ee::s_ee_instruction_trace_entry, m_pc)) + "\n";
    m_out += "  %slot_op = getelementptr i8, ptr %slot, i64 " +
             std::to_string(offsetof(ee::s_ee_instruction_trace_entry, m_opcode_word)) + "\n";
    m_out += "  store i32 %pc, ptr %slot_pc\n";
    m_out += "  store i32 %opcode, ptr %slot_op\n";
    m_out += "  %slot_kind = getelementptr i8, ptr %slot, i64 " +
             std::to_string(offsetof(ee::s_ee_instruction_trace_entry, m_kind)) + "\n";
    m_out += "  store i32 %kind, ptr %slot_kind\n";
    m_out += "  %slot_reference = getelementptr i8, ptr %slot, i64 " +
             std::to_string(offsetof(ee::s_ee_instruction_trace_entry, m_reference_slot)) + "\n";
    m_out += "  store i64 %reference_slot, ptr %slot_reference\n";
    m_out += "  %slot_hle = getelementptr i8, ptr %slot, i64 " +
             std::to_string(offsetof(ee::s_ee_instruction_trace_entry, m_hle_service)) + "\n";
    m_out += "  store i32 0, ptr %slot_hle\n";
    m_out += "  %next_used = add i32 %used, 1\n";
    m_out += "  store i32 %next_used, ptr %count_field\n";
    m_out += "  br label %done\n";
    m_out += "trace_overflow:\n";
    m_out += "  %overflow_field = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime, m_instruction_trace_overflow)) + "\n";
    m_out += "  store i32 1, ptr %overflow_field\n";
    m_out += "  br label %done\n";
    m_out += "done:\n";
    m_out += "  ret void\n";
    m_out += "}\n\n";

    // Optional native issue-timing observation, with no host symbol
    // dependency. The callback is explicitly installed by the host and
    // may reject an event; a rejection is latched, never converted into
    // guessed cycles or a guest-visible interrupt.
    m_out += "define internal void @ee_observe_timing_event(ptr %rt, i32 %pc, i32 %opcode, i32 %kind, i64 %attempt, i64 %reference_slot) {\n";
    m_out += "entry:\n";
    m_out += "  %null = icmp eq ptr %rt, null\n";
    m_out += "  br i1 %null, label %done, label %check\n";
    m_out += "check:\n";
    m_out += "  %failed_ptr = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime, m_timing_observer_failed)) + "\n";
    m_out += "  %failed = load i32, ptr %failed_ptr\n";
    m_out += "  %already_failed = icmp ne i32 %failed, 0\n";
    m_out += "  br i1 %already_failed, label %done, label %check_callback\n";
    m_out += "check_callback:\n";
    m_out += "  %cb_ptr = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime, m_timing_observer)) + "\n";
    m_out += "  %callback = load ptr, ptr %cb_ptr\n";
    m_out += "  %enabled = icmp ne ptr %callback, null\n";
    m_out += "  br i1 %enabled, label %invoke, label %done\n";
    m_out += "invoke:\n";
    m_out += "  %user_ptr = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime, m_timing_observer_user)) + "\n";
    m_out += "  %user = load ptr, ptr %user_ptr\n";
    m_out += "  %ok = call i1 %callback(ptr %user, i32 %pc, i32 %opcode, i32 %kind, i32 0, i64 %attempt, i64 %reference_slot)\n";
    m_out += "  br i1 %ok, label %done, label %reject\n";
    m_out += "reject:\n";
    m_out += "  store i32 1, ptr %failed_ptr\n";
    m_out += "  br label %done\n";
    m_out += "done:\n";
    m_out += "  ret void\n";
    m_out += "}\n\n";

    // Private helper avoids unresolved external symbols in MCJIT fixtures.
    // Count executed VALID guest instructions, not host dispatches/loops.
    // This is NOT an EE cycle counter and must not drive GS scanout.
    m_out += "define internal void @ee_count_guest_instruction(ptr %rt, i32 %pc, i32 %opcode) {\n";
    m_out += "entry:\n";
    m_out += "  %valid_rt = icmp ne ptr %rt, null\n";
    m_out += "  br i1 %valid_rt, label %count, label %done\n";
    m_out += "count:\n";
    m_out += "  %counter_ptr = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime, m_guest_instruction_attempts)) +
             "\n";
    m_out += "  %old_count = load i64, ptr %counter_ptr\n";
    m_out += "  %new_count = add i64 %old_count, 1\n";
    m_out += "  store i64 %new_count, ptr %counter_ptr\n";
    // Reference interpreter slot accounting is distinct from reached
    // instructions: a not-taken branch-likely reserves its annulled slot.
    // It is explicitly NOT an R5900 cycle clock or a scanout source.
    m_out += "  %ref_counter_ptr = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime,
                                     m_reference_instruction_slots)) + "\n";
    m_out += "  %ref_old = load i64, ptr %ref_counter_ptr\n";
    m_out += "  %ref_new = add i64 %ref_old, 1\n";
    m_out += "  store i64 %ref_new, ptr %ref_counter_ptr\n";
    m_out += "  call void @ee_observe_timing_event(ptr %rt, i32 %pc, i32 %opcode, i32 0, i64 %new_count, i64 %ref_new)\n";
    m_out += "  call void @ee_trace_instruction_observation(ptr %rt, i32 %pc, i32 %opcode, i32 0, i64 %new_count, i64 %ref_new)\n";
    m_out += "  br label %done\n";
    m_out += "done:\n";
    m_out += "  ret void\n";
    m_out += "}\n\n";

    // The ps2-web-recomp reference interpreter charges two instruction
    // slots for a branch, including when branch-likely annuls its delay
    // instruction. This helper never increments the actual executed
    // instruction counter or touches the guest's architectural state.
    m_out += "define internal void @ee_count_reference_annulled_slot(ptr %rt, i32 %pc, i32 %opcode) {\n";
    m_out += "entry:\n";
    m_out += "  %is_null = icmp eq ptr %rt, null\n";
    m_out += "  br i1 %is_null, label %done, label %count\n";
    m_out += "count:\n";
    m_out += "  %ref_ptr = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime,
                                     m_reference_instruction_slots)) + "\n";
    m_out += "  %old = load i64, ptr %ref_ptr\n";
    m_out += "  %new = add i64 %old, 1\n";
    m_out += "  store i64 %new, ptr %ref_ptr\n";
    m_out += "  %attempt_ptr = getelementptr i8, ptr %rt, i64 " +
             std::to_string(offsetof(ee::s_ee_runtime, m_guest_instruction_attempts)) + "\n";
    m_out += "  %attempt = load i64, ptr %attempt_ptr\n";
    m_out += "  call void @ee_observe_timing_event(ptr %rt, i32 %pc, i32 %opcode, i32 1, i64 %attempt, i64 %new)\n";
    m_out += "  call void @ee_trace_instruction_observation(ptr %rt, i32 %pc, i32 %opcode, i32 1, i64 %attempt, i64 %new)\n";
    m_out += "  br label %done\n";
    m_out += "done:\n";
    m_out += "  ret void\n";
    m_out += "}\n\n";
}

std::string module_builder::gptr_lo(uint8_t reg)
{
    // Emits "%tN = getelementptr ..." (a GEP is not a valid inline operand
    // for load/store) and returns the temp holding the pointer.
    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "getelementptr %%ee_cpu_state, ptr %%cpu, i32 0, i32 0, i32 %u, i32 0",
        (unsigned)reg);
    std::string t = newtemp();
    m_out += "  " + t + " = " + buf + "\n";
    return t;
}

std::string module_builder::gptr_hi(uint8_t reg)
{
    // Emits "%tN = getelementptr ..." (a GEP is not a valid inline operand
    // for load/store) and returns the temp holding the pointer.
    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "getelementptr %%ee_cpu_state, ptr %%cpu, i32 0, i32 0, i32 %u, i32 1",
        (unsigned)reg);
    std::string t = newtemp();
    m_out += "  " + t + " = " + buf + "\n";
    return t;
}

std::string module_builder::cop0_ptr(uint8_t reg)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "getelementptr %%ee_cpu_state, ptr %%cpu, i32 0, i32 9, i32 %u",
        (unsigned)reg);
    return buf;
}

std::string module_builder::fpr_ptr(uint8_t reg)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "getelementptr %%ee_cpu_state, ptr %%cpu, i32 0, i32 10, i32 %u",
        (unsigned)reg);
    return buf;
}

std::string module_builder::fcsr_ptr()
{
    return "getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 12";
}

std::string module_builder::fpu_acc_ptr()
{
    return "getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 13";
}

std::string module_builder::load_fpr_i32(uint8_t freg)
{
    std::string p = newtemp();
    std::string v64 = newtemp();
    std::string v32 = newtemp();
    m_out += "  " + p + " = " + fpr_ptr(freg) + "\n";
    m_out += "  " + v64 + " = load i64, ptr " + p + "\n";
    m_out += "  " + v32 + " = trunc i64 " + v64 + " to i32\n";
    return v32;
}

std::string module_builder::load_fpr_f32(uint8_t freg)
{
    std::string bits = load_fpr_i32(freg);
    std::string f = newtemp();
    m_out += "  " + f + " = bitcast i32 " + bits + " to float\n";
    return f;
}

void module_builder::store_fpr_i32(uint8_t freg, const std::string& ival)
{
    std::string p = newtemp();
    std::string v64 = newtemp();
    m_out += "  " + p + " = " + fpr_ptr(freg) + "\n";
    m_out += "  " + v64 + " = zext i32 " + ival + " to i64\n";
    m_out += "  store i64 " + v64 + ", ptr " + p + "\n";
}

void module_builder::store_fpr_f32(uint8_t freg, const std::string& fval)
{
    std::string bits = newtemp();
    m_out += "  " + bits + " = bitcast float " + fval + " to i32\n";
    store_fpr_i32(freg, bits);
}

std::string module_builder::vu0_lo_ptr(uint8_t reg)
{
    // Emits "%tN = getelementptr ..." (a GEP is not a valid inline operand
    // for load/store) and returns the temp holding the pointer.
    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "getelementptr %%ee_cpu_state, ptr %%cpu, i32 0, i32 11, i32 %u, i32 0",
        (unsigned)reg);
    std::string t = newtemp();
    m_out += "  " + t + " = " + buf + "\n";
    return t;
}

std::string module_builder::vu0_hi_ptr(uint8_t reg)
{
    // Emits "%tN = getelementptr ..." (a GEP is not a valid inline operand
    // for load/store) and returns the temp holding the pointer.
    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "getelementptr %%ee_cpu_state, ptr %%cpu, i32 0, i32 11, i32 %u, i32 1",
        (unsigned)reg);
    std::string t = newtemp();
    m_out += "  " + t + " = " + buf + "\n";
    return t;
}

std::string module_builder::state_field(int field)
{
    std::string ge = newtemp();
    m_out += "  " + ge + " = getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 " +
             std::to_string(field) + "\n";
    return ge;
}

std::string module_builder::block_label(uint32_t pc)
{
    // Bare label name. Definitions: "bb_0x..:"; references: "label %bb_0x.."
    char buf[32];
    std::snprintf(buf, sizeof(buf), "bb_0x%x", pc);
    return buf;
}

std::string module_builder::newtemp()
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%%t%d", m_temp_counter++);
    return buf;
}

// ---- Register tracking (Phase 7: reduce register-state synchronisation) ----

std::string module_builder::load_gpr_temp(uint8_t reg)
{
    if (reg == 0) return "0";
    auto it = m_cur_lo.find(reg);
    if (it != m_cur_lo.end()) return it->second;
    std::string lo = newtemp(); std::string hi = newtemp();
    m_out += "  " + lo + " = load i64, ptr " + gptr_lo(reg) + "\n";
    m_out += "  " + hi + " = load i64, ptr " + gptr_hi(reg) + "\n";
    m_cur_lo[reg] = lo;
    m_cur_hi[reg] = hi;
    return lo;
}

std::string module_builder::src_lo(uint8_t reg)
{
    if (reg == 0) return "0";
    return load_gpr_temp(reg);
}

std::string module_builder::src_hi(uint8_t reg)
{
    if (reg == 0) return "0";
    load_gpr_temp(reg);
    return m_cur_hi[reg];
}

void module_builder::emit_guest_store_trace(
    const ee::s_ee_instruction& ins,
    const std::string& effective_address,
    const std::string& affected_address, uint32_t width,
    const std::string& source_value, uint32_t source_lane
)
{
    // SSA values capture current architectural operands even when the CPU
    // backing store has not been synchronized at this block boundary.
    const std::string base = src_lo(ins.rs);
    const std::string sp = src_lo(29);
    const std::string fp = src_lo(30);
    const std::string ra = src_lo(31);
    m_out += "  call void @ee_trace_guest_store(ptr %rt, i32 " +
             std::to_string(ins.pc) + ", i32 " +
             std::to_string(ins.opcode_word) + ", i32 " +
             effective_address + ", i32 " + affected_address + ", i32 " +
             std::to_string(width) + ", i64 " + source_value + ", i32 " +
             std::to_string(source_lane) + ", i64 " + base + ", i64 " +
             sp + ", i64 " + fp + ", i64 " + ra + ")\n";
}

void module_builder::emit_guest_v0_write(
    const std::string& old_value, const std::string& new_value
)
{
    if (m_current_guest_insn == nullptr)
    {
        return;
    }

    // All values refer to the emitter's live SSA GPR state. This observes
    // the guest V0 assignment without causing a second architectural write.
    m_out += "  call void @ee_trace_guest_v0_write(ptr %rt, i32 " +
             std::to_string(m_current_guest_insn->pc) + ", i32 " +
             std::to_string(m_current_guest_insn->opcode_word) + ", i64 " +
             old_value + ", i64 " + new_value + ")\n";
}

void module_builder::set_gpr_temp(uint8_t reg, const std::string& lo,
                                  const std::string& hi)
{
    if (reg == 0) return; // $zero is hard-wired
    if (reg == 2 && m_current_guest_insn != nullptr)
    {
        const std::string old_value = src_lo(reg);
        emit_guest_v0_write(old_value, lo);
    }
    m_cur_lo[reg] = lo;
    m_cur_hi[reg] = hi;
    m_dirty_regs.insert(reg);
}

// Write only the low 64 bits of a GPR; preserve the existing upper 64.
// Matches PCSX2 interpreter behaviour for scalar integer ops (UD[0] write only).
void module_builder::set_gpr_lo64(uint8_t reg, const std::string& lo)
{
    if (reg == 0) return;
    // Ensure hi is loaded into the tracking map so it is not lost on store-back.
    std::string hi = src_hi(reg);
    if (reg == 2 && m_current_guest_insn != nullptr)
    {
        const std::string old_value = src_lo(reg);
        emit_guest_v0_write(old_value, lo);
    }
    m_cur_lo[reg] = lo;
    m_cur_hi[reg] = hi;
    m_dirty_regs.insert(reg);
}

// Sign-extend a 32-bit (or low-32-of-64) result into the full low 64 bits,
// then preserve the upper 64. Used by ADDU/ADDIU/SUBU/LW/LUI/etc.
std::string module_builder::trunc32_sext64(const std::string& val)
{
    // trunc to i32 then sext to i64 so the high 32 of the low doubleword
    // become a proper sign extension of the 32-bit result.
    std::string t32 = newtemp();
    std::string t64 = newtemp();
    m_out += "  " + t32 + " = trunc i64 " + val + " to i32\n";
    m_out += "  " + t64 + " = sext i32 " + t32 + " to i64\n";
    return t64;
}

void module_builder::set_gpr_lo32_sext(uint8_t reg, const std::string& val)
{
    if (reg != 0)
    {
        set_gpr_lo64(reg, trunc32_sext64(val));
    }
}

void module_builder::store_dirty_regs(const std::set<uint8_t>& live_out)
{
    for (uint8_t r : m_dirty_regs)
    {
        if (live_out.count(r) == 0) continue;
        auto lit = m_cur_lo.find(r);
        auto hit = m_cur_hi.find(r);
        if (lit == m_cur_lo.end() || hit == m_cur_hi.end()) continue;
        m_out += "  store i64 " + lit->second + ", ptr " + gptr_lo(r) + "\n";
        m_out += "  store i64 " + hit->second + ", ptr " + gptr_hi(r) + "\n";
    }
}

void module_builder::reset_tracking()
{
    m_cur_lo.clear();
    m_cur_hi.clear();
    m_dirty_regs.clear();
}

// ---- Block reads/writes (computed from actual instructions) ----

static std::set<uint8_t> block_written(const std::vector<ee::s_ee_instruction>& block)
{
    std::set<uint8_t> w;
    using ee::e_ee_opcode;
    for (const auto& ins : block)
    {
        if (ins.valid != ee::e_ee_valid::VALID) continue;
        switch (ins.opcode)
        {
        case e_ee_opcode::OP_JAL:
            w.insert(31);
            break;
        case e_ee_opcode::OP_REGIMM:
            // BLTZAL/BGEZAL and their likely forms always write $ra = PC+8.
            if (ins.rt >= 0x10 && ins.rt <= 0x13)
                w.insert(31);
            break;
        case e_ee_opcode::OP_ADDI:
        case e_ee_opcode::OP_ADDIU:
        case e_ee_opcode::OP_DADDI:
        case e_ee_opcode::OP_DADDIU:
        case e_ee_opcode::OP_ANDI:
        case ee::e_ee_opcode::OP_ORI:
        case ee::e_ee_opcode::OP_XORI:
        case ee::e_ee_opcode::OP_LUI:
        case ee::e_ee_opcode::OP_SLTI:
        case ee::e_ee_opcode::OP_SLTIU:
        case ee::e_ee_opcode::OP_LB:
        case ee::e_ee_opcode::OP_LH:
        case ee::e_ee_opcode::OP_LBU:
        case ee::e_ee_opcode::OP_LHU:
        case ee::e_ee_opcode::OP_LW:
        case ee::e_ee_opcode::OP_LWU:
        case ee::e_ee_opcode::OP_LD:
        case ee::e_ee_opcode::OP_LQ:
        case ee::e_ee_opcode::OP_LWL:
        case ee::e_ee_opcode::OP_LWR:
        case ee::e_ee_opcode::OP_LDL:
        case ee::e_ee_opcode::OP_LDR:
            if (ins.rt != 0) w.insert(ins.rt);
            break;
        case e_ee_opcode::OP_SPECIAL:
            switch (static_cast<e_ee_opcode>(ins.funct))
            {
            case e_ee_opcode::FUNCT_MFHI:
            case e_ee_opcode::FUNCT_MFLO:
                if (ins.rd != 0) w.insert(ins.rd);
                break;
            case e_ee_opcode::FUNCT_JALR:
                if (ins.rd != 0) w.insert(ins.rd);
                break;
            case e_ee_opcode::FUNCT_ADD:
            case e_ee_opcode::FUNCT_ADDU:
            case e_ee_opcode::FUNCT_SUB:
            case e_ee_opcode::FUNCT_SUBU:
            case e_ee_opcode::FUNCT_DADD:
            case e_ee_opcode::FUNCT_DADDU:
            case e_ee_opcode::FUNCT_DSUB:
            case e_ee_opcode::FUNCT_DSUBU:
            case e_ee_opcode::FUNCT_DSLL:
            case e_ee_opcode::FUNCT_DSRL:
            case e_ee_opcode::FUNCT_DSRA:
            case e_ee_opcode::FUNCT_DSLL32:
            case e_ee_opcode::FUNCT_DSRL32:
            case e_ee_opcode::FUNCT_DSRA32:
            case e_ee_opcode::FUNCT_DSLLV:
            case e_ee_opcode::FUNCT_DSRLV:
            case e_ee_opcode::FUNCT_DSRAV:
            case e_ee_opcode::FUNCT_SRAV:
            case e_ee_opcode::FUNCT_MOVZ:
            case e_ee_opcode::FUNCT_MOVN:
            case e_ee_opcode::FUNCT_AND:
            case e_ee_opcode::FUNCT_OR:
            case e_ee_opcode::FUNCT_XOR:
            case e_ee_opcode::FUNCT_NOR:
            case e_ee_opcode::FUNCT_SLT:
            case e_ee_opcode::FUNCT_SLTU:
            case e_ee_opcode::FUNCT_SLL:
            case e_ee_opcode::FUNCT_SRL:
            case e_ee_opcode::FUNCT_SRA:
            case e_ee_opcode::FUNCT_ROTR:
            case e_ee_opcode::FUNCT_ROTRV:
                if (ins.rd != 0) w.insert(ins.rd);
                break;
            default:
                break;
            }
            break;
        case e_ee_opcode::OP_COP0:
            if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP0_MFC0) && ins.rt != 0) w.insert(ins.rt);
            break;
        case e_ee_opcode::OP_COP1:
            if ((ins.rs == static_cast<uint8_t>(e_ee_opcode::COP1_MFC1) ||
                 ins.rs == static_cast<uint8_t>(e_ee_opcode::COP1_CFC1)) &&
                ins.rt != 0)
            {
                w.insert(ins.rt);
            }
            break;
        case e_ee_opcode::OP_COP2:
            if ((ins.rs == static_cast<uint8_t>(e_ee_opcode::COP2_QMFC2) ||
                 ins.rs == static_cast<uint8_t>(e_ee_opcode::COP2_CFC2)) &&
                ins.rt != 0)
            {
                w.insert(ins.rt);
            }
            break;
        case e_ee_opcode::OP_MMI:
            switch (ins.funct)
            {
            case 0x10: // MFHI1: rd = HI1
            case 0x12: // MFLO1: rd = LO1
                if (ins.rd != 0) w.insert(ins.rd);
                break;
            case 0x11: // MTHI1: HI1 = rs
            case 0x13: // MTLO1: LO1 = rs
            case 0x31: // PMTHL: updates HI/LO, never rd
            case 0x1a: // DIV1: writes HI1/LO1, not rd
            case 0x1b: // DIVU1: writes HI1/LO1, not rd
                break;
            default: // packed MMI operations usually write rd
                // Divides and MMI3 full-bank transfers have no rd write.
                if ((ins.funct == 0x09 || ins.funct == 0x29) &&
                    (ins.sa == 0x0d ||
                     (ins.funct == 0x29 &&
                      (ins.sa == 0x08 || ins.sa == 0x09))))
                {
                    break;
                }
                if (ins.rd != 0) w.insert(ins.rd);
                break;
            }
            break;
        default:
            break;
        }
    }
    return w;
}

static std::set<uint8_t> block_read_before_write(const std::vector<ee::s_ee_instruction>& block)
{
    std::set<uint8_t> result;
    std::set<uint8_t> written_so_far;
    auto consider_read = [&](uint8_t r) {
        if (r != 0 && written_so_far.count(r) == 0) result.insert(r);
    };
    using ee::e_ee_opcode;
    for (const auto& ins : block)
    {
        if (ins.valid != ee::e_ee_valid::VALID) continue;
        switch (ins.opcode)
        {
        case e_ee_opcode::OP_JAL:
            written_so_far.insert(31);
            break;
        case e_ee_opcode::OP_ADDIU:
        case e_ee_opcode::OP_DADDIU:
        case e_ee_opcode::OP_ANDI:
        case e_ee_opcode::OP_ORI:
        case e_ee_opcode::OP_XORI:
        case e_ee_opcode::OP_LUI:
        case e_ee_opcode::OP_SLTI:
        case e_ee_opcode::OP_SLTIU:
        case e_ee_opcode::OP_LB:
        case e_ee_opcode::OP_LH:
        case e_ee_opcode::OP_LBU:
        case e_ee_opcode::OP_LHU:
        case e_ee_opcode::OP_LW:
        case e_ee_opcode::OP_LWU:
        case e_ee_opcode::OP_LD:
        case e_ee_opcode::OP_LQ:
        case e_ee_opcode::OP_LWL:
        case e_ee_opcode::OP_LWR:
        case e_ee_opcode::OP_LDL:
        case e_ee_opcode::OP_LDR:
            consider_read(ins.rs);
            if (ins.rt != 0)
            {
                // A partial load must merge with the current destination.
                if (ins.opcode == e_ee_opcode::OP_LWL ||
                    ins.opcode == e_ee_opcode::OP_LWR ||
                    ins.opcode == e_ee_opcode::OP_LDL ||
                    ins.opcode == e_ee_opcode::OP_LDR)
                {
                    consider_read(ins.rt);
                }
                written_so_far.insert(ins.rt);
            }
            break;
        case e_ee_opcode::OP_SW:
        case e_ee_opcode::OP_SB:
        case e_ee_opcode::OP_SH:
        case e_ee_opcode::OP_SD:
        case e_ee_opcode::OP_SQ:
        case e_ee_opcode::OP_SWL:
        case e_ee_opcode::OP_SDL:
        case e_ee_opcode::OP_SWR:
        case e_ee_opcode::OP_SDR:
            consider_read(ins.rs);
            consider_read(ins.rt);
            break;
        case e_ee_opcode::OP_LWC1:
        case e_ee_opcode::OP_SWC1:
        case e_ee_opcode::OP_LQC2:
        case e_ee_opcode::OP_SQC2:
            // These transfers read a GPR address base but have no GPR result.
            consider_read(ins.rs);
            break;
        case e_ee_opcode::OP_SPECIAL:
            switch (static_cast<e_ee_opcode>(ins.funct))
            {
            case e_ee_opcode::FUNCT_MFHI:
            case e_ee_opcode::FUNCT_MFLO:
                break;
            case e_ee_opcode::FUNCT_MTHI:
            case e_ee_opcode::FUNCT_MTLO:
            case e_ee_opcode::FUNCT_MULT:
            case e_ee_opcode::FUNCT_MULTU:
            case e_ee_opcode::FUNCT_DIV:
            case e_ee_opcode::FUNCT_DIVU:
                consider_read(ins.rs);
                consider_read(ins.rt);
                break;
            case e_ee_opcode::FUNCT_JR:
                consider_read(ins.rs);
                break;
            case e_ee_opcode::FUNCT_JALR:
                consider_read(ins.rs);
                if (ins.rd != 0) written_so_far.insert(ins.rd);
                break;
            case e_ee_opcode::FUNCT_DSLLV:
            case e_ee_opcode::FUNCT_DSRLV:
            case e_ee_opcode::FUNCT_DSRAV:
                consider_read(ins.rs);
                [[fallthrough]];
            case e_ee_opcode::FUNCT_DSLL:
            case e_ee_opcode::FUNCT_DSRL:
            case e_ee_opcode::FUNCT_DSRA:
            case e_ee_opcode::FUNCT_DSLL32:
            case e_ee_opcode::FUNCT_DSRL32:
            case e_ee_opcode::FUNCT_DSRA32:
                // Doubleword shifts read rt and preserve the upper half of rd.
                consider_read(ins.rt);
                if (ins.rd != 0) written_so_far.insert(ins.rd);
                break;
            case e_ee_opcode::FUNCT_SRAV:
                consider_read(ins.rs);
                consider_read(ins.rt);
                if (ins.rd != 0)
                {
                    written_so_far.insert(ins.rd);
                }
                break;
            case e_ee_opcode::FUNCT_MOVZ:
            case e_ee_opcode::FUNCT_MOVN:
                // Conditional register moves preserve rd when their predicate
                // is false, so the old destination is an architectural input.
                consider_read(ins.rs);
                consider_read(ins.rt);
                consider_read(ins.rd);
                if (ins.rd != 0)
                {
                    written_so_far.insert(ins.rd);
                }
                break;
            case e_ee_opcode::FUNCT_DADD:
            case e_ee_opcode::FUNCT_DADDU:
            case e_ee_opcode::FUNCT_DSUB:
            case e_ee_opcode::FUNCT_DSUBU:
                // Scalar doubleword results preserve the upper 64 bits of rd.
                consider_read(ins.rs);
                consider_read(ins.rt);
                if (ins.rd != 0) written_so_far.insert(ins.rd);
                break;
            case e_ee_opcode::FUNCT_ADD:
            case e_ee_opcode::FUNCT_ADDU:
            case e_ee_opcode::FUNCT_SUB:
            case e_ee_opcode::FUNCT_SUBU:
            case e_ee_opcode::FUNCT_AND:
            case e_ee_opcode::FUNCT_OR:
            case e_ee_opcode::FUNCT_XOR:
            case e_ee_opcode::FUNCT_NOR:
            case e_ee_opcode::FUNCT_SLT:
            case e_ee_opcode::FUNCT_SLTU:
            case e_ee_opcode::FUNCT_SLL:
            case e_ee_opcode::FUNCT_SRL:
            case e_ee_opcode::FUNCT_SRA:
            case e_ee_opcode::FUNCT_ROTR:
            case e_ee_opcode::FUNCT_ROTRV:
                consider_read(ins.rs);
                consider_read(ins.rt);
                if (ins.rd != 0) written_so_far.insert(ins.rd);
                break;
            default:
                break;
            }
            break;
        case e_ee_opcode::OP_BEQ:
        case e_ee_opcode::OP_BNE:
        case e_ee_opcode::OP_BEQL:
        case e_ee_opcode::OP_BNEL:
            consider_read(ins.rs);
            consider_read(ins.rt);
            break;
        case e_ee_opcode::OP_BLEZ:
        case e_ee_opcode::OP_BGTZ:
        case e_ee_opcode::OP_BLEZL:
        case e_ee_opcode::OP_BGTZL:
            consider_read(ins.rs);
            break;
        case e_ee_opcode::OP_REGIMM:
            consider_read(ins.rs);
            // The link is architecturally written before the delay slot, so
            // later reads of $ra in this same block must see the new value.
            if (ins.rt >= 0x10 && ins.rt <= 0x13)
                written_so_far.insert(31);
            break;
        case e_ee_opcode::OP_ADDI:
        case e_ee_opcode::OP_DADDI:
            consider_read(ins.rs);
            if (ins.rt != 0) written_so_far.insert(ins.rt);
            break;
        case e_ee_opcode::OP_COP0:
            if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP0_MTC0)) consider_read(ins.rt);
            break;
        case e_ee_opcode::OP_COP1:
            if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP1_MTC1) ||
                ins.rs == static_cast<uint8_t>(e_ee_opcode::COP1_CTC1))
            {
                consider_read(ins.rt);
            }
            if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP1_LWC1)) {
                consider_read(ins.rs);
                if (ins.rt != 0) written_so_far.insert(ins.rt);
            }
            if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP1_SWC1)) {
                consider_read(ins.rs);
                consider_read(ins.rt);
            }
            break;
        case e_ee_opcode::OP_COP2:
            if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP2_QMTC2) ||
                ins.rs == static_cast<uint8_t>(e_ee_opcode::COP2_CTC2))
            {
                consider_read(ins.rt);
            }
            break;
        case e_ee_opcode::OP_MMI:
            switch (ins.funct)
            {
            case 0x04: // PLZCW updates only rd's low doubleword.
                consider_read(ins.rs);
                consider_read(ins.rd); // preserve previous upper 64 bits
                if (ins.rd != 0) written_so_far.insert(ins.rd);
                break;
            case 0x10: // MFHI1: rd = HI1
            case 0x12: // MFLO1: rd = LO1
            case 0x30: // PMFHL: writes all 128 bits of rd
                if (ins.rd != 0) written_so_far.insert(ins.rd);
                break;
            case 0x31: // PMTHL: only reads rs; no GPR destination
                consider_read(ins.rs);
                break;
            case 0x11: // MTHI1: HI1 = rs
            case 0x13: // MTLO1: LO1 = rs
            case 0x1a: // DIV1
            case 0x1b: // DIVU1
                consider_read(ins.rs);
                consider_read(ins.rt);
                break;
            case 0x09: // MMI2: PMFHI / PMFLO / PMULTW / PDIVW
            case 0x29: // MMI3: PMTHI / PMTLO / PMULTUW / PDIVUW
                if (ins.sa == 0x08 || ins.sa == 0x09)
                {
                    if (ins.funct == 0x29)
                    {
                        consider_read(ins.rs);
                    }
                    else if (ins.rd != 0)
                    {
                        written_so_far.insert(ins.rd);
                    }
                    break;
                }
                if (ins.sa == 0x0d) // PDIV* never writes rd
                {
                    consider_read(ins.rs);
                    consider_read(ins.rt);
                    break;
                }
                [[fallthrough]];
            default: // packed MMI operations read rs/rt and write rd
                consider_read(ins.rs);
                consider_read(ins.rt);
                if (ins.rd != 0) written_so_far.insert(ins.rd);
                break;
            }
            break;
        default:
            break;
        }
    }
    return result;
}

void module_builder::emit_insn(const ee::s_ee_instruction& ins,
                               bool is_last_in_block,
                               uint32_t fallthrough_pc,
                               bool in_delay_slot)
{
    using ee::e_ee_opcode;
    // Restore the previous instruction even when an opcode emits an early
    // return (or is a branch delay-slot instruction). LLVM register writes
    // outside emit_insn must never inherit a stale guest instruction PC.
    const ee::s_ee_instruction* saved_insn = m_current_guest_insn;
    m_current_guest_insn = &ins;
    struct s_insn_trace_restore
    {
        const ee::s_ee_instruction*& m_target;
        const ee::s_ee_instruction* m_saved;
        ~s_insn_trace_restore()
        {
            m_target = m_saved;
        }
    } restore{m_current_guest_insn, saved_insn};

    m_out += "  ; ";
    m_out += u32hex(ins.pc);
    m_out += ": ";
    m_out += ins.mnemonic;
    m_out += "\n";

    if (ins.valid != ee::e_ee_valid::VALID)
    {
        // Never silently skip or approximate an instruction we cannot lower
        // (PLAN.md guardrail). Record the guest pc + reason and leave the
        // function with EE_EXIT_UNSUPPORTED so the host can see exactly which
        // instruction was reached. Code after it in the block is unreachable.
        // Flush registers already computed in this block (they live in SSA
        // temps) so the host sees correct architectural state at the exit.
        {
            std::set<uint8_t> all;
            for (uint8_t r : m_dirty_regs) all.insert(r);
            store_dirty_regs(all);
        }
        char pcbuf[32];
        std::snprintf(pcbuf, sizeof(pcbuf), "%u", ins.pc);
        std::string ppc = state_field(6);   // m_pc
        m_out += "  store i32 " + std::string(pcbuf) + ", ptr " + ppc + "\n";
        std::string pex = state_field(7);   // m_exit_reason
        m_out += "  store i32 4, ptr " + pex + "\n";
        m_out += "  ret i32 4\n";
        m_out += "unsup_cont_" + std::string(pcbuf) + "_" + std::to_string(m_temp_counter++) + ":\n";
        return;
    }

    // Instructions that the code generator refuses remain uncounted.
    // The call is internal to each IR module, so it has no host ABI cost.
    m_out += "  call void @ee_count_guest_instruction(ptr %rt, i32 " +
             std::to_string(ins.pc) + ", i32 " +
             std::to_string(ins.opcode_word) + ")\n";

    if (fallthrough_pc == 0)
        fallthrough_pc = ins.pc + 8;

    // MIPS scalar loads/stores and VU0 quadword transfers raise
    // AdEL/AdES before any memory or destination-register side effect.
    // LQ/SQ are intentionally exempt: they mask address bits 0..3.
    // Commit prior SSA writes before taking the exception so EPC,
    // Cause.BD and BadVAddr observe precise guest architectural state.
    const auto guard_memory_alignment =
        [&](const std::string& address, uint32_t width, uint32_t excode)
    {
        const std::string bits = newtemp();
        const std::string misaligned = newtemp();
        const std::string suffix = std::to_string(m_temp_counter++);
        const std::string fault = "memory_alignment_fault_" + suffix;
        const std::string aligned = "memory_alignment_ok_" + suffix;
        m_out += "  " + bits + " = and i32 " + address + ", " +
                 std::to_string(width - 1u) + "\n";
        m_out += "  " + misaligned + " = icmp ne i32 " + bits + ", 0\n";
        std::set<uint8_t> dirty(m_dirty_regs.begin(), m_dirty_regs.end());
        store_dirty_regs(dirty);
        reset_tracking();
        m_out += "  br i1 " + misaligned + ", label %" + fault +
                 ", label %" + aligned + "\n";
        m_out += fault + ":\n";
        m_out += "  call i32 @ee_raise_exception(ptr %cpu, ptr %rt, i32 " +
                 std::to_string(excode) + ", i32 " +
                 std::to_string(ins.pc) + ", i32 " +
                 std::to_string(in_delay_slot ? 1u : 0u) + ", i32 " +
                 address + ")\n";
        m_out += "  ret i32 1\n"; // EE_EXIT_EXCEPTION
        m_out += aligned + ":\n";
    };

    // Precise conditional exception: publish prior SSA state before
    // exception entry, but do not commit a faulting arithmetic result.
    const auto emit_checked_exception =
        [&](const std::string& condition, uint32_t excode)
    {
        const std::set<uint8_t> dirty(m_dirty_regs.begin(), m_dirty_regs.end());
        store_dirty_regs(dirty);
        reset_tracking();
        const std::string suffix = std::to_string(m_temp_counter++);
        const std::string fault = "checked_fault_" + suffix;
        const std::string normal = "checked_continue_" + suffix;
        m_out += "  br i1 " + condition + ", label %" + fault +
                 ", label %" + normal + "\n";
        m_out += fault + ":\n";
        m_out += "  call i32 @ee_raise_exception(ptr %cpu, ptr %rt, i32 " +
                 std::to_string(excode) + ", i32 " + std::to_string(ins.pc) +
                 ", i32 " + std::to_string(in_delay_slot ? 1u : 0u) +
                 ", i32 0)\n";
        m_out += "  ret i32 1\n";
        m_out += normal + ":\n";
    };

    switch (ins.opcode)
    {
    case ee::e_ee_opcode::OP_ADDI:
    case ee::e_ee_opcode::OP_DADDI:
    {
        // Signed 32-bit ADDI and 64-bit DADDI overflow before the write.
        const bool word = ins.opcode == ee::e_ee_opcode::OP_ADDI;
        const std::string lhs = word
            ? trunc32_sext64(src_lo(ins.rs)) : src_lo(ins.rs);
        const std::string rhs = std::to_string(ins.immediate);
        const std::string overflow = newtemp();
        if (word)
        {
            const std::string wide = newtemp();
            m_out += "  " + wide + " = add i64 " + lhs + ", " + rhs + "\n";
            const std::string result = trunc32_sext64(wide);
            m_out += "  " + overflow + " = icmp ne i64 " +
                     result + ", " + wide + "\n";
            emit_checked_exception(overflow, 12u);
            set_gpr_lo64(ins.rt, result);
        }
        else
        {
            const std::string source_wide = newtemp();
            const std::string wide = newtemp();
            const std::string result = newtemp();
            const std::string result_wide = newtemp();
            m_out += "  " + source_wide + " = sext i64 " + lhs + " to i128\n";
            m_out += "  " + wide + " = add i128 " + source_wide + ", " + rhs + "\n";
            m_out += "  " + result + " = trunc i128 " + wide + " to i64\n";
            m_out += "  " + result_wide + " = sext i64 " + result + " to i128\n";
            m_out += "  " + overflow + " = icmp ne i128 " +
                     wide + ", " + result_wide + "\n";
            emit_checked_exception(overflow, 12u);
            set_gpr_lo64(ins.rt, result);
        }
        break;
    }
    case ee::e_ee_opcode::OP_ADDIU:
    {
        // 32-bit add, result sign-extended into low 64; upper 64 preserved.
        // PCSX2: UD[0] = (s64)(s32)(UL[0] + Imm)
        std::string d = newtemp();
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        m_out += "  " + d + " = add i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        set_gpr_lo32_sext(ins.rt, d);
        break;
    }
    case ee::e_ee_opcode::OP_DADDIU:
    {
        // Full 64-bit add; upper 64 preserved.
        std::string d = newtemp();
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        m_out += "  " + d + " = add i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        set_gpr_lo64(ins.rt, d);
        break;
    }
    case ee::e_ee_opcode::OP_ORI:
    {
        // Zero-extend 16-bit imm, OR into full 64; upper 64 preserved.
        std::string d = newtemp();
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%u", ins.uimmediate);
        m_out += "  " + d + " = or i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        set_gpr_lo64(ins.rt, d);
        break;
    }
    case ee::e_ee_opcode::OP_ANDI:
    {
        std::string d = newtemp();
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%u", ins.uimmediate);
        m_out += "  " + d + " = and i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        set_gpr_lo64(ins.rt, d);
        break;
    }
    case ee::e_ee_opcode::OP_XORI:
    {
        std::string d = newtemp();
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%u", ins.uimmediate);
        m_out += "  " + d + " = xor i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        set_gpr_lo64(ins.rt, d);
        break;
    }
    case ee::e_ee_opcode::OP_LUI:
    {
        // LUI writes a 32-bit constant << 16, sign-extended into low 64.
        std::string d = newtemp();
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%u", ins.uimmediate);
        m_out += "  " + d + " = shl i64 " + imm + ", 16\n";
        set_gpr_lo32_sext(ins.rt, d);
        break;
    }
    case ee::e_ee_opcode::OP_LB:
    case ee::e_ee_opcode::OP_LBU:
    case ee::e_ee_opcode::OP_LH:
    case ee::e_ee_opcode::OP_LHU:
    case ee::e_ee_opcode::OP_LW:
    case ee::e_ee_opcode::OP_LWU:
    case ee::e_ee_opcode::OP_LD:
    {
        // Integer loads: addr = trunc32(rs + sext16(imm)); result into low 64,
        // upper 64 preserved (except full 64-bit LD still only writes lo64).
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        std::string sum = newtemp();
        std::string addr = newtemp();
        m_out += "  " + sum + " = add i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        m_out += "  " + addr + " = trunc i64 " + sum + " to i32\n";
        const uint32_t width =
            ins.opcode == ee::e_ee_opcode::OP_LH ||
            ins.opcode == ee::e_ee_opcode::OP_LHU ? 2u :
            ins.opcode == ee::e_ee_opcode::OP_LD ? 8u :
            ins.opcode == ee::e_ee_opcode::OP_LW ||
            ins.opcode == ee::e_ee_opcode::OP_LWU ? 4u : 1u;
        if (width != 1u)
        {
            guard_memory_alignment(addr, width, 4u); // AdEL
        }

        if (ins.opcode == ee::e_ee_opcode::OP_LB ||
            ins.opcode == ee::e_ee_opcode::OP_LBU) {
            std::string b = newtemp();
            std::string d = newtemp();
            m_out += "  " + b + " = call i8 @ee_mem_read8(ptr %rt, i32 " + addr + ")\n";
            if (ins.opcode == ee::e_ee_opcode::OP_LB)
                m_out += "  " + d + " = sext i8 " + b + " to i64\n";
            else
                m_out += "  " + d + " = zext i8 " + b + " to i64\n";
            set_gpr_lo64(ins.rt, d);
        } else if (ins.opcode == ee::e_ee_opcode::OP_LH ||
                   ins.opcode == ee::e_ee_opcode::OP_LHU) {
            std::string h = newtemp();
            std::string d = newtemp();
            m_out += "  " + h + " = call i16 @ee_mem_read16(ptr %rt, i32 " + addr + ")\n";
            if (ins.opcode == ee::e_ee_opcode::OP_LH)
                m_out += "  " + d + " = sext i16 " + h + " to i64\n";
            else
                m_out += "  " + d + " = zext i16 " + h + " to i64\n";
            set_gpr_lo64(ins.rt, d);
        } else if (ins.opcode == ee::e_ee_opcode::OP_LD) {
            std::string d = newtemp();
            m_out += "  " + d + " = call i64 @ee_mem_read64(ptr %rt, i32 " + addr + ")\n";
            set_gpr_lo64(ins.rt, d);
        } else {
            // LW / LWU
            std::string w = newtemp();
            std::string d = newtemp();
            m_out += "  " + w + " = call i32 @ee_mem_read32(ptr %rt, i32 " + addr + ")\n";
            if (ins.opcode == ee::e_ee_opcode::OP_LWU)
                m_out += "  " + d + " = zext i32 " + w + " to i64\n";
            else
                m_out += "  " + d + " = sext i32 " + w + " to i64\n";
            set_gpr_lo64(ins.rt, d);
        }
        break;
    }
    case ee::e_ee_opcode::OP_SB:
    case ee::e_ee_opcode::OP_SH:
    case ee::e_ee_opcode::OP_SW:
    case ee::e_ee_opcode::OP_SD:
    {
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        std::string sum = newtemp();
        std::string addr = newtemp();
        m_out += "  " + sum + " = add i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        m_out += "  " + addr + " = trunc i64 " + sum + " to i32\n";
        const uint32_t width =
            ins.opcode == ee::e_ee_opcode::OP_SB ? 1u :
            ins.opcode == ee::e_ee_opcode::OP_SH ? 2u :
            ins.opcode == ee::e_ee_opcode::OP_SD ? 8u : 4u;
        if (width != 1u)
        {
            guard_memory_alignment(addr, width, 5u); // AdES
        }
        emit_guest_store_trace(ins, addr, addr, width, src_lo(ins.rt), 0u);
        if (ins.opcode == ee::e_ee_opcode::OP_SB) {
            std::string v = newtemp();
            m_out += "  " + v + " = trunc i64 " + src_lo(ins.rt) + " to i8\n";
            m_out += "  call void @ee_mem_write8(ptr %rt, i32 " + addr + ", i8 " + v + ")\n";
        } else if (ins.opcode == ee::e_ee_opcode::OP_SH) {
            std::string v = newtemp();
            m_out += "  " + v + " = trunc i64 " + src_lo(ins.rt) + " to i16\n";
            m_out += "  call void @ee_mem_write16(ptr %rt, i32 " + addr + ", i16 " + v + ")\n";
        } else if (ins.opcode == ee::e_ee_opcode::OP_SD) {
            m_out += "  call void @ee_mem_write64(ptr %rt, i32 " + addr + ", i64 " +
                     src_lo(ins.rt) + ")\n";
        } else {
            std::string v = newtemp();
            m_out += "  " + v + " = trunc i64 " + src_lo(ins.rt) + " to i32\n";
            m_out += "  call void @ee_mem_write32(ptr %rt, i32 " + addr + ", i32 " + v + ")\n";
        }
        break;
    }
    case ee::e_ee_opcode::OP_LQ:
    {
        // Load quadword into full 128-bit GPR (lo + hi).
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        std::string sum = newtemp();
        std::string raw_addr = newtemp();
        std::string addr = newtemp();
        std::string addr8 = newtemp();
        std::string vlo = newtemp();
        std::string vhi = newtemp();
        m_out += "  " + sum + " = add i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        m_out += "  " + raw_addr + " = trunc i64 " + sum + " to i32\n";
        m_out += "  " + addr + " = and i32 " + raw_addr + ", -16\n";
        m_out += "  " + vlo + " = call i64 @ee_mem_read64(ptr %rt, i32 " + addr + ")\n";
        m_out += "  " + addr8 + " = add i32 " + addr + ", 8\n";
        m_out += "  " + vhi + " = call i64 @ee_mem_read64(ptr %rt, i32 " + addr8 + ")\n";
        set_gpr_temp(ins.rt, vlo, vhi);
        break;
    }
    case ee::e_ee_opcode::OP_SQ:
    {
        // EE LQ/SQ mask the low four effective-address bits, unlike
        // LQC2/SQC2. Retain the original address in store diagnostics.
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        std::string sum = newtemp();
        std::string raw_addr = newtemp();
        std::string raw_addr8 = newtemp();
        std::string addr = newtemp();
        std::string addr8 = newtemp();
        m_out += "  " + sum + " = add i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        m_out += "  " + raw_addr + " = trunc i64 " + sum + " to i32\n";
        m_out += "  " + addr + " = and i32 " + raw_addr + ", -16\n";
        emit_guest_store_trace(ins, raw_addr, addr, 8u, src_lo(ins.rt), 0u);
        m_out += "  call void @ee_mem_write64(ptr %rt, i32 " + addr + ", i64 " +
                 src_lo(ins.rt) + ")\n";
        m_out += "  " + addr8 + " = add i32 " + addr + ", 8\n";
        m_out += "  " + raw_addr8 + " = add i32 " + raw_addr + ", 8\n";
        emit_guest_store_trace(ins, raw_addr8, addr8, 8u, src_hi(ins.rt), 1u);
        m_out += "  call void @ee_mem_write64(ptr %rt, i32 " + addr8 + ", i64 " +
                 src_hi(ins.rt) + ")\n";
        break;
    }
    case ee::e_ee_opcode::OP_LWL:
    case ee::e_ee_opcode::OP_LWR:
    case ee::e_ee_opcode::OP_LDL:
    case ee::e_ee_opcode::OP_LDR:
    {
        // Exact R5900 partial-load merge: the low GPR word/doubleword
        // retains the unaffected lanes. Upper GPR 64 bits are preserved.
        const char* name =
            ins.opcode == ee::e_ee_opcode::OP_LWL ? "lwl" :
            ins.opcode == ee::e_ee_opcode::OP_LWR ? "lwr" :
            ins.opcode == ee::e_ee_opcode::OP_LDL ? "ldl" : "ldr";
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        std::string sum = newtemp();
        std::string addr = newtemp();
        std::string merged = newtemp();
        m_out += "  " + sum + " = add i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        m_out += "  " + addr + " = trunc i64 " + sum + " to i32\n";
        m_out += "  " + merged + " = call i64 @ee_mem_" + name +
                 "(ptr %rt, i32 " + addr + ", i64 " + src_lo(ins.rt) + ")\n";
        set_gpr_lo64(ins.rt, merged);
        break;
    }
    case ee::e_ee_opcode::OP_SWL:
    case ee::e_ee_opcode::OP_SWR:
    case ee::e_ee_opcode::OP_SDL:
    case ee::e_ee_opcode::OP_SDR:
    {
        // Read-modify-write at the aligned containing word/doubleword.
        const char* name =
            ins.opcode == ee::e_ee_opcode::OP_SWL ? "swl" :
            ins.opcode == ee::e_ee_opcode::OP_SWR ? "swr" :
            ins.opcode == ee::e_ee_opcode::OP_SDL ? "sdl" : "sdr";
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        std::string sum = newtemp();
        std::string addr = newtemp();
        m_out += "  " + sum + " = add i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        m_out += "  " + addr + " = trunc i64 " + sum + " to i32\n";
        const bool is_word =
            ins.opcode == ee::e_ee_opcode::OP_SWL ||
            ins.opcode == ee::e_ee_opcode::OP_SWR;
        const std::string aligned = newtemp();
        m_out += "  " + aligned + " = and i32 " + addr +
                 (is_word ? ", -4\n" : ", -8\n");
        emit_guest_store_trace(ins, addr, aligned, is_word ? 4u : 8u,
                               src_lo(ins.rt), 0u);
        m_out += std::string("  call void @ee_mem_") + name + "(ptr %rt, i32 " + addr +
                 ", i64 " + src_lo(ins.rt) + ")\n";
        break;
    }
    case ee::e_ee_opcode::OP_LQC2:
    {
        // LQC2 transfers a full 128-bit vector register, not a scalar dword.
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        std::string sum = newtemp();
        std::string addr = newtemp();
        std::string addr8 = newtemp();
        std::string vlo = newtemp();
        std::string vhi = newtemp();
        m_out += "  " + sum + " = add i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        m_out += "  " + addr + " = trunc i64 " + sum + " to i32\n";
        guard_memory_alignment(addr, 16u, 4u); // EX_AdEL
        m_out += "  " + addr8 + " = add i32 " + addr + ", 8\n";
        m_out += "  " + vlo + " = call i64 @ee_mem_read64(ptr %rt, i32 " + addr + ")\n";
        m_out += "  " + vhi + " = call i64 @ee_mem_read64(ptr %rt, i32 " + addr8 + ")\n";
        // VF00 is hardwired to (0,0,0,1). The access may still trap
        // or read mapped memory, but it may not mutate the VF0 slot.
        if (ins.rt != 0u)
        {
            m_out += "  store i64 " + vlo + ", ptr " + vu0_lo_ptr(ins.rt) + "\n";
            m_out += "  store i64 " + vhi + ", ptr " + vu0_hi_ptr(ins.rt) + "\n";
        }
        break;
    }
    case ee::e_ee_opcode::OP_SQC2:
    {
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        std::string sum = newtemp();
        std::string addr = newtemp();
        std::string addr8 = newtemp();
        std::string vlo = newtemp();
        std::string vhi = newtemp();
        m_out += "  " + sum + " = add i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        m_out += "  " + addr + " = trunc i64 " + sum + " to i32\n";
        guard_memory_alignment(addr, 16u, 5u); // EX_AdES
        m_out += "  " + addr8 + " = add i32 " + addr + ", 8\n";
        // SQC2 VF00 always stores (0,0,0,1), even if the host backing
        // VF0 bytes are stale. Only VF1..31 are backed by storage.
        if (ins.rt == 0u)
        {
            m_out += "  " + vlo + " = add i64 0, 0\n";
            m_out += "  " + vhi + " = add i64 4575657221408423936, 0\n";
        }
        else
        {
            m_out += "  " + vlo + " = load i64, ptr " + vu0_lo_ptr(ins.rt) + "\n";
            m_out += "  " + vhi + " = load i64, ptr " + vu0_hi_ptr(ins.rt) + "\n";
        }
        emit_guest_store_trace(ins, addr, addr, 8u, vlo, 0u);
        m_out += "  call void @ee_mem_write64(ptr %rt, i32 " + addr + ", i64 " + vlo + ")\n";
        emit_guest_store_trace(ins, addr8, addr8, 8u, vhi, 1u);
        m_out += "  call void @ee_mem_write64(ptr %rt, i32 " + addr8 + ", i64 " + vhi + ")\n";
        break;
    }
    case ee::e_ee_opcode::OP_CACHE:
    case ee::e_ee_opcode::OP_PREF:
        m_out += "  ; cache/pref nop\n";
        break;
    case ee::e_ee_opcode::OP_SLTI:
    case ee::e_ee_opcode::OP_SLTIU:
    {
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        std::string cmp = newtemp();
        std::string d = newtemp();
        if (ins.opcode == ee::e_ee_opcode::OP_SLTI)
            m_out += "  " + cmp + " = icmp slt i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        else {
            // SLTIU: imm is sign-extended then compared as unsigned (MIPS rule).
            m_out += "  " + cmp + " = icmp ult i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        }
        m_out += "  " + d + " = zext i1 " + cmp + " to i64\n";
        set_gpr_lo64(ins.rt, d);
        break;
    }
    case ee::e_ee_opcode::OP_REGIMM:
    {
        auto rt = static_cast<ee::e_ee_opcode>(ins.rt);
        if (rt == ee::e_ee_opcode::REGIMM_MTSAB ||
            rt == ee::e_ee_opcode::REGIMM_MTSAH)
        {
            // MTSAB: ((GPR[rs] low 4 bits XOR immediate low 4 bits) * 8).
            // MTSAH: ((GPR[rs] low 3 bits XOR immediate low 3 bits) * 16).
            const bool is_byte = rt == ee::e_ee_opcode::REGIMM_MTSAB;
            const uint32_t lane_mask = is_byte ? 0x0fu : 0x07u;
            const uint32_t bit_shift = is_byte ? 3u : 4u;
            const std::string source = newtemp();
            const std::string shifted = newtemp();
            const std::string result = newtemp();
            const std::string result32 = newtemp();
            const std::string sa_ptr = state_field(5);
            m_out += "  " + source + " = and i64 " + src_lo(ins.rs) + ", " +
                     std::to_string(lane_mask) + "\n";
            m_out += "  " + shifted + " = xor i64 " + source + ", " +
                     std::to_string(ins.uimmediate & lane_mask) + "\n";
            m_out += "  " + result + " = shl i64 " + shifted + ", " +
                     std::to_string(bit_shift) + "\n";
            m_out += "  " + result32 + " = trunc i64 " + result + " to i32\n";
            m_out += "  store i32 " + result32 + ", ptr " + sa_ptr + "\n";
            break;
        }
        // Any REGIMM encoding not explicitly implemented below is not emitted
        // as a NOP. Decoder status normally exits before this switch for it.
        if ((ins.rt != 0x00 && ins.rt != 0x01 && ins.rt != 0x02 && ins.rt != 0x03 &&
             ins.rt != 0x08 && ins.rt != 0x09 && ins.rt != 0x0a && ins.rt != 0x0b &&
             ins.rt != 0x0c && ins.rt != 0x0e &&
             ins.rt != 0x10 && ins.rt != 0x11 && ins.rt != 0x12 && ins.rt != 0x13 &&
             ins.rt != 0x18 && ins.rt != 0x19)) {
            m_out += "  ; regimm rt=0x" + u32hex(ins.rt) + " reserved/nop\n";
            break;
        }
        // Immediate traps: conditional raise EX_Tr (code 13).
        if (rt == ee::e_ee_opcode::REGIMM_TGEI || rt == ee::e_ee_opcode::REGIMM_TGEIU ||
            rt == ee::e_ee_opcode::REGIMM_TLTI || rt == ee::e_ee_opcode::REGIMM_TLTIU ||
            rt == ee::e_ee_opcode::REGIMM_TEQI || rt == ee::e_ee_opcode::REGIMM_TNEI) {
            char imm[32];
            std::snprintf(imm, sizeof(imm), "%d", (int)ins.immediate);
            std::string imm64 = newtemp();
            std::string cmp = newtemp();
            m_out += "  " + imm64 + " = sext i32 " + imm + " to i64\n";
            const char* pred = "eq";
            if (rt == ee::e_ee_opcode::REGIMM_TGEI) pred = "sge";
            else if (rt == ee::e_ee_opcode::REGIMM_TGEIU) pred = "uge";
            else if (rt == ee::e_ee_opcode::REGIMM_TLTI) pred = "slt";
            else if (rt == ee::e_ee_opcode::REGIMM_TLTIU) pred = "ult";
            else if (rt == ee::e_ee_opcode::REGIMM_TNEI) pred = "ne";
            m_out += "  " + cmp + " = icmp " + std::string(pred) + " i64 " +
                     src_lo(ins.rs) + ", " + imm64 + "\n";
            emit_checked_exception(cmp, 13u);
            break;
        }
        // Branches: BLTZ/BGEZ family including link (*AL) and likely (*L/*ALL).
        const bool is_gez =
            (rt == ee::e_ee_opcode::REGIMM_BGEZ ||
             rt == ee::e_ee_opcode::REGIMM_BGEZL ||
             rt == ee::e_ee_opcode::REGIMM_BGEZAL ||
             rt == ee::e_ee_opcode::REGIMM_BGEZALL);
        const bool is_link =
            (rt == ee::e_ee_opcode::REGIMM_BLTZAL ||
             rt == ee::e_ee_opcode::REGIMM_BGEZAL ||
             rt == ee::e_ee_opcode::REGIMM_BLTZALL ||
             rt == ee::e_ee_opcode::REGIMM_BGEZALL);
        if (is_link) {
            std::string d = newtemp();
            char ra[32];
            std::snprintf(ra, sizeof(ra), "%u", (unsigned)(ins.pc + 8));
            m_out += "  " + d + " = add i64 " + ra + ", 0\n";
            set_gpr_lo64(31, d);
        }
        std::string cmp = newtemp();
        if (is_gez)
            m_out += "  " + cmp + " = icmp sge i64 " + src_lo(ins.rs) + ", 0\n";
        else
            m_out += "  " + cmp + " = icmp slt i64 " + src_lo(ins.rs) + ", 0\n";
        if (is_last_in_block) {
            m_out += "  br i1 " + cmp + ", label %" + block_label(ins.branch_target)
                   + ", label %" + block_label(fallthrough_pc) + "\n";
        }
        break;
    }
    case ee::e_ee_opcode::OP_BEQ:
    case ee::e_ee_opcode::OP_BEQL:
    {
        std::string cmp = newtemp();
        m_out += "  " + cmp + " = icmp eq i64 " + src_lo(ins.rs) + ", "
               + src_lo(ins.rt) + "\n";
        if (is_last_in_block) {
            m_out += "  br i1 " + cmp + ", label %" + block_label(ins.branch_target)
                   + ", label %" + block_label(fallthrough_pc) + "\n";
        }
        break;
    }
    case ee::e_ee_opcode::OP_BNE:
    case ee::e_ee_opcode::OP_BNEL:
    {
        std::string cmp = newtemp();
        m_out += "  " + cmp + " = icmp ne i64 " + src_lo(ins.rs) + ", "
               + src_lo(ins.rt) + "\n";
        if (is_last_in_block) {
            m_out += "  br i1 " + cmp + ", label %" + block_label(ins.branch_target)
                   + ", label %" + block_label(fallthrough_pc) + "\n";
        }
        break;
    }
    case ee::e_ee_opcode::OP_BLEZ:
    case ee::e_ee_opcode::OP_BLEZL:
    {
        std::string cmp = newtemp();
        m_out += "  " + cmp + " = icmp sle i64 " + src_lo(ins.rs) + ", 0\n";
        if (is_last_in_block) {
            m_out += "  br i1 " + cmp + ", label %" + block_label(ins.branch_target)
                   + ", label %" + block_label(fallthrough_pc) + "\n";
        }
        break;
    }
    case ee::e_ee_opcode::OP_BGTZ:
    case ee::e_ee_opcode::OP_BGTZL:
    {
        std::string cmp = newtemp();
        m_out += "  " + cmp + " = icmp sgt i64 " + src_lo(ins.rs) + ", 0\n";
        if (is_last_in_block) {
            m_out += "  br i1 " + cmp + ", label %" + block_label(ins.branch_target)
                   + ", label %" + block_label(fallthrough_pc) + "\n";
        }
        break;
    }
    case ee::e_ee_opcode::OP_J:
        if (is_last_in_block) {
            m_out += "  br label %" + block_label(ins.jump_target) + "\n";
        }
        break;
    case ee::e_ee_opcode::OP_JAL:
    {
        // Fallback for a missing delay-slot instruction: preserve the link
        // and dispatch to the direct call target.
        std::string d = newtemp();
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%u", (unsigned)(ins.pc + 8u));
        m_out += "  " + d + " = add i64 " + imm + ", 0\n";
        set_gpr_lo64(31, d);
        if (is_last_in_block)
        {
            const std::string pc_ptr = state_field(6);
            const std::string exit_ptr = state_field(7);
            m_out += "  store i32 " + std::to_string(ins.jump_target) + ", ptr " + pc_ptr + "\n";
            m_out += "  store i32 3, ptr " + exit_ptr + "\n";
            m_out += "  ret i32 3\n";
        }
        break;
    }
    case ee::e_ee_opcode::OP_SPECIAL:
    {
        auto funct = static_cast<ee::e_ee_opcode>(ins.funct);
        if (funct == ee::e_ee_opcode::FUNCT_JR)
        {
            if (is_last_in_block)
            {
                const std::string target = src_lo(ins.rs);
                const std::string target32 = newtemp();
                m_out += "  " + target32 + " = trunc i64 " + target + " to i32\n";
                const std::string pc_ptr = state_field(6);
                const std::string exit_ptr = state_field(7);
                const uint32_t exit_reason = ins.rs == 31 ? 0u : 3u;
                m_out += "  store i32 " + target32 + ", ptr " + pc_ptr + "\n";
                m_out += "  store i32 " + std::to_string(exit_reason) + ", ptr " + exit_ptr + "\n";
                m_out += "  ret i32 " + std::to_string(exit_reason) + "\n";
            }
        }
        else if (funct == ee::e_ee_opcode::FUNCT_JALR)
        {
            const std::string target = src_lo(ins.rs);
            std::string d = newtemp();
            char imm[32];
            std::snprintf(imm, sizeof(imm), "%u", (unsigned)(ins.pc + 8u));
            m_out += "  " + d + " = add i64 " + imm + ", 0\n";
            set_gpr_lo64(ins.rd, d);
            if (is_last_in_block)
            {
                const std::string target32 = newtemp();
                m_out += "  " + target32 + " = trunc i64 " + target + " to i32\n";
                const std::string pc_ptr = state_field(6);
                const std::string exit_ptr = state_field(7);
                m_out += "  store i32 " + target32 + ", ptr " + pc_ptr + "\n";
                m_out += "  store i32 3, ptr " + exit_ptr + "\n";
                m_out += "  ret i32 3\n";
            }
        }
        else if (funct == ee::e_ee_opcode::FUNCT_MFHI)
        {
            // MFHI moves the full 64-bit HI into the low doubleword; upper preserved.
            std::string ge = state_field(1);
            std::string v = newtemp();
            m_out += "  " + v + " = load i64, ptr " + ge + "\n";
            set_gpr_lo64(ins.rd, v);
        }
        else if (funct == ee::e_ee_opcode::FUNCT_MFLO)
        {
            std::string ge = state_field(2);
            std::string v = newtemp();
            m_out += "  " + v + " = load i64, ptr " + ge + "\n";
            set_gpr_lo64(ins.rd, v);
        }
        else if (funct == ee::e_ee_opcode::FUNCT_MTHI)
        {
            m_out += "  store i64 " + src_lo(ins.rs) + ", ptr " + state_field(1) + "\n";
        }
        else if (funct == ee::e_ee_opcode::FUNCT_MTLO)
        {
            m_out += "  store i64 " + src_lo(ins.rs) + ", ptr " + state_field(2) + "\n";
        }
        else if (funct == ee::e_ee_opcode::FUNCT_SYSCALL ||
                 funct == ee::e_ee_opcode::FUNCT_BREAK)
        {
            // Full exception entry via runtime helper.
            // EX_Sys = 8, EX_Bp = 9. BD=0 for the exception PC = this insn.
            const char* code =
                (funct == ee::e_ee_opcode::FUNCT_SYSCALL) ? "8" : "9";
            char pcbuf[32];
            std::snprintf(pcbuf, sizeof(pcbuf), "%u", (unsigned)ins.pc);
            std::string vec = newtemp();
            m_out += "  " + vec + " = call i32 @ee_raise_exception(ptr %cpu, ptr %rt, i32 " +
                     std::string(code) + ", i32 " + pcbuf + ", i32 0, i32 0)\n";
            // Exit translated function so the host can dispatch to the vector.
            m_out += "  ret i32 1\n"; // EE_EXIT_EXCEPTION
        }
        else if (funct == ee::e_ee_opcode::FUNCT_SLL ||
                 funct == ee::e_ee_opcode::FUNCT_SRL ||
                 funct == ee::e_ee_opcode::FUNCT_SRA ||
                 ins.funct == 0x04 || // SLLV (enum also names ROTR)
                 ins.funct == 0x06 || // SRLV (enum also names ROTRV)
                 ins.funct == 0x07)   // SRAV
        {
            // 32-bit shifts: operate on low 32 of rt (or rs for *V), result
            // sign-extended into low 64; upper 64 preserved.
            const bool variable = (ins.funct == 0x04 || ins.funct == 0x06 ||
                                   ins.funct == 0x07);
            std::string val32 = newtemp();
            m_out += "  " + val32 + " = trunc i64 " + src_lo(ins.rt) + " to i32\n";
            std::string shamt;
            if (variable) {
                std::string rs32 = newtemp();
                std::string mask = newtemp();
                m_out += "  " + rs32 + " = trunc i64 " + src_lo(ins.rs) + " to i32\n";
                m_out += "  " + mask + " = and i32 " + rs32 + ", 31\n";
                shamt = mask;
            } else {
                char sa[16];
                std::snprintf(sa, sizeof(sa), "%u", (unsigned)(ins.sa & 31));
                shamt = sa;
            }
            std::string r32 = newtemp();
            std::string r64 = newtemp();
            if (funct == ee::e_ee_opcode::FUNCT_SLL || ins.funct == 0x04)
                m_out += "  " + r32 + " = shl i32 " + val32 + ", " + shamt + "\n";
            else if (funct == ee::e_ee_opcode::FUNCT_SRA || ins.funct == 0x07)
                m_out += "  " + r32 + " = ashr i32 " + val32 + ", " + shamt + "\n";
            else
                m_out += "  " + r32 + " = lshr i32 " + val32 + ", " + shamt + "\n";
            m_out += "  " + r64 + " = sext i32 " + r32 + " to i64\n";
            set_gpr_lo64(ins.rd, r64);
        }
        else if (ins.funct == 0x38 || ins.funct == 0x3a || ins.funct == 0x3b ||
                 ins.funct == 0x3c || ins.funct == 0x3e || ins.funct == 0x3f ||
                 funct == ee::e_ee_opcode::FUNCT_DSLL ||
                 funct == ee::e_ee_opcode::FUNCT_DSRL ||
                 funct == ee::e_ee_opcode::FUNCT_DSRA)
        {
            // 64-bit shifts (DSLL/DSRL/DSRA and *32 variants).
            // Encoding (R5900/MIPS III): 0x38 DSLL, 0x3a DSRL, 0x3b DSRA,
            // 0x3c DSLL32, 0x3e DSRL32, 0x3f DSRA32.
            unsigned sa = ins.sa & 31;
            if (ins.funct == 0x3c || ins.funct == 0x3e || ins.funct == 0x3f)
                sa += 32;
            char sa_s[16];
            std::snprintf(sa_s, sizeof(sa_s), "%u", sa);
            std::string r = newtemp();
            if (ins.funct == 0x38 || ins.funct == 0x3c ||
                funct == ee::e_ee_opcode::FUNCT_DSLL)
                m_out += "  " + r + " = shl i64 " + src_lo(ins.rt) + ", " + sa_s + "\n";
            else if (ins.funct == 0x3b || ins.funct == 0x3f ||
                     funct == ee::e_ee_opcode::FUNCT_DSRA)
                m_out += "  " + r + " = ashr i64 " + src_lo(ins.rt) + ", " + sa_s + "\n";
            else
                m_out += "  " + r + " = lshr i64 " + src_lo(ins.rt) + ", " + sa_s + "\n";
            set_gpr_lo64(ins.rd, r);
        }
        else if (funct == ee::e_ee_opcode::FUNCT_MULT ||
                 funct == ee::e_ee_opcode::FUNCT_MULTU)
        {
            // 32x32 -> 64 product into HI:LO (each half sign-extended to 64).
            std::string a32 = newtemp();
            std::string b32 = newtemp();
            std::string a64 = newtemp();
            std::string b64 = newtemp();
            std::string prod = newtemp();
            std::string lo32 = newtemp();
            std::string hi32 = newtemp();
            std::string lo64 = newtemp();
            std::string hi64 = newtemp();
            m_out += "  " + a32 + " = trunc i64 " + src_lo(ins.rs) + " to i32\n";
            m_out += "  " + b32 + " = trunc i64 " + src_lo(ins.rt) + " to i32\n";
            if (funct == ee::e_ee_opcode::FUNCT_MULT) {
                m_out += "  " + a64 + " = sext i32 " + a32 + " to i64\n";
                m_out += "  " + b64 + " = sext i32 " + b32 + " to i64\n";
            } else {
                m_out += "  " + a64 + " = zext i32 " + a32 + " to i64\n";
                m_out += "  " + b64 + " = zext i32 " + b32 + " to i64\n";
            }
            m_out += "  " + prod + " = mul i64 " + a64 + ", " + b64 + "\n";
            m_out += "  " + lo32 + " = trunc i64 " + prod + " to i32\n";
            std::string shift = newtemp();
            m_out += "  " + shift + " = lshr i64 " + prod + ", 32\n";
            m_out += "  " + hi32 + " = trunc i64 " + shift + " to i32\n";
            m_out += "  " + lo64 + " = sext i32 " + lo32 + " to i64\n";
            m_out += "  " + hi64 + " = sext i32 " + hi32 + " to i64\n";
            m_out += "  store i64 " + lo64 + ", ptr " + state_field(2) + "\n";
            m_out += "  store i64 " + hi64 + ", ptr " + state_field(1) + "\n";
            // R5900 MULT/MULTU additionally write the low signed 32-bit
            // product into their encoded rd GPR. Unlike generic MIPS III,
            // this field is not reserved. It is used by SDBZ's SIFCMD
            // registration for its 12-byte handler entry offsets.
            // Source operands were captured before this write, so rd may
            // alias rs or rt; $zero remains hard-wired.
            set_gpr_lo64(ins.rd, lo64);
        }
        else if (funct == ee::e_ee_opcode::FUNCT_DIV ||
                 funct == ee::e_ee_opcode::FUNCT_DIVU)
        {
            // DIV/DIVU operate on low 32-bit words and sign-extend both HI
            // (remainder) and LO (quotient). LLVM division by zero and signed
            // INT_MIN / -1 are undefined, so neither case may reach sdiv/srem.
            // Although the manual leaves zero division undefined, hardware
            // observations (ps2autotests cpu/ee/muldiv.expected) give LO
            // -1 (signed negative dividend: +1), and HI = dividend.
            const bool is_unsigned = funct == ee::e_ee_opcode::FUNCT_DIVU;
            const std::string lhs32 = newtemp();
            const std::string rhs32 = newtemp();
            const std::string div_zero = newtemp();
            const std::string safe_rhs = newtemp();
            const std::string quotient_raw = newtemp();
            const std::string remainder_raw = newtemp();
            std::string quotient = quotient_raw;
            std::string remainder = remainder_raw;
            const std::string hi_ptr = state_field(1);
            const std::string lo_ptr = state_field(2);

            m_out += "  " + lhs32 + " = trunc i64 " + src_lo(ins.rs) + " to i32\n";
            m_out += "  " + rhs32 + " = trunc i64 " + src_lo(ins.rt) + " to i32\n";
            m_out += "  " + div_zero + " = icmp eq i32 " + rhs32 + ", 0\n";
            m_out += "  " + safe_rhs + " = select i1 " + div_zero +
                     ", i32 1, i32 " + rhs32 + "\n";
            if (is_unsigned)
            {
                m_out += "  " + quotient_raw + " = udiv i32 " + lhs32 + ", " + safe_rhs + "\n";
                m_out += "  " + remainder_raw + " = urem i32 " + lhs32 + ", " + safe_rhs + "\n";
            }
            else
            {
                // R5900 signed INT_MIN / -1 produces quotient INT_MIN and
                // remainder zero. Use safe operands to avoid LLVM poison.
                const std::string is_min = newtemp();
                const std::string is_neg_one = newtemp();
                const std::string overflow = newtemp();
                const std::string safe_lhs = newtemp();
                const std::string safe_signed_rhs = newtemp();
                quotient = newtemp();
                remainder = newtemp();
                m_out += "  " + is_min + " = icmp eq i32 " + lhs32 + ", -2147483648\n";
                m_out += "  " + is_neg_one + " = icmp eq i32 " + rhs32 + ", -1\n";
                m_out += "  " + overflow + " = and i1 " + is_min + ", " + is_neg_one + "\n";
                m_out += "  " + safe_lhs + " = select i1 " + overflow +
                         ", i32 0, i32 " + lhs32 + "\n";
                m_out += "  " + safe_signed_rhs + " = select i1 " + overflow +
                         ", i32 1, i32 " + safe_rhs + "\n";
                m_out += "  " + quotient_raw + " = sdiv i32 " +
                         safe_lhs + ", " + safe_signed_rhs + "\n";
                m_out += "  " + remainder_raw + " = srem i32 " +
                         safe_lhs + ", " + safe_signed_rhs + "\n";
                m_out += "  " + quotient + " = select i1 " + overflow +
                         ", i32 -2147483648, i32 " + quotient_raw + "\n";
                m_out += "  " + remainder + " = select i1 " + overflow +
                         ", i32 0, i32 " + remainder_raw + "\n";
            }

            const std::string quotient64 = newtemp();
            const std::string remainder64 = newtemp();
            std::string fallback = "-1";
            if (!is_unsigned)
            {
                const std::string negative = newtemp();
                fallback = newtemp();
                m_out += "  " + negative + " = icmp slt i32 " + lhs32 + ", 0\n";
                m_out += "  " + fallback + " = select i1 " + negative +
                         ", i32 1, i32 -1\n";
            }
            const std::string final_hi = newtemp();
            const std::string final_lo = newtemp();
            m_out += "  " + final_hi + " = select i1 " + div_zero +
                     ", i32 " + lhs32 + ", i32 " + remainder + "\n";
            m_out += "  " + final_lo + " = select i1 " + div_zero +
                     ", i32 " + fallback + ", i32 " + quotient + "\n";
            m_out += "  " + quotient64 + " = sext i32 " + final_lo + " to i64\n";
            m_out += "  " + remainder64 + " = sext i32 " + final_hi + " to i64\n";
            m_out += "  store i64 " + remainder64 + ", ptr " + hi_ptr + "\n";
            m_out += "  store i64 " + quotient64 + ", ptr " + lo_ptr + "\n";
        }
        else if (funct == ee::e_ee_opcode::FUNCT_SLT ||
                 funct == ee::e_ee_opcode::FUNCT_SLTU)
        {
            std::string cmp = newtemp();
            std::string d = newtemp();
            if (funct == ee::e_ee_opcode::FUNCT_SLT)
                m_out += "  " + cmp + " = icmp slt i64 " + src_lo(ins.rs) + ", " +
                         src_lo(ins.rt) + "\n";
            else
                m_out += "  " + cmp + " = icmp ult i64 " + src_lo(ins.rs) + ", " +
                         src_lo(ins.rt) + "\n";
            m_out += "  " + d + " = zext i1 " + cmp + " to i64\n";
            set_gpr_lo64(ins.rd, d);
        }
        else if (funct == ee::e_ee_opcode::FUNCT_MOVZ ||
                 funct == ee::e_ee_opcode::FUNCT_MOVN ||
                 funct == ee::e_ee_opcode::FUNCT_MOVNI)
        {
            // MOVZ: rd = rs if rt == 0; MOVN: rd = rs if rt != 0.
            std::string cmp = newtemp();
            std::string d = newtemp();
            if (funct == ee::e_ee_opcode::FUNCT_MOVZ)
                m_out += "  " + cmp + " = icmp eq i64 " + src_lo(ins.rt) + ", 0\n";
            else
                m_out += "  " + cmp + " = icmp ne i64 " + src_lo(ins.rt) + ", 0\n";
            m_out += "  " + d + " = select i1 " + cmp + ", i64 " + src_lo(ins.rs) +
                     ", i64 " + src_lo(ins.rd) + "\n";
            set_gpr_lo64(ins.rd, d);
        }
        else if (funct == ee::e_ee_opcode::FUNCT_SYNC)
        {
            m_out += "  ; sync (nop in static recompiler)\n";
        }
        else if (funct == ee::e_ee_opcode::FUNCT_TGE ||
                 funct == ee::e_ee_opcode::FUNCT_TGEU ||
                 funct == ee::e_ee_opcode::FUNCT_TLT ||
                 funct == ee::e_ee_opcode::FUNCT_TLTU ||
                 funct == ee::e_ee_opcode::FUNCT_TEQ ||
                 funct == ee::e_ee_opcode::FUNCT_TNE)
        {
            const char* pred = "eq";
            if (funct == ee::e_ee_opcode::FUNCT_TGE) pred = "sge";
            else if (funct == ee::e_ee_opcode::FUNCT_TGEU) pred = "uge";
            else if (funct == ee::e_ee_opcode::FUNCT_TLT) pred = "slt";
            else if (funct == ee::e_ee_opcode::FUNCT_TLTU) pred = "ult";
            else if (funct == ee::e_ee_opcode::FUNCT_TNE) pred = "ne";
            const std::string cmp = newtemp();
            m_out += "  " + cmp + " = icmp " + pred + " i64 " +
                     src_lo(ins.rs) + ", " + src_lo(ins.rt) + "\n";
            emit_checked_exception(cmp, 13u);
        }
        else if (funct == ee::e_ee_opcode::FUNCT_DADD ||
                 funct == ee::e_ee_opcode::FUNCT_DSUB)
        {
            // Exact signed i64 overflow detection without LLVM poison.
            const std::string lhs = src_lo(ins.rs);
            const std::string rhs = src_lo(ins.rt);
            const std::string lhs_wide = newtemp();
            const std::string rhs_wide = newtemp();
            const std::string wide = newtemp();
            const std::string result = newtemp();
            const std::string result_wide = newtemp();
            const std::string overflow = newtemp();
            const char* op = funct == ee::e_ee_opcode::FUNCT_DADD
                ? "add" : "sub";
            m_out += "  " + lhs_wide + " = sext i64 " + lhs + " to i128\n";
            m_out += "  " + rhs_wide + " = sext i64 " + rhs + " to i128\n";
            m_out += "  " + wide + " = " + op + " i128 " +
                     lhs_wide + ", " + rhs_wide + "\n";
            m_out += "  " + result + " = trunc i128 " + wide + " to i64\n";
            m_out += "  " + result_wide + " = sext i64 " + result + " to i128\n";
            m_out += "  " + overflow + " = icmp ne i128 " +
                     result_wide + ", " + wide + "\n";
            emit_checked_exception(overflow, 12u);
            set_gpr_lo64(ins.rd, result);
        }
        else if (funct == e_ee_opcode::FUNCT_ADD || funct == e_ee_opcode::FUNCT_SUB)
        {
            // Widen the signed low words before arithmetic. The exact sum or
            // difference fits in i64, including both signed-overflow cases.
            const std::string lhs = trunc32_sext64(src_lo(ins.rs));
            const std::string rhs = trunc32_sext64(src_lo(ins.rt));
            const std::string wide = newtemp();
            const char* operation = funct == e_ee_opcode::FUNCT_ADD ? "add" : "sub";
            m_out += "  " + wide + " = " + operation + " i64 " + lhs + ", " + rhs + "\n";
            const std::string result = trunc32_sext64(wide);
            const std::string overflow = newtemp();
            const std::string suffix = std::to_string(m_temp_counter++);
            m_out += "  " + overflow + " = icmp ne i64 " + result + ", " + wide + "\n";
            const std::set<uint8_t> dirty(m_dirty_regs.begin(), m_dirty_regs.end());
            store_dirty_regs(dirty);
            reset_tracking();
            m_out += "  br i1 " + overflow + ", label %word_overflow_" + suffix +
                     ", label %word_result_" + suffix + "\n";
            m_out += "word_overflow_" + suffix + ":\n";
            m_out += "  call i32 @ee_raise_exception(ptr %cpu, ptr %rt, i32 12, i32 " +
                     std::to_string(ins.pc) + ", i32 " +
                     std::to_string(in_delay_slot ? 1u : 0u) + ", i32 0)\n";
            m_out += "  ret i32 1\n";
            m_out += "word_result_" + suffix + ":\n";
            set_gpr_lo64(ins.rd, result);
        }
        else if (ins.funct == 0x14 || ins.funct == 0x16 || ins.funct == 0x17)
        {
            // DSLLV / DSRLV / DSRAV: 64-bit variable shift, amount = rs & 63.
            std::string sh = newtemp();
            std::string mask = newtemp();
            std::string r = newtemp();
            m_out += "  " + sh + " = trunc i64 " + src_lo(ins.rs) + " to i32\n";
            m_out += "  " + mask + " = and i32 " + sh + ", 63\n";
            std::string mask64 = newtemp();
            m_out += "  " + mask64 + " = zext i32 " + mask + " to i64\n";
            if (ins.funct == 0x14)
                m_out += "  " + r + " = shl i64 " + src_lo(ins.rt) + ", " + mask64 + "\n";
            else if (ins.funct == 0x17)
                m_out += "  " + r + " = ashr i64 " + src_lo(ins.rt) + ", " + mask64 + "\n";
            else
                m_out += "  " + r + " = lshr i64 " + src_lo(ins.rt) + ", " + mask64 + "\n";
            set_gpr_lo64(ins.rd, r);
        }
        else
        {
            auto a = src_lo(ins.rs);
            auto b = src_lo(ins.rt);
            std::string r = newtemp();
            const char* op = "add";
            bool is_32bit_arith = false;
            bool known = true;
            switch (funct)
            {
            case e_ee_opcode::FUNCT_ADDU:
                op = "add"; is_32bit_arith = true; break;
            case e_ee_opcode::FUNCT_SUBU:
                op = "sub"; is_32bit_arith = true; break;
            case e_ee_opcode::FUNCT_DADD:
            case e_ee_opcode::FUNCT_DADDU:
                op = "add"; is_32bit_arith = false; break; // full 64-bit
            case e_ee_opcode::FUNCT_DSUB:
            case e_ee_opcode::FUNCT_DSUBU:
                op = "sub"; is_32bit_arith = false; break;
            case e_ee_opcode::FUNCT_AND: op = "and"; break;
            case e_ee_opcode::FUNCT_OR:  op = "or";  break;
            case e_ee_opcode::FUNCT_XOR: op = "xor"; break;
            case e_ee_opcode::FUNCT_NOR: op = "or";  break;
            default:
                known = false;
                break;
            }
            if (!known)
            {
                m_out += "  ; special funct 0x" + u32hex(ins.funct) +
                         " not yet lowered\n";
            }
            else if (funct == e_ee_opcode::FUNCT_NOR)
            {
                std::string o = newtemp();
                m_out += "  " + o + " = or i64 " + a + ", " + b + "\n";
                m_out += "  " + r + " = xor i64 " + o + ", -1\n";
                set_gpr_lo64(ins.rd, r);
            }
            else
            {
                m_out += "  " + r + " = " + std::string(op) + " i64 " + a + ", " + b + "\n";
                if (is_32bit_arith)
                    set_gpr_lo32_sext(ins.rd, r); // 32-bit result, sext into low 64
                else
                    set_gpr_lo64(ins.rd, r);      // full 64-bit result
            }
        }
        break;
    }
    case ee::e_ee_opcode::OP_MMI:
        emit_mmi_insn(ins);
        break;
    case ee::e_ee_opcode::OP_COP0:
    {
        // COP0: rs/fmt selects class; rd = CP0 register, rt = GPR.
        // CP0 regs of interest: 12=Status, 13=Cause, 14=EPC, 9=Count, 11=Compare.
        const uint8_t fmt = ins.rs;
        const uint8_t cp0_reg = ins.rd;
        const uint8_t gpr = ins.rt;

        if (fmt == static_cast<uint8_t>(ee::e_ee_opcode::COP0_MFC0))
        {
            // GPR.rt = sext32(COP0[rd])
            std::string p = newtemp();
            std::string v32 = newtemp();
            std::string v64 = newtemp();
            m_out += "  " + p + " = " + cop0_ptr(cp0_reg) + "\n";
            m_out += "  " + v32 + " = load i32, ptr " + p + "\n";
            m_out += "  " + v64 + " = sext i32 " + v32 + " to i64\n";
            set_gpr_lo64(gpr, v64);
        }
        else if (fmt == static_cast<uint8_t>(ee::e_ee_opcode::COP0_MTC0))
        {
            // Guest MTC0 side effects are owned by the runtime (not a raw
            // store into m_cop0). In particular Compare writes clear IP7.
            const std::string v32 = newtemp();
            m_out += "  " + v32 + " = trunc i64 " + src_lo(gpr) + " to i32\n";
            m_out += "  call void @ee_cop0_write(ptr %cpu, i32 " +
                     std::to_string(cp0_reg) + ", i32 " + v32 + ")\n";
        }
        else if (fmt == static_cast<uint8_t>(ee::e_ee_opcode::COP0_BC0))
        {
            // BC0F/BC0T: condition from Cause / simple always-false stub.
            // Without a live COP0 condition bit, branch not-taken (conservative).
            if (is_last_in_block) {
                m_out += "  br label %" + block_label(fallthrough_pc) + "\n";
            }
        }
        else if (fmt == static_cast<uint8_t>(ee::e_ee_opcode::COP0_CO))
        {
            const uint8_t fn = ins.funct;
            if (fn == static_cast<uint8_t>(ee::e_ee_opcode::COP0_ERET))
            {
                // Full exception return: clear EXL, PC <- EPC, exit DISPATCH.
                std::string epc = newtemp();
                m_out += "  " + epc + " = call i32 @ee_exception_return(ptr %cpu)\n";
                m_out += "  ret i32 3\n"; // EE_EXIT_DISPATCH
            }
            else if (fn == static_cast<uint8_t>(ee::e_ee_opcode::COP0_EI))
            {
                // Enable interrupts: set Status.EIE (bit 16 on R5900).
                std::string sp = newtemp();
                std::string st = newtemp();
                std::string st2 = newtemp();
                m_out += "  " + sp + " = " + cop0_ptr(12) + "\n";
                m_out += "  " + st + " = load i32, ptr " + sp + "\n";
                m_out += "  " + st2 + " = or i32 " + st + ", 65536\n"; // EIE
                m_out += "  store i32 " + st2 + ", ptr " + sp + "\n";
            }
            else if (fn == static_cast<uint8_t>(ee::e_ee_opcode::COP0_DI))
            {
                // Disable interrupts: clear Status.EIE.
                std::string sp = newtemp();
                std::string st = newtemp();
                std::string st2 = newtemp();
                m_out += "  " + sp + " = " + cop0_ptr(12) + "\n";
                m_out += "  " + st + " = load i32, ptr " + sp + "\n";
                m_out += "  " + st2 + " = and i32 " + st + ", -65537\n";
                m_out += "  store i32 " + st2 + ", ptr " + sp + "\n";
            }
            else if (fn == static_cast<uint8_t>(ee::e_ee_opcode::COP0_TLBR) ||
                     fn == static_cast<uint8_t>(ee::e_ee_opcode::COP0_TLBWI) ||
                     fn == static_cast<uint8_t>(ee::e_ee_opcode::COP0_TLBWR) ||
                     fn == static_cast<uint8_t>(ee::e_ee_opcode::COP0_TLBP))
            {
                // TLB ops: no host TLB model yet — architectural no-op with note.
                m_out += "  ; cop0 tlb funct=0x" + u32hex(fn) + " (nop in static recompiler)\n";
            }
            else
            {
                m_out += "  ; cop0 co funct=0x" + u32hex(fn) + "\n";
            }
        }
        else
        {
            m_out += "  ; cop0 fmt=0x" + u32hex(fmt) + " not yet lowered\n";
        }
        break;
    }
    case ee::e_ee_opcode::OP_LWC1:
    case ee::e_ee_opcode::OP_SWC1:
    {
        // Primary LWC1/SWC1: base=rs, ft=rt, offset=imm.
        char imm[32];
        std::snprintf(imm, sizeof(imm), "%d", ins.immediate);
        std::string sum = newtemp();
        std::string addr = newtemp();
        m_out += "  " + sum + " = add i64 " + src_lo(ins.rs) + ", " + imm + "\n";
        m_out += "  " + addr + " = trunc i64 " + sum + " to i32\n";
        guard_memory_alignment(addr, 4u,
                               ins.opcode == ee::e_ee_opcode::OP_LWC1
                                   ? 4u : 5u);
        if (ins.opcode == ee::e_ee_opcode::OP_LWC1) {
            std::string w = newtemp();
            m_out += "  " + w + " = call i32 @ee_mem_read32(ptr %rt, i32 " + addr + ")\n";
            store_fpr_i32(ins.rt, w);
        } else {
            std::string w = load_fpr_i32(ins.rt);
            const std::string source64 = newtemp();
            m_out += "  " + source64 + " = zext i32 " + w + " to i64\n";
            emit_guest_store_trace(ins, addr, addr, 4u, source64, 0u);
            m_out += "  call void @ee_mem_write32(ptr %rt, i32 " + addr + ", i32 " + w + ")\n";
        }
        break;
    }
    case ee::e_ee_opcode::OP_COP1:
    {
        // MIPS COP1 encoding:
        //   bits 25-21 (rs/fmt): MFC1=0 CFC1=2 MTC1=4 CTC1=6 BC=8
        //                        S=16 D=17 W=20 L=21
        //   For moves: rt=GPR, rd=FS
        //   For arith: rt=FT, rd=FS, sa=FD, funct=op
        const uint8_t fmt = ins.rs;
        const uint8_t ft = ins.rt;
        const uint8_t fs = ins.rd;
        const uint8_t fd = ins.sa;
        const uint8_t fp_funct = ins.funct;

        if (fmt == static_cast<uint8_t>(ee::e_ee_opcode::COP1_MFC1))
        {
            // GPR.rt = sext32(FPR.fs)
            std::string bits = load_fpr_i32(fs);
            std::string sx = newtemp();
            m_out += "  " + sx + " = sext i32 " + bits + " to i64\n";
            set_gpr_lo64(ft, sx);
        }
        else if (fmt == static_cast<uint8_t>(ee::e_ee_opcode::COP1_MTC1))
        {
            // FPR.fs = low32(GPR.rt)
            std::string lo32 = newtemp();
            m_out += "  " + lo32 + " = trunc i64 " + src_lo(ft) + " to i32\n";
            store_fpr_i32(fs, lo32);
        }
        else if (fmt == static_cast<uint8_t>(ee::e_ee_opcode::COP1_CFC1))
        {
            // Hardware FCR read aliases: selectors 0..15 read FCR0,
            // 16..31 read FCR31. See ps2autotests ee_fpu/fcr.expected.
            if (fs < 16u)
            {
                set_gpr_lo64(ft, "11824"); // R5900 FPU revision 0x2e30
            }
            else
            {
                const std::string p = newtemp();
                const std::string raw = newtemp();
                const std::string masked = newtemp();
                const std::string value = newtemp();
                const std::string sx = newtemp();
                m_out += "  " + p + " = " + fcsr_ptr() + "\n";
                m_out += "  " + raw + " = load i32, ptr " + p + "\n";
                m_out += "  " + masked + " = and i32 " + raw + ", " +
                         std::to_string(0x0083c078u) + "\n";
                m_out += "  " + value + " = or i32 " + masked + ", " +
                         std::to_string(0x01000001u) + "\n";
                m_out += "  " + sx + " = zext i32 " + value + " to i64\n";
                set_gpr_lo64(ft, sx);
            }
        }
        else if (fmt == static_cast<uint8_t>(ee::e_ee_opcode::COP1_CTC1))
        {
            // FCR0 is read-only. FCR31 exposes only condition,
            // cause and sticky bits plus hardwired FS and RM bits.
            if (fs == 31u)
            {
                const std::string lo32 = newtemp();
                const std::string masked = newtemp();
                const std::string value = newtemp();
                const std::string p = newtemp();
                m_out += "  " + lo32 + " = trunc i64 " + src_lo(ft) + " to i32\n";
                m_out += "  " + masked + " = and i32 " + lo32 + ", " +
                         std::to_string(0x0083c078u) + "\n";
                m_out += "  " + value + " = or i32 " + masked + ", " +
                         std::to_string(0x01000001u) + "\n";
                m_out += "  " + p + " = " + fcsr_ptr() + "\n";
                m_out += "  store i32 " + value + ", ptr " + p + "\n";
            }
        }
        else if (fmt == 0x08)
        {
            // BC1: rt selects F/T and likely (bits of rt).
            // rt: 0=BC1F, 1=BC1T, 2=BC1FL, 3=BC1TL (common encoding).
            const bool branch_true = (ft & 1) != 0;
            std::string p = newtemp();
            std::string fcsr = newtemp();
            std::string fcc = newtemp();
            std::string cmp = newtemp();
            m_out += "  " + p + " = " + fcsr_ptr() + "\n";
            m_out += "  " + fcsr + " = load i32, ptr " + p + "\n";
            // FCC0 in bit 23 of FCSR (MIPS standard).
            m_out += "  " + fcc + " = and i32 " + fcsr + ", 8388608\n";
            if (branch_true)
                m_out += "  " + cmp + " = icmp ne i32 " + fcc + ", 0\n";
            else
                m_out += "  " + cmp + " = icmp eq i32 " + fcc + ", 0\n";
            if (is_last_in_block) {
                m_out += "  br i1 " + cmp + ", label %" + block_label(ins.branch_target)
                       + ", label %" + block_label(fallthrough_pc) + "\n";
            }
        }
        else if (fmt == 0x10 || fmt == 0x14)
        {
            // fmt S (16) or W (20). Encodings match PCSX2 tbl_COP1_S.
            auto store_acc_i32 = [&](const std::string& bits) {
                const std::string p = newtemp();
                m_out += "  " + p + " = " + fpu_acc_ptr() + "\n";
                m_out += "  store i32 " + bits + ", ptr " + p + "\n";
            };
            auto store_acc_overflow = [&](const std::string& overflow) {
                const std::string p = newtemp();
                const std::string word = newtemp();
                m_out += "  " + p + " = getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 14\n";
                m_out += "  " + word + " = zext i1 " + overflow + " to i32\n";
                m_out += "  store i32 " + word + ", ptr " + p + "\n";
            };
            auto set_fcc = [&](const std::string& cmp_i1) {
                std::string p = newtemp();
                std::string fcsr = newtemp();
                std::string cleared = newtemp();
                std::string bit = newtemp();
                std::string merged = newtemp();
                m_out += "  " + p + " = " + fcsr_ptr() + "\n";
                m_out += "  " + fcsr + " = load i32, ptr " + p + "\n";
                m_out += "  " + cleared + " = and i32 " + fcsr + ", -8388609\n";
                m_out += "  " + bit + " = select i1 " + cmp_i1 + ", i32 8388608, i32 0\n";
                m_out += "  " + merged + " = or i32 " + cleared + ", " + bit + "\n";
                m_out += "  store i32 " + merged + ", ptr " + p + "\n";
            };

            // ADD/SUB/MUL share the result and exception interface for
            // FPR and accumulator destinations.
            if (fmt == 0x10 && (fp_funct <= 0x02 ||
                                (fp_funct >= 0x18 && fp_funct <= 0x1a)))
            {
                const std::string lhs = load_fpr_i32(fs);
                const std::string rhs = load_fpr_i32(ft);
                const std::string tuple = newtemp();
                const std::string bits = newtemp();
                const std::string overflow = newtemp();
                const std::string underflow = newtemp();
                const bool subtract = fp_funct == 0x01 || fp_funct == 0x19;
                if (fp_funct == 0x02 || fp_funct == 0x1a)
                {
                    m_out += "  " + tuple + " = call { i32, i1, i1 } @ee_cop1_mul_bits(i32 " +
                             lhs + ", i32 " + rhs + ")\n";
                }
                else
                {
                    m_out += "  " + tuple + " = call { i32, i1, i1 } @ee_cop1_add_bits(i32 " +
                             lhs + ", i32 " + rhs + ", i1 " + (subtract ? "true" : "false") + ")\n";
                }
                m_out += "  " + bits + " = extractvalue { i32, i1, i1 } " + tuple + ", 0\n";
                m_out += "  " + overflow + " = extractvalue { i32, i1, i1 } " + tuple + ", 1\n";
                m_out += "  " + underflow + " = extractvalue { i32, i1, i1 } " + tuple + ", 2\n";

                const std::string fcsr = newtemp();
                const std::string old_flags = newtemp();
                const std::string set_o = newtemp();
                const std::string clear_o = newtemp();
                const std::string after_o = newtemp();
                const std::string set_u = newtemp();
                const std::string clear_u = newtemp();
                const std::string after_u = newtemp();
                const std::string final_flags = newtemp();
                m_out += "  " + fcsr + " = " + fcsr_ptr() + "\n";
                m_out += "  " + old_flags + " = load i32, ptr " + fcsr + "\n";
                m_out += "  " + set_o + " = or i32 " + old_flags + ", 32784\n";
                m_out += "  " + clear_o + " = and i32 " + old_flags + ", -32769\n";
                m_out += "  " + after_o + " = select i1 " + overflow +
                         ", i32 " + set_o + ", i32 " + clear_o + "\n";
                m_out += "  " + set_u + " = or i32 " + after_o + ", 16392\n";
                m_out += "  " + clear_u + " = and i32 " + after_o + ", -16385\n";
                m_out += "  " + after_u + " = select i1 " + underflow +
                         ", i32 " + set_u + ", i32 " + clear_u + "\n";
                m_out += "  " + final_flags + " = select i1 " + overflow +
                         ", i32 " + after_o + ", i32 " + after_u + "\n";
                m_out += "  store i32 " + final_flags + ", ptr " + fcsr + "\n";
                if (fp_funct >= 0x18)
                {
                    store_acc_i32(bits);
                    store_acc_overflow(overflow);
                }
                else
                {
                    store_fpr_i32(fd, bits);
                }
            }
            else if (fmt == 0x10 && fp_funct == 0x03) // DIV.S
            {
                // Divide-by-zero is detected from the raw divisor exponent;
                // denormals count as zero. The result is signed FMAX, and the
                // selected cause/sticky flags are ORed without clearing others.
                const std::string raw_fs = load_fpr_i32(fs);
                const std::string raw_ft = load_fpr_i32(ft);
                const std::string fs_exponent = newtemp();
                const std::string ft_exponent = newtemp();
                const std::string numerator_is_zero = newtemp();
                const std::string divisor_is_zero = newtemp();

                m_out += "  " + fs_exponent + " = and i32 " + raw_fs + ", 2139095040\n";
                m_out += "  " + ft_exponent + " = and i32 " + raw_ft + ", 2139095040\n";
                m_out += "  " + numerator_is_zero + " = icmp eq i32 " + fs_exponent + ", 0\n";
                m_out += "  " + divisor_is_zero + " = icmp eq i32 " + ft_exponent + ", 0\n";
                // Use the same signed-digit divider as RSQRT.S. This
                // retains exponent 255 and the hardware quotient rounding,
                // including signed zero and saturation to the EE maximum.
                const std::string result = newtemp();
                m_out += "  " + result + " = call i32 @ee_cop1_radix_quotient(i32 " +
                         raw_fs + ", i32 " + raw_ft + ")\n";
                store_fpr_i32(fd, result);

                const std::string divide_zero_flags = newtemp();
                const std::string old_flags = newtemp();
                const std::string flags_to_set = newtemp();
                const std::string flags_if_divide_zero = newtemp();
                const std::string fcsr = newtemp();
                m_out += "  " + divide_zero_flags + " = select i1 " + numerator_is_zero +
                         ", i32 131136, i32 65568\n";
                m_out += "  " + fcsr + " = " + fcsr_ptr() + "\n";
                m_out += "  " + old_flags + " = load i32, ptr " + fcsr + "\n";
                m_out += "  " + flags_to_set + " = or i32 " + old_flags + ", " + divide_zero_flags + "\n";
                m_out += "  " + flags_if_divide_zero + " = select i1 " + divisor_is_zero +
                         ", i32 " + flags_to_set + ", i32 " + old_flags + "\n";
                m_out += "  store i32 " + flags_if_divide_zero + ", ptr " + fcsr + "\n";
            }
            else if (fmt == 0x10 && fp_funct == 0x04) // SQRT.S fd, ft
            {
                const std::string raw = load_fpr_i32(ft);
                const std::string negative = newtemp();
                const std::string result = newtemp();
                m_out += "  " + negative + " = icmp slt i32 " + raw + ", 0\n";
                // The divide unit's signed-digit root is not always the
                // nearest IEEE result. Reuse the RSQRT.S root recurrence.
                m_out += "  " + result + " = call i32 @ee_cop1_radix_root(i32 " + raw + ")\n";
                store_fpr_i32(fd, result);

                // The R5900 checks Ft's raw sign before flushing exponent
                // zero. Negative zero/denormals set I|SI and return +0.
                const std::string fcsr = newtemp();
                const std::string old_flags = newtemp();
                const std::string cleared_flags = newtemp();
                const std::string invalid_flags = newtemp();
                const std::string final_flags = newtemp();
                m_out += "  " + fcsr + " = " + fcsr_ptr() + "\n";
                m_out += "  " + old_flags + " = load i32, ptr " + fcsr + "\n";
                m_out += "  " + cleared_flags + " = and i32 " + old_flags + ", -196609\n";
                m_out += "  " + invalid_flags + " = or i32 " + cleared_flags + ", 131136\n";
                m_out += "  " + final_flags + " = select i1 " + negative +
                         ", i32 " + invalid_flags + ", i32 " + cleared_flags + "\n";
                m_out += "  store i32 " + final_flags + ", ptr " + fcsr + "\n";
            }
            else if (fmt == 0x10 && fp_funct == 0x05) // ABS.S
            {
                // ABS.S is a raw sign-bit clear in the EE interpreter. Keep
                // NaN payloads and signalling encodings untouched.
                const std::string raw = load_fpr_i32(fs);
                const std::string result = newtemp();
                m_out += "  " + result + " = and i32 " + raw + ", 2147483647\n";
                store_fpr_i32(fd, result);

                // ABS.S clears overflow/underflow cause flags only.
                const std::string fcsr = newtemp();
                const std::string fcsr_value = newtemp();
                const std::string fcsr_updated = newtemp();
                m_out += "  " + fcsr + " = " + fcsr_ptr() + "\n";
                m_out += "  " + fcsr_value + " = load i32, ptr " + fcsr + "\n";
                m_out += "  " + fcsr_updated + " = and i32 " + fcsr_value + ", -49153\n";
                m_out += "  store i32 " + fcsr_updated + ", ptr " + fcsr + "\n";
            }
            else if (fmt == 0x10 && fp_funct == 0x06) // MOV.S
            {
                store_fpr_f32(fd, load_fpr_f32(fs));
            }
            else if (fmt == 0x10 && fp_funct == 0x07) // NEG.S
            {
                // NEG.S toggles only the sign bit, preserving every payload
                // bit (including NaN payloads and denormal encodings).
                const std::string raw = load_fpr_i32(fs);
                const std::string result = newtemp();
                m_out += "  " + result + " = xor i32 " + raw + ", -2147483648\n";
                store_fpr_i32(fd, result);

                // NEG.S clears overflow/underflow cause flags only.
                const std::string fcsr = newtemp();
                const std::string fcsr_value = newtemp();
                const std::string fcsr_updated = newtemp();
                m_out += "  " + fcsr + " = " + fcsr_ptr() + "\n";
                m_out += "  " + fcsr_value + " = load i32, ptr " + fcsr + "\n";
                m_out += "  " + fcsr_updated + " = and i32 " + fcsr_value + ", -49153\n";
                m_out += "  store i32 " + fcsr_updated + ", ptr " + fcsr + "\n";
            }
            else if (fmt == 0x10 && fp_funct == 0x16) // RSQRT.S  fd = fs / sqrt(ft)
            {
                const std::string raw_fs = load_fpr_i32(fs);
                const std::string raw_ft = load_fpr_i32(ft);
                const std::string fs_exponent = newtemp();
                const std::string numerator_is_zero = newtemp();
                const std::string ft_exponent = newtemp();
                const std::string divisor_is_zero = newtemp();
                const std::string sign = newtemp();
                const std::string negative = newtemp();
                m_out += "  " + ft_exponent + " = and i32 " + raw_ft + ", 2139095040\n";
                m_out += "  " + divisor_is_zero + " = icmp eq i32 " + ft_exponent + ", 0\n";
                m_out += "  " + sign + " = and i32 " + raw_ft + ", -2147483648\n";
                m_out += "  " + negative + " = icmp ne i32 " + sign + ", 0\n";
                // An exponent-zero numerator distinguishes invalid from divide-by-zero.
                m_out += "  " + fs_exponent + " = and i32 " + raw_fs + ", 2139095040\n";
                m_out += "  " + numerator_is_zero + " = icmp eq i32 " + fs_exponent + ", 0\n";
                // A host sqrt/div pair loses exponent-255 values and can
                // round the last significand bit differently. The R5900
                // recurrence works directly on the original guest words.
                const std::string result = newtemp();
                m_out += "  " + result + " = call i32 @ee_cop1_rsqrt_bits(i32 " +
                         raw_fs + ", i32 " + raw_ft + ")\n";
                store_fpr_i32(fd, result);

                // Negative Ft sets I independently of the zero test.
                // Zero/zero sets I; nonzero/zero sets D (PCSX2 interpreter).
                // Preserve existing cause/sticky policy; result saturation is
                // handled by the integer recurrence.
                const std::string fcsr = newtemp();
                const std::string old_flags = newtemp();
                const std::string cleared_flags = newtemp();
                const std::string negative_flags = newtemp();
                const std::string invalid_flags = newtemp();
                const std::string divide_flags = newtemp();
                const std::string zero_flags = newtemp();
                const std::string base_flags = newtemp();
                const std::string final_flags = newtemp();
                m_out += "  " + fcsr + " = " + fcsr_ptr() + "\n";
                m_out += "  " + old_flags + " = load i32, ptr " + fcsr + "\n";
                m_out += "  " + cleared_flags + " = and i32 " + old_flags + ", -196609\n";
                m_out += "  " + negative_flags + " = or i32 " + cleared_flags + ", 131136\n";
                m_out += "  " + base_flags + " = select i1 " + negative +
                         ", i32 " + negative_flags + ", i32 " + cleared_flags + "\n";
                m_out += "  " + invalid_flags + " = or i32 " + base_flags + ", 131136\n";
                m_out += "  " + divide_flags + " = or i32 " + base_flags + ", 65568\n";
                m_out += "  " + zero_flags + " = select i1 " + numerator_is_zero +
                         ", i32 " + invalid_flags + ", i32 " + divide_flags + "\n";
                m_out += "  " + final_flags + " = select i1 " + divisor_is_zero +
                         ", i32 " + zero_flags + ", i32 " + base_flags + "\n";
                m_out += "  store i32 " + final_flags + ", ptr " + fcsr + "\n";
            }
            else if (fmt == 0x10 && fp_funct >= 0x1c && fp_funct <= 0x1f)
            {
                const bool subtract = (fp_funct & 1u) != 0;
                const std::string lhs = load_fpr_i32(fs);
                const std::string rhs = load_fpr_i32(ft);
                const std::string acc_ptr = newtemp();
                const std::string acc = newtemp();
                const std::string latch_ptr = newtemp();
                const std::string latch_word = newtemp();
                const std::string latch = newtemp();
                const std::string tuple = newtemp();
                const std::string bits = newtemp();
                const std::string overflow = newtemp();
                const std::string underflow = newtemp();
                const std::string product_underflow = newtemp();
                m_out += "  " + acc_ptr + " = " + fpu_acc_ptr() + "\n";
                m_out += "  " + acc + " = load i32, ptr " + acc_ptr + "\n";
                m_out += "  " + latch_ptr + " = getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 14\n";
                m_out += "  " + latch_word + " = load i32, ptr " + latch_ptr + "\n";
                m_out += "  " + latch + " = icmp ne i32 " + latch_word + ", 0\n";
                m_out += "  " + tuple + " = call { i32, i1, i1, i1 } @ee_cop1_mac_bits(i32 " +
                         acc + ", i32 " + lhs + ", i32 " + rhs + ", i1 " +
                         (subtract ? "true" : "false") + ", i1 " + latch + ")\n";
                m_out += "  " + bits + " = extractvalue { i32, i1, i1, i1 } " + tuple + ", 0\n";
                m_out += "  " + overflow + " = extractvalue { i32, i1, i1, i1 } " + tuple + ", 1\n";
                m_out += "  " + underflow + " = extractvalue { i32, i1, i1, i1 } " + tuple + ", 2\n";
                m_out += "  " + product_underflow + " = extractvalue { i32, i1, i1, i1 } " + tuple + ", 3\n";

                // Product underflow contributes sticky SU independently
                // of the final accumulation's cause bits.
                const std::string fcsr = newtemp();
                const std::string old_flags = newtemp();
                const std::string cleared = newtemp();
                const std::string o_flags = newtemp();
                const std::string u_flags = newtemp();
                const std::string product_flags = newtemp();
                const std::string after_o = newtemp();
                const std::string after_u = newtemp();
                const std::string final_flags = newtemp();
                m_out += "  " + fcsr + " = " + fcsr_ptr() + "\n";
                m_out += "  " + old_flags + " = load i32, ptr " + fcsr + "\n";
                m_out += "  " + cleared + " = and i32 " + old_flags + ", -49153\n";
                m_out += "  " + o_flags + " = select i1 " + overflow + ", i32 32784, i32 0\n";
                m_out += "  " + u_flags + " = select i1 " + underflow + ", i32 16392, i32 0\n";
                m_out += "  " + product_flags + " = select i1 " + product_underflow + ", i32 8, i32 0\n";
                m_out += "  " + after_o + " = or i32 " + cleared + ", " + o_flags + "\n";
                m_out += "  " + after_u + " = or i32 " + after_o + ", " + u_flags + "\n";
                m_out += "  " + final_flags + " = or i32 " + after_u + ", " + product_flags + "\n";
                m_out += "  store i32 " + final_flags + ", ptr " + fcsr + "\n";
                if (fp_funct >= 0x1e)
                {
                    store_acc_i32(bits);
                    store_acc_overflow(overflow);
                }
                else
                {
                    store_fpr_i32(fd, bits);
                }
            }
            else if (fmt == 0x10 && (fp_funct == 0x28 || fp_funct == 0x29)) // MAX/MIN
            {
                // The EE interpreter implements MAX.S/MIN.S with signed
                // integer ordering of the raw IEEE-754 words. In particular,
                // do not replace this with host fcmp/select: the bitwise rule
                // defines the result for signed zero, infinities and NaNs too.
                const std::string a = load_fpr_i32(fs);
                const std::string b = load_fpr_i32(ft);
                const std::string a_negative = newtemp();
                const std::string b_negative = newtemp();
                const std::string both_negative = newtemp();
                const std::string a_less = newtemp();
                const std::string a_not_less = newtemp();
                const std::string choose_a = newtemp();
                const std::string result = newtemp();

                m_out += "  " + a_negative + " = icmp slt i32 " + a + ", 0\n";
                m_out += "  " + b_negative + " = icmp slt i32 " + b + ", 0\n";
                m_out += "  " + both_negative + " = and i1 " + a_negative + ", " + b_negative + "\n";
                m_out += "  " + a_less + " = icmp slt i32 " + a + ", " + b + "\n";
                m_out += "  " + a_not_less + " = xor i1 " + a_less + ", true\n";

                // For two negative encodings, signed order is reversed:
                // MAX chooses the signed minimum and MIN the signed maximum.
                if (fp_funct == 0x28)
                    m_out += "  " + choose_a + " = select i1 " + both_negative + ", i1 " + a_less + ", i1 " + a_not_less + "\n";
                else
                    m_out += "  " + choose_a + " = select i1 " + both_negative + ", i1 " + a_not_less + ", i1 " + a_less + "\n";

                m_out += "  " + result + " = select i1 " + choose_a + ", i32 " + a + ", i32 " + b + "\n";
                store_fpr_i32(fd, result);

                // MAX.S/MIN.S clear only the overflow/underflow cause flags.
                // Sticky flags and every other FCSR bit remain untouched.
                const std::string fcsr = newtemp();
                const std::string fcsr_value = newtemp();
                const std::string fcsr_updated = newtemp();
                m_out += "  " + fcsr + " = " + fcsr_ptr() + "\n";
                m_out += "  " + fcsr_value + " = load i32, ptr " + fcsr + "\n";
                m_out += "  " + fcsr_updated + " = and i32 " + fcsr_value + ", -49153\n";
                m_out += "  store i32 " + fcsr_updated + ", ptr " + fcsr + "\n";
            }
            else if (fp_funct == 0x20 || (fmt == 0x14 && fp_funct == 0x20))
            {
                // CVT.S.W truncates toward zero, rather than rounding to
                // nearest (ps2autotests ee_fpu/convert.expected).
                // Convert back to i64, not i32: +INT_MAX can round to +2^31.
                // If the rounded magnitude overshot, decrement its raw
                // float word by one ULP. This is defined for every i32.
                const std::string wi = load_fpr_i32(fs);
                const std::string fr = newtemp();
                const std::string wide = newtemp();
                const std::string back = newtemp();
                const std::string negative = newtemp();
                const std::string below = newtemp();
                const std::string above = newtemp();
                const std::string overshot = newtemp();
                const std::string raw = newtemp();
                const std::string corrected = newtemp();
                const std::string result = newtemp();
                m_out += "  " + fr + " = sitofp i32 " + wi + " to float\n";
                m_out += "  " + wide + " = sext i32 " + wi + " to i64\n";
                m_out += "  " + back + " = fptosi float " + fr + " to i64\n";
                m_out += "  " + negative + " = icmp slt i32 " + wi + ", 0\n";
                m_out += "  " + below + " = icmp slt i64 " + back + ", " + wide + "\n";
                m_out += "  " + above + " = icmp sgt i64 " + back + ", " + wide + "\n";
                m_out += "  " + overshot + " = select i1 " + negative + ", i1 " +
                         below + ", i1 " + above + "\n";
                m_out += "  " + raw + " = bitcast float " + fr + " to i32\n";
                m_out += "  " + corrected + " = sub i32 " + raw + ", 1\n";
                m_out += "  " + result + " = select i1 " + overshot + ", i32 " +
                         corrected + ", i32 " + raw + "\n";
                store_fpr_i32(fd, result);
            }
            else if (fmt == 0x10 && fp_funct == 0x24) // CVT.W.S
            {
                // The EE interpreter converts toward zero and saturates rather
                // than exposing LLVM's undefined out-of-range fptosi behaviour.
                // PCSX2's reference path accepts exponent fields through
                // 0x4e (absolute values < 2^31), then clamps by the sign bit.
                // NaN/Inf (exponent 0xff) therefore clamp to signed extrema.
                const std::string raw = load_fpr_i32(fs);
                const std::string exponent = newtemp();
                const std::string in_range = newtemp();
                const std::string safe_bits = newtemp();
                const std::string safe_float = newtemp();
                const std::string converted = newtemp();
                const std::string sign = newtemp();
                const std::string is_negative = newtemp();
                const std::string saturated = newtemp();
                const std::string result = newtemp();

                m_out += "  " + exponent + " = and i32 " + raw + ", " +
                         std::to_string(0x7f800000u) + "\n";
                m_out += "  " + in_range + " = icmp ule i32 " + exponent + ", " +
                         std::to_string(0x4e800000u) + "\n";
                // Make fptosi's input defined even when the original value is
                // outside the signed 32-bit conversion range or is NaN/Inf.
                m_out += "  " + safe_bits + " = select i1 " + in_range +
                         ", i32 " + raw + ", i32 0\n";
                m_out += "  " + safe_float + " = bitcast i32 " + safe_bits + " to float\n";
                m_out += "  " + converted + " = fptosi float " + safe_float + " to i32\n";
                m_out += "  " + sign + " = and i32 " + raw + ", -2147483648\n";
                m_out += "  " + is_negative + " = icmp ne i32 " + sign + ", 0\n";
                m_out += "  " + saturated + " = select i1 " + is_negative +
                         ", i32 -2147483648, i32 2147483647\n";
                m_out += "  " + result + " = select i1 " + in_range + ", i32 " +
                         converted + ", i32 " + saturated + "\n";
                store_fpr_i32(fd, result);
            }
            else if (fmt == 0x10 && (fp_funct == 0x30 || fp_funct == 0x32 ||
                                     fp_funct == 0x34 || fp_funct == 0x36))
            {
                // ps2autotests ee_fpu/compare.expected: exponent-255
                // operands retain their mantissas; denormals and both
                // signed zeros compare equal. Build a monotonic unsigned
                // key from the raw sign/magnitude representation, avoiding
                // host IEEE NaN handling entirely.
                auto load_compare_key = [&](uint8_t freg) -> std::string
                {
                    const std::string raw = load_fpr_i32(freg);
                    const std::string exponent = newtemp();
                    const std::string zero = newtemp();
                    const std::string bits = newtemp();
                    const std::string negative = newtemp();
                    const std::string negative_key = newtemp();
                    const std::string positive_key = newtemp();
                    const std::string key = newtemp();

                    m_out += "  " + exponent + " = and i32 " + raw + ", 2139095040\n";
                    m_out += "  " + zero + " = icmp eq i32 " + exponent + ", 0\n";
                    m_out += "  " + bits + " = select i1 " + zero +
                             ", i32 0, i32 " + raw + "\n";
                    m_out += "  " + negative + " = icmp slt i32 " + bits + ", 0\n";
                    m_out += "  " + negative_key + " = xor i32 " + bits + ", -1\n";
                    m_out += "  " + positive_key + " = xor i32 " + bits + ", -2147483648\n";
                    m_out += "  " + key + " = select i1 " + negative +
                             ", i32 " + negative_key + ", i32 " + positive_key + "\n";
                    return key;
                };

                // C.F always clears FCC0 and does not inspect either FPR.
                if (fp_funct == 0x30)
                {
                    const std::string false_value = newtemp();
                    m_out += "  " + false_value + " = add i1 0, 0\n";
                    set_fcc(false_value);
                }
                else
                {
                    const std::string fa = load_compare_key(fs);
                    const std::string fb = load_compare_key(ft);
                    const std::string cmp = newtemp();
                    const char* pred = "eq";
                    if (fp_funct == 0x34) pred = "ult";
                    else if (fp_funct == 0x36) pred = "ule";
                    m_out += "  " + cmp + " = icmp " + std::string(pred) + " i32 " + fa +
                             ", " + fb + "\n";
                    set_fcc(cmp);
                }
            }
            else if (fmt == 0x10 && (fp_funct == 0x0d || fp_funct == 0x0c ||
                                     fp_funct == 0x0e || fp_funct == 0x0f))
            {
                // TRUNC/ROUND/CEIL/FLOOR.W.S — approximate with fptosi
                std::string fa = load_fpr_f32(fs);
                std::string wi = newtemp();
                m_out += "  " + wi + " = fptosi float " + fa + " to i32\n";
                store_fpr_i32(fd, wi);
            }
            else
            {
                m_out += "  ; cop1 fmt=" + u32hex(fmt) + " funct=0x" + u32hex(fp_funct) +
                         " not yet lowered\n";
            }
        }

        else
        {
            m_out += "  ; cop1 fmt=0x" + u32hex(fmt) + " not yet lowered\n";
        }
        break;
    }
    case ee::e_ee_opcode::OP_COP2:
    {
        const char* helper = nullptr;
        bool reads_vu0 = false;
        switch (ins.rs)
        {
        case static_cast<uint8_t>(e_ee_opcode::COP2_QMFC2):
            helper = "ee_vu0_qmfc2";
            reads_vu0 = true;
            break;
        case static_cast<uint8_t>(e_ee_opcode::COP2_CFC2):
            helper = "ee_vu0_cfc2";
            reads_vu0 = true;
            break;
        case static_cast<uint8_t>(e_ee_opcode::COP2_QMTC2):
            helper = "ee_vu0_qmtc2";
            break;
        case static_cast<uint8_t>(e_ee_opcode::COP2_CTC2):
            helper = "ee_vu0_ctc2";
            break;
        default:
            break;
        }

        const bool vu0_macro = ins.rs >= 0x10u;
        if (!helper && !vu0_macro)
        {
            // BC2 is emitted separately as a control-flow predicate.
            m_out += "  ; COP2 operation has no runtime lowering\n";
            break;
        }

        // The helpers access architectural state in memory. Flush the
        // block-local GPR cache before crossing that boundary, then invalidate
        // it so later guest instructions reload any GPR changed by QMFC2/CFC2.
        std::set<uint8_t> dirty;
        for (uint8_t reg : m_dirty_regs)
            dirty.insert(reg);
        store_dirty_regs(dirty);
        reset_tracking();

        // VU User's Manual v6.0, chapter 6: transfer bit 0 selects
        // the interlock with an active micro-subroutine. Non-interlocked
        // transfers may catch up to the current EE cycle but do not wait
        // for VCALLMS completion; .I reads wait for E, writes for E/M.
        // This choice is encoded in the guest instruction, not inferred
        // from whether the transfer occupies a branch delay slot.
        const bool interlock = (ins.opcode_word & 1u) != 0u;
        const uint32_t sync_mode = interlock
            ? (reads_vu0 ? 1u : 2u) : 0u;
        const std::string result = newtemp();
        if (vu0_macro)
        {
            const uint32_t special2 =
                (ins.opcode_word & 3u) | ((ins.opcode_word >> 4u) & 0x7cu);
            const bool indexed_memory =
                (ins.opcode_word & 0x3cu) == 0x3cu &&
                special2 >= 0x34u && special2 <= 0x37u;
            const bool scalar_q =
                (ins.opcode_word & 0x3cu) == 0x3cu &&
                (special2 >= 0x39u && special2 <= 0x3bu);
            m_out += "  " + result + " = call i32 @" +
                     std::string(indexed_memory
                         ? "ee_vu0_macro_indexed_memory"
                         : scalar_q ? "ee_vu0_macro_scalar"
                                    : "ee_vu0_macro_fmac") +
                     "(ptr %cpu, ptr %rt, i32 " +
                     std::to_string(ins.opcode_word) + ", i32 " +
                     std::to_string(ins.pc) + ")\n";
        }
        else
        {
            m_out += "  " + result + " = call i32 @" + helper +
                     "(ptr %cpu, ptr %rt, i32 " + std::to_string(ins.rt) +
                     ", i32 " + std::to_string(ins.rd) +
                     ", i32 " + std::to_string(sync_mode) +
                     ", i32 " + std::to_string(ins.pc) + ")\n";
        }

        // Do not execute following guest instructions when the scheduler or
        // control callback is unavailable/fails. Keep the guest PC at the
        // transfer and surface the existing unsupported exit reason until a
        // dedicated VU0 runtime-failure exit is introduced.
        const std::string succeeded = newtemp();
        m_out += "  " + succeeded + " = icmp eq i32 " + result + ", 0\n";
        const std::string suffix = std::to_string(m_temp_counter++);
        const std::string fail_label = "vu0_fail_" + suffix;
        const std::string success_label = "vu0_success_" + suffix;
        m_out += "  br i1 " + succeeded + ", label %" + success_label +
                 ", label %" + fail_label + "\n";
        m_out += fail_label + ":\n";
        const std::string pc_ptr = state_field(6);
        const std::string exit_ptr = state_field(7);
        m_out += "  store i32 " + std::to_string(ins.pc) + ", ptr " + pc_ptr + "\n";
        m_out += "  store i32 4, ptr " + exit_ptr + "\n";
        m_out += "  ret i32 4\n";
        m_out += success_label + ":\n";
        break;
    }
    default:
        break;
    }
}

void module_builder::emit_block(const std::vector<ee::s_ee_instruction>& block,
                                bool /*is_last*/, uint32_t /*entry_pc*/,
                                const std::set<uint8_t>& live_in,
                                const std::set<uint8_t>& live_out,
                                uint32_t fallthrough_pc,
                                bool is_terminator,
                                bool guarded_loop_header)
{
    using ee::e_ee_opcode;
    if (block.empty()) return;
    m_out += block_label(block[0].pc);
    m_out += ":\n";

    if (guarded_loop_header)
    {
        // This guard executes before the first instruction in an
        // intra-function loop header. It is *not* an EE exception: the
        // host receives the exact resume PC and a distinct reason.
        // Null runtime and a zero limit keep older JIT clients working.
        // Incoming predecessors must flush their architectural GPR
        // changes; emit_function_cfg seeds all GPRs live at this header.
        const uint32_t pc = block[0].pc;
        const std::string suffix = std::to_string(pc);
        const std::string enabled_label = "loop_enabled_" + suffix;
        const std::string tick_label = "loop_tick_" + suffix;
        const std::string decrement_label = "loop_decrement_" + suffix;
        const std::string exhausted_label = "loop_exhausted_" + suffix;
        const std::string continue_label = "loop_continue_" + suffix;
        const std::string is_null = newtemp();
        m_out += "  " + is_null + " = icmp eq ptr %rt, null\n";
        m_out += "  br i1 " + is_null + ", label %" + continue_label +
                 ", label %" + enabled_label + "\n";
        m_out += enabled_label + ":\n";
        const std::string limit_ptr = newtemp();
        const std::string limit = newtemp();
        const std::string disabled = newtemp();
        m_out += "  " + limit_ptr + " = getelementptr i8, ptr %rt, i64 " +
                 std::to_string(offsetof(ee::s_ee_runtime, m_loop_safepoint_limit)) +
                 "\n";
        m_out += "  " + limit + " = load i32, ptr " + limit_ptr + "\n";
        m_out += "  " + disabled + " = icmp eq i32 " + limit + ", 0\n";
        m_out += "  br i1 " + disabled + ", label %" + continue_label +
                 ", label %" + tick_label + "\n";
        m_out += tick_label + ":\n";
        const std::string remaining_ptr = newtemp();
        const std::string remaining = newtemp();
        const std::string empty = newtemp();
        m_out += "  " + remaining_ptr + " = getelementptr i8, ptr %rt, i64 " +
                 std::to_string(offsetof(ee::s_ee_runtime, m_loop_safepoint_remaining)) +
                 "\n";
        m_out += "  " + remaining + " = load i32, ptr " + remaining_ptr + "\n";
        m_out += "  " + empty + " = icmp eq i32 " + remaining + ", 0\n";
        const std::string probe_check_label = "loop_probe_check_" + suffix;
        m_out += "  br i1 " + empty + ", label %" + exhausted_label +
                 ", label %" + probe_check_label + "\n";

        // Host idle-loop probe: an armed header records the budget at its
        // first visit and yields at its next visit, after exactly one real
        // iteration. Unarmed (all-zero) probes fall through unchanged.
        const std::string probe_label = "loop_probe_" + suffix;
        const std::string probe_first_label = "loop_probe_first_" + suffix;
        const std::string probe_again_label = "loop_probe_again_" + suffix;
        const std::string probe_yield_label = "loop_probe_yield_" + suffix;
        m_out += probe_check_label + ":\n";
        std::string armed;
        for (uint32_t slot = 0u; slot < 4u; ++slot)
        {
            const std::string slot_ptr = newtemp();
            const std::string slot_pc = newtemp();
            const std::string slot_hit = newtemp();
            m_out += "  " + slot_ptr + " = getelementptr i8, ptr %rt, i64 " +
                     std::to_string(offsetof(ee::s_ee_runtime, m_idle_probe_pcs) +
                                    4u * slot) + "\n";
            m_out += "  " + slot_pc + " = load i32, ptr " + slot_ptr + "\n";
            m_out += "  " + slot_hit + " = icmp eq i32 " + slot_pc + ", " +
                     std::to_string(pc) + "\n";
            if (armed.empty())
            {
                armed = slot_hit;
            }
            else
            {
                const std::string combined = newtemp();
                m_out += "  " + combined + " = or i1 " + armed + ", " +
                         slot_hit + "\n";
                armed = combined;
            }
        }
        m_out += "  br i1 " + armed + ", label %" + probe_label +
                 ", label %" + decrement_label + "\n";
        m_out += probe_label + ":\n";
        const std::string state_ptr = newtemp();
        const std::string state = newtemp();
        const std::string first_visit = newtemp();
        m_out += "  " + state_ptr + " = getelementptr i8, ptr %rt, i64 " +
                 std::to_string(offsetof(ee::s_ee_runtime, m_idle_probe_state)) +
                 "\n";
        m_out += "  " + state + " = load i32, ptr " + state_ptr + "\n";
        m_out += "  " + first_visit + " = icmp eq i32 " + state + ", 0\n";
        m_out += "  br i1 " + first_visit + ", label %" + probe_first_label +
                 ", label %" + probe_again_label + "\n";
        m_out += probe_first_label + ":\n";
        const std::string hit_ptr = newtemp();
        const std::string budget_ptr = newtemp();
        m_out += "  store i32 1, ptr " + state_ptr + "\n";
        m_out += "  " + hit_ptr + " = getelementptr i8, ptr %rt, i64 " +
                 std::to_string(offsetof(ee::s_ee_runtime, m_idle_probe_hit_pc)) +
                 "\n";
        m_out += "  store i32 " + std::to_string(pc) + ", ptr " + hit_ptr + "\n";
        m_out += "  " + budget_ptr + " = getelementptr i8, ptr %rt, i64 " +
                 std::to_string(offsetof(ee::s_ee_runtime, m_idle_probe_remaining)) +
                 "\n";
        m_out += "  store i32 " + remaining + ", ptr " + budget_ptr + "\n";
        m_out += "  br label %" + decrement_label + "\n";
        m_out += probe_again_label + ":\n";
        const std::string hit_again_ptr = newtemp();
        const std::string hit_pc = newtemp();
        const std::string one_state = newtemp();
        const std::string same_pc = newtemp();
        const std::string yield_now = newtemp();
        m_out += "  " + hit_again_ptr + " = getelementptr i8, ptr %rt, i64 " +
                 std::to_string(offsetof(ee::s_ee_runtime, m_idle_probe_hit_pc)) +
                 "\n";
        m_out += "  " + hit_pc + " = load i32, ptr " + hit_again_ptr + "\n";
        m_out += "  " + one_state + " = icmp eq i32 " + state + ", 1\n";
        m_out += "  " + same_pc + " = icmp eq i32 " + hit_pc + ", " +
                 std::to_string(pc) + "\n";
        m_out += "  " + yield_now + " = and i1 " + one_state + ", " + same_pc + "\n";
        m_out += "  br i1 " + yield_now + ", label %" + probe_yield_label +
                 ", label %" + decrement_label + "\n";
        m_out += probe_yield_label + ":\n";
        const std::string probe_pc_ptr = state_field(6);
        const std::string probe_reason_ptr = state_field(7);
        m_out += "  store i32 2, ptr " + state_ptr + "\n";
        m_out += "  store i32 " + std::to_string(pc) + ", ptr " + probe_pc_ptr + "\n";
        m_out += "  store i32 6, ptr " + probe_reason_ptr + "\n";
        m_out += "  ret i32 6\n";

        m_out += decrement_label + ":\n";
        const std::string decremented = newtemp();
        m_out += "  " + decremented + " = sub i32 " + remaining + ", 1\n";
        m_out += "  store i32 " + decremented + ", ptr " + remaining_ptr + "\n";
        m_out += "  br label %" + continue_label + "\n";
        m_out += exhausted_label + ":\n";
        const std::string pc_ptr = state_field(6);
        const std::string reason_ptr = state_field(7);
        m_out += "  store i32 " + std::to_string(pc) + ", ptr " + pc_ptr + "\n";
        // Two distinct host-only outcomes use the *same* exact resume PC
        // after all predecessor delay-slot writes have committed. Default
        // mode preserves the prior terminal LOOP_BUDGET diagnostic.
        const std::string yield_flag_ptr = newtemp();
        const std::string yield_flag = newtemp();
        const std::string yield_enabled = newtemp();
        const std::string safepoint_exit = newtemp();
        m_out += "  " + yield_flag_ptr + " = getelementptr i8, ptr %rt, i64 " +
                 std::to_string(offsetof(ee::s_ee_runtime,
                                         m_loop_safepoint_yield_enabled)) + "\n";
        m_out += "  " + yield_flag + " = load i32, ptr " + yield_flag_ptr + "\n";
        m_out += "  " + yield_enabled + " = icmp ne i32 " + yield_flag + ", 0\n";
        m_out += "  " + safepoint_exit + " = select i1 " + yield_enabled +
                 ", i32 6, i32 5\n";
        m_out += "  store i32 " + safepoint_exit + ", ptr " + reason_ptr + "\n";
        m_out += "  ret i32 " + safepoint_exit + "\n";
        m_out += continue_label + ":\n";
    }

    reset_tracking();

    for (uint8_t r : live_in)
        (void)load_gpr_temp(r);

    // Analyzer layout for control-flow blocks:
    //   [ ... straight-line ..., control-flow insn, delay-slot insn ]
    size_t n = block.size();
    int ctrl_idx = -1;
    int delay_idx = -1;
    if (n >= 2 && block[n - 2].has_delay_slot) {
        ctrl_idx = static_cast<int>(n - 2);
        delay_idx = static_cast<int>(n - 1);
    } else if (n >= 1 && (block[n - 1].has_delay_slot ||
                          block[n - 1].flow != ee::e_ee_flow::FLOW_NONE)) {
        // ERET and exception instructions terminate without a delay slot.
        ctrl_idx = static_cast<int>(n - 1);
    }

    for (size_t i = 0; i < n; ++i) {
        if (static_cast<int>(i) == ctrl_idx || static_cast<int>(i) == delay_idx)
            continue;
        emit_insn(block[i], false, 0);
    }

    auto make_cond = [&](const ee::s_ee_instruction& ctrl) -> std::string {
        using ee::e_ee_opcode;
        std::string cmp = newtemp();
        if (ctrl.opcode == e_ee_opcode::OP_BEQ ||
            ctrl.opcode == e_ee_opcode::OP_BEQL) {
            m_out += "  " + cmp + " = icmp eq i64 " + src_lo(ctrl.rs) + ", "
                   + src_lo(ctrl.rt) + "\n";
        } else if (ctrl.opcode == e_ee_opcode::OP_BNE ||
                   ctrl.opcode == e_ee_opcode::OP_BNEL) {
            m_out += "  " + cmp + " = icmp ne i64 " + src_lo(ctrl.rs) + ", "
                   + src_lo(ctrl.rt) + "\n";
        } else if (ctrl.opcode == e_ee_opcode::OP_BLEZ ||
                   ctrl.opcode == e_ee_opcode::OP_BLEZL) {
            m_out += "  " + cmp + " = icmp sle i64 " + src_lo(ctrl.rs) + ", 0\n";
        } else if (ctrl.opcode == e_ee_opcode::OP_BGTZ ||
                   ctrl.opcode == e_ee_opcode::OP_BGTZL) {
            m_out += "  " + cmp + " = icmp sgt i64 " + src_lo(ctrl.rs) + ", 0\n";
        } else if (ctrl.opcode == e_ee_opcode::OP_REGIMM) {
            auto rt = static_cast<e_ee_opcode>(ctrl.rt);
            const bool is_gez =
                (rt == e_ee_opcode::REGIMM_BGEZ ||
                 rt == e_ee_opcode::REGIMM_BGEZL ||
                 rt == e_ee_opcode::REGIMM_BGEZAL ||
                 rt == e_ee_opcode::REGIMM_BGEZALL);
            if (is_gez)
                m_out += "  " + cmp + " = icmp sge i64 " + src_lo(ctrl.rs) + ", 0\n";
            else
                m_out += "  " + cmp + " = icmp slt i64 " + src_lo(ctrl.rs) + ", 0\n";
        } else if (ctrl.opcode == e_ee_opcode::OP_COP1 && ctrl.rs == 0x08) {
            // BC1F/T and BC1FL/TL read FCC0 from FCSR. Evaluate the condition
            // before the delay slot; the slot is allowed to overwrite FCSR.
            std::string fcsr_ptr_temp = newtemp();
            std::string fcsr = newtemp();
            std::string fcc = newtemp();
            m_out += "  " + fcsr_ptr_temp + " = " + fcsr_ptr() + "\n";
            m_out += "  " + fcsr + " = load i32, ptr " + fcsr_ptr_temp + "\n";
            m_out += "  " + fcc + " = and i32 " + fcsr + ", 8388608\n";
            m_out += "  " + cmp + " = icmp " + std::string((ctrl.rt & 1) ? "ne" : "eq") +
                     " i32 " + fcc + ", 0\n";
        } else if (ctrl.opcode == e_ee_opcode::OP_COP2 &&
                   ctrl.rs == static_cast<uint8_t>(e_ee_opcode::COP2_BC2)) {
            // PCSX2's BC2 predicate is VPU_STAT bit 8. Read it before the
            // delay slot; missing VU0 state is an execution failure, not a
            // guessed branch outcome.
            const std::string condition = newtemp();
            const std::string unavailable = newtemp();
            const std::string suffix = std::to_string(m_temp_counter++);
            const std::string fail_label = "bc2_fail_" + suffix;
            const std::string ready_label = "bc2_ready_" + suffix;
            m_out += "  " + condition +
                     " = call i32 @ee_vu0_get_branch_condition(ptr %rt)\n";
            m_out += "  " + unavailable + " = icmp eq i32 " + condition + ", 2\n";
            m_out += "  br i1 " + unavailable + ", label %" + fail_label +
                     ", label %" + ready_label + "\n";
            m_out += fail_label + ":\n";
            const std::string pc_ptr = state_field(6);
            const std::string exit_ptr = state_field(7);
            m_out += "  store i32 " + std::to_string(ctrl.pc) + ", ptr " + pc_ptr + "\n";
            m_out += "  store i32 4, ptr " + exit_ptr + "\n";
            m_out += "  ret i32 4\n";
            m_out += ready_label + ":\n";
            m_out += "  " + cmp + " = icmp " +
                     std::string((ctrl.rt & 1) ? "ne" : "eq") +
                     " i32 " + condition + ", 0\n";
        } else {
            // Unconditional or unsupported — treat as always-taken.
            m_out += "  " + cmp + " = add i1 1, 0\n";
        }
        return cmp;
    };

    if (ctrl_idx >= 0) {
        const auto& ctrl = block[ctrl_idx];
        uint32_t ft = fallthrough_pc ? fallthrough_pc : (ctrl.pc + 8);
        uint32_t target = ctrl.branch_target ? ctrl.branch_target : ctrl.jump_target;

        // Do not let an unsupported branch reach make_cond()'s fallback
        // predicate (which is intentionally always true for unconditional
        // control transfers). In particular, BC2FL/BC2TL have an annulled
        // delay slot whose execution depends on a VU0 condition we do not
        // model. Exit at the branch itself instead of guessing an edge.
        if (ctrl.valid != ee::e_ee_valid::VALID) {
            // Unsupported ordinary transfers still execute their architectural
            // delay slot before exiting to the dispatcher. Branch-likely
            // transfers are different: their slot is annulled when not taken,
            // and without a supported predicate we cannot safely decide whether
            // to execute it. In that case, fail closed at the branch itself.
            if (!ctrl.branch_likely && delay_idx >= 0) {
                emit_insn(block[delay_idx], false, 0, true);
                store_dirty_regs(live_out);
            }
            emit_insn(ctrl, true, ft);
            // emit_insn() leaves a uniquely named continuation block for
            // callers that may still emit later instructions. A control
            // transfer is the end of this block, so terminate that unreachable
            // continuation explicitly before the next CFG label is emitted.
            m_out += "  unreachable\n";
        } else if (ctrl.branch_likely && delay_idx >= 0) {
            // ---------------------------------------------------------------
            // Branch-likely: delay slot executes ONLY when the branch is taken.
            // Split into taken (delay + target) and not-taken (skip delay, ft).
            // ---------------------------------------------------------------
            // The branch condition reads the PRE-delay-slot register values.
            m_out += "  call void @ee_count_guest_instruction(ptr %rt, i32 " +
                         std::to_string(ctrl.pc) + ", i32 " +
                         std::to_string(ctrl.opcode_word) + ")\n";
            std::string cmp = make_cond(ctrl);

            // BLTZALL/BGEZALL write the link unconditionally, including when
            // the branch is not taken and the delay slot is annulled.
            const auto likely_rt = static_cast<e_ee_opcode>(ctrl.rt);
            const bool likely_has_link =
                ctrl.opcode == e_ee_opcode::OP_REGIMM &&
                (likely_rt == e_ee_opcode::REGIMM_BLTZALL ||
                 likely_rt == e_ee_opcode::REGIMM_BGEZALL);
            if (likely_has_link)
            {
                std::string link = newtemp();
                char link_pc[32];
                std::snprintf(link_pc, sizeof(link_pc), "%u", (unsigned)(ctrl.pc + 8u));
                m_out += "  " + link + " = add i64 " + link_pc + ", 0\n";
                set_gpr_lo64(31, link);
            }

            // Flush all pre-delay architectural state in the common block, so
            // both successors start from it. (Previously the not-taken path
            // "invalidated" the delay slot's destinations, which also dropped
            // earlier writes to those registers made in this same block.)
            store_dirty_regs(live_out);

            // Labels must be unique per emitted block: the analyzer can emit
            // overlapping blocks that contain the same branch-likely pc.
            char taken_buf[64], nottaken_buf[64];
            std::snprintf(taken_buf, sizeof(taken_buf), "bl_taken_0x%x_0x%x", block[0].pc, ctrl.pc);
            std::snprintf(nottaken_buf, sizeof(nottaken_buf), "bl_nottaken_0x%x_0x%x", block[0].pc, ctrl.pc);

            m_out += "  br i1 " + cmp + ", label %" + taken_buf
                   + ", label %" + nottaken_buf + "\n";

            // --- taken path: execute delay slot, then go to the branch target ---
            m_out += taken_buf;
            m_out += ":\n";
            emit_insn(block[delay_idx], false, 0, true);
            store_dirty_regs(live_out);
            m_out += "  br label %" + block_label(target ? target : 0) + "\n";

            // --- not-taken path: delay slot annulled, state already flushed ---
            m_out += nottaken_buf;
            m_out += ":\n";
            m_out += "  call void @ee_count_reference_annulled_slot(ptr %rt, i32 " +
                         std::to_string(block[delay_idx].pc) + ", i32 " +
                         std::to_string(block[delay_idx].opcode_word) + ")\n";
            m_out += "  br label %" + block_label(ft) + "\n";
        } else if (delay_idx >= 0) {
            // Ordinary conditional branches sample their condition before the
            // delay slot, although the slot itself always executes. Keep the
            // predicate in SSA, execute the slot, then select the successor.
            const auto regimm_rt = static_cast<e_ee_opcode>(ctrl.rt);
            const bool has_link =
                ctrl.opcode == e_ee_opcode::OP_REGIMM &&
                (regimm_rt == e_ee_opcode::REGIMM_BLTZAL ||
                 regimm_rt == e_ee_opcode::REGIMM_BGEZAL);
            const bool supported_conditional =
                ctrl.flow == ee::e_ee_flow::FLOW_BRANCH &&
                (ctrl.opcode == e_ee_opcode::OP_BEQ ||
                 ctrl.opcode == e_ee_opcode::OP_BNE ||
                 ctrl.opcode == e_ee_opcode::OP_BLEZ ||
                 ctrl.opcode == e_ee_opcode::OP_BGTZ ||
                 ctrl.opcode == e_ee_opcode::OP_REGIMM ||
                 (ctrl.opcode == e_ee_opcode::OP_COP1 && ctrl.rs == 0x08) ||
                 (ctrl.opcode == e_ee_opcode::OP_COP2 &&
                  ctrl.rs == static_cast<uint8_t>(e_ee_opcode::COP2_BC2)));
            if (supported_conditional)
            {
                m_out += "  call void @ee_count_guest_instruction(ptr %rt, i32 " +
                         std::to_string(ctrl.pc) + ", i32 " +
                         std::to_string(ctrl.opcode_word) + ")\n";
                const std::string cmp = make_cond(ctrl);

                // BLTZAL/BGEZAL always set $ra to PC+8 before the ordinary
                // delay slot executes, independent of the branch outcome.
                if (has_link)
                {
                    std::string link = newtemp();
                    char link_pc[32];
                    std::snprintf(link_pc, sizeof(link_pc), "%u", (unsigned)(ctrl.pc + 8u));
                    m_out += "  " + link + " = add i64 " + link_pc + ", 0\n";
                    set_gpr_lo64(31, link);
                }

                emit_insn(block[delay_idx], false, 0, true);
                store_dirty_regs(live_out);
                m_out += "  br i1 " + cmp + ", label %" + block_label(target) +
                         ", label %" + block_label(ft) + "\n";
            }
            else if (ctrl.opcode == e_ee_opcode::OP_JAL)
            {
                m_out += "  call void @ee_count_guest_instruction(ptr %rt, i32 " +
                         std::to_string(ctrl.pc) + ", i32 " +
                         std::to_string(ctrl.opcode_word) + ")\n";
                // JAL writes $ra before the delay slot executes.
                std::string link = newtemp();
                char link_pc[32];
                std::snprintf(link_pc, sizeof(link_pc), "%u", (unsigned)(ctrl.pc + 8u));
                m_out += "  " + link + " = add i64 " + link_pc + ", 0\n";
                set_gpr_lo64(31, link);

                emit_insn(block[delay_idx], false, 0, true);
                store_dirty_regs(live_out);

                // A callee is emitted as a separate translated function. Return
                // to the host dispatcher with its exact guest entry address.
                const std::string pc_ptr = state_field(6);
                const std::string exit_ptr = state_field(7);
                m_out += "  store i32 " + std::to_string(ctrl.jump_target) + ", ptr " + pc_ptr + "\n";
                m_out += "  store i32 3, ptr " + exit_ptr + "\n";
                m_out += "  ret i32 3\n";
            }
            else if (ctrl.opcode == e_ee_opcode::OP_SPECIAL &&
                     (ctrl.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_JR) ||
                      ctrl.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_JALR)))
            {
                m_out += "  call void @ee_count_guest_instruction(ptr %rt, i32 " +
                         std::to_string(ctrl.pc) + ", i32 " +
                         std::to_string(ctrl.opcode_word) + ")\n";
                // Register-indirect targets are sampled before the delay slot:
                // the slot may overwrite rs, or rd may alias rs in JALR.
                const std::string jump_target = src_lo(ctrl.rs);
                const bool is_jalr =
                    ctrl.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_JALR);
                if (is_jalr)
                {
                    std::string link = newtemp();
                    char link_pc[32];
                    std::snprintf(link_pc, sizeof(link_pc), "%u", (unsigned)(ctrl.pc + 8u));
                    m_out += "  " + link + " = add i64 " + link_pc + ", 0\n";
                    set_gpr_lo64(ctrl.rd, link);
                }

                emit_insn(block[delay_idx], false, 0, true);
                store_dirty_regs(live_out);

                const std::string target32 = newtemp();
                m_out += "  " + target32 + " = trunc i64 " + jump_target + " to i32\n";
                const std::string pc_ptr = state_field(6);
                const std::string exit_ptr = state_field(7);
                const uint32_t exit_reason =
                    ctrl.flow == ee::e_ee_flow::FLOW_RETURN ? 0u : 3u;
                m_out += "  store i32 " + target32 + ", ptr " + pc_ptr + "\n";
                m_out += "  store i32 " + std::to_string(exit_reason) + ", ptr " + exit_ptr + "\n";
                m_out += "  ret i32 " + std::to_string(exit_reason) + "\n";
            }
            else
            {
                // Other transfers retain their existing emitter path.
                emit_insn(block[delay_idx], false, 0, true);
                store_dirty_regs(live_out);
                emit_insn(ctrl, true, ft);
            }
        } else {
            store_dirty_regs(live_out);
            emit_insn(ctrl, true, ft);
        }
    } else {
        // Straight-line instructions were emitted by the loop above. Do not
        // emit the last one a second time when closing the block.
        store_dirty_regs(live_out);
        if (is_terminator || fallthrough_pc == 0)
            m_out += "  br label %" + block_label(0) + "\n";
        else
            m_out += "  br label %" + block_label(fallthrough_pc) + "\n";
    }
}

void module_builder::begin_function(const char* func_name)
{
    m_out += "define i32 @";
    m_out += func_name;
    m_out += "(ptr %cpu, ptr %rt) {\n";
    m_out += "entry:\n";
}

void module_builder::end_function()
{
    // Callers ensure every path already has a terminator.
    m_out += "}\n";
}
void module_builder::emit_function(const std::vector<ee::s_ee_instruction>& insns,
                                   uint32_t entry_pc, const char* func_name)
{
    using ee::e_ee_opcode;
    header();
    declare_memory_helpers();

    std::vector<std::vector<ee::s_ee_instruction>> blocks;
    std::vector<ee::s_ee_instruction> cur;

    // A branch may target an instruction in the middle of the linear input
    // sequence. Split there so every intra-function branch has a real LLVM
    // basic-block label instead of branching into the middle of another block.
    std::set<uint32_t> branch_targets;
    for (const auto& ins : insns)
    {
        if (ins.branch_target != 0)
            branch_targets.insert(ins.branch_target);
        if (ins.jump_target != 0)
            branch_targets.insert(ins.jump_target);
    }

    for (size_t i = 0; i < insns.size(); ++i)
    {
        const auto& ins = insns[i];

        if (!cur.empty() && branch_targets.count(ins.pc) != 0)
        {
            blocks.push_back(std::move(cur));
            cur.clear();
        }

        cur.push_back(ins);   // unsupported ones are kept: emit_insn makes an explicit exit
        const bool ends_block = (ins.flow != ee::e_ee_flow::FLOW_NONE);
        const bool last = (i + 1 == insns.size());
        if (ends_block)
        {
            // A control transfer and its delay slot are one indivisible block.
            if (ins.has_delay_slot && i + 1 < insns.size())
            {
                cur.push_back(insns[i + 1]);
                ++i;
            }
            blocks.push_back(std::move(cur));
            cur.clear();
        }
        else if (last)
        {
            blocks.push_back(std::move(cur));
            cur.clear();
        }
    }

    // Conservative liveness: all touched regs are live-in at block 0,
    // all touched regs are live-out at every block.
    std::set<uint8_t> touched;
    for (const auto& ins : insns)
    {
        if (ins.valid != ee::e_ee_valid::VALID) continue;
        switch (ins.opcode)
        {
        case ee::e_ee_opcode::OP_ADDIU:
        case ee::e_ee_opcode::OP_DADDIU:
        case ee::e_ee_opcode::OP_ANDI:
        case ee::e_ee_opcode::OP_ORI:
        case ee::e_ee_opcode::OP_XORI:
        case ee::e_ee_opcode::OP_LUI:
        case ee::e_ee_opcode::OP_SLTI:
        case ee::e_ee_opcode::OP_SLTIU:
        case ee::e_ee_opcode::OP_LB:
        case ee::e_ee_opcode::OP_LH:
        case ee::e_ee_opcode::OP_LBU:
        case e_ee_opcode::OP_LHU:
        case e_ee_opcode::OP_LW:
        case e_ee_opcode::OP_LWU:
        case e_ee_opcode::OP_LD:
        case e_ee_opcode::OP_LQ:
        case e_ee_opcode::OP_LWL:
        case e_ee_opcode::OP_LWR:
        case e_ee_opcode::OP_LDL:
        case e_ee_opcode::OP_LDR:
            touched.insert(ins.rt);
            if (ins.rs != 0) touched.insert(ins.rs);
            break;
        case e_ee_opcode::OP_SW:
        case e_ee_opcode::OP_SB:
        case e_ee_opcode::OP_SH:
        case e_ee_opcode::OP_SD:
        case e_ee_opcode::OP_SQ:
        case e_ee_opcode::OP_SWL:
        case e_ee_opcode::OP_SDL:
        case e_ee_opcode::OP_SWR:
        case e_ee_opcode::OP_SDR:
            if (ins.rs != 0) touched.insert(ins.rs);
            touched.insert(ins.rt);
            break;
        case e_ee_opcode::OP_LWC1:
        case e_ee_opcode::OP_SWC1:
        case e_ee_opcode::OP_LQC2:
        case e_ee_opcode::OP_SQC2:
            if (ins.rs != 0)
            {
                touched.insert(ins.rs);
            }
            break;
        case e_ee_opcode::OP_SPECIAL:
            switch (static_cast<ee::e_ee_opcode>(ins.funct))
            {
            case e_ee_opcode::FUNCT_JR:
                if (ins.rs != 0) touched.insert(ins.rs);
                break;
            case e_ee_opcode::FUNCT_JALR:
                if (ins.rs != 0) touched.insert(ins.rs);
                if (ins.rd != 0) touched.insert(ins.rd);
                break;
            default:
                if (ins.rd != 0) touched.insert(ins.rd);
                if (ins.rs != 0) touched.insert(ins.rs);
                if (ins.rt != 0) touched.insert(ins.rt);
                break;
            }
            break;
        case e_ee_opcode::OP_REGIMM:
        case e_ee_opcode::OP_ADDI:
            if (ins.rs != 0) touched.insert(ins.rs);
            touched.insert(ins.rt);
            break;
        case e_ee_opcode::OP_COP2:
            if (ins.rt != 0) touched.insert(ins.rt);
            break;
        default:
            break;
        }
    }
    touched.insert(31);

    begin_function(func_name);

    if (!blocks.empty() && !blocks[0].empty())
        m_out += "  br label %" + block_label(blocks[0][0].pc) + "\n";
    else
        m_out += "  br label %" + block_label(0) + "\n";

    std::set<uint32_t> defined_pcs;
    std::set<uint32_t> referenced_pcs;
    for (size_t b = 0; b < blocks.size(); ++b)
    {
        if (blocks[b].empty())
            continue;
        defined_pcs.insert(blocks[b][0].pc);
        if (b + 1 < blocks.size() && !blocks[b + 1].empty())
            referenced_pcs.insert(blocks[b + 1][0].pc);
        for (const auto& ins : blocks[b])
        {
            if (ins.branch_target != 0)
                referenced_pcs.insert(ins.branch_target);
            if (ins.jump_target != 0)
                referenced_pcs.insert(ins.jump_target);
            if (ins.has_delay_slot)
                referenced_pcs.insert(ins.pc + 8);
        }
    }

    for (size_t b = 0; b < blocks.size(); ++b)
    {
        const uint32_t fallthrough_pc =
            (b + 1 < blocks.size() && !blocks[b + 1].empty())
                ? blocks[b + 1][0].pc : 0;
        emit_block(blocks[b], b == blocks.size() - 1, entry_pc,
                   touched, touched, fallthrough_pc, b == blocks.size() - 1);
    }

    // A direct target outside this emitted function is a dispatcher exit, not
    // an undefined LLVM label. Preserve the guest PC for the host dispatcher.
    for (uint32_t pc : referenced_pcs)
    {
        if (pc == 0 || defined_pcs.count(pc) != 0)
            continue;
        m_out += block_label(pc) + ":\n";
        std::string pc_ptr = state_field(6);
        m_out += "  store i32 " + std::to_string(pc) + ", ptr " + pc_ptr + "\n";
        std::string exit_ptr = state_field(7);
        m_out += "  store i32 3, ptr " + exit_ptr + "\n";
        m_out += "  ret i32 3\n";
    }

    m_out += block_label(0);
    m_out += ":\n";
    m_out += "  ret i32 0\n";
    m_out += "\n";
    end_function();

    if (m_out.find("call i32 @ee_cop1_rsqrt_bits(") != std::string::npos ||
        m_out.find("call i32 @ee_cop1_radix_quotient(") != std::string::npos ||
        m_out.find("call i32 @ee_cop1_radix_root(") != std::string::npos)
    {
        m_out += k_ee_cop1_rsqrt_ir;
    }
    if (m_out.find("@ee_cop1_mac_bits(") != std::string::npos)
    {
        m_out += k_ee_cop1_mac_ir;
    }
    if (m_out.find("@ee_cop1_add_bits(") != std::string::npos)
    {
        m_out += k_ee_cop1_add_ir;
    }
    if (m_out.find("@ee_cop1_mul_bits(") != std::string::npos)
    {
        m_out += k_ee_cop1_mul_ir;
    }
    m_ctx.ir(optimize_ir(m_out, m_opt_level));
  }

void module_builder::emit_gpr_loads(const std::vector<uint8_t>& regs)
{
    for (uint8_t r : regs)
    {
        std::string plo = gptr_lo(r);
        m_out += "  " + reg_lo(r) + " = load i64, ptr " + plo + "\n";
        std::string phi = gptr_hi(r);
        m_out += "  " + reg_hi(r) + " = load i64, ptr " + phi + "\n";
    }
}

void module_builder::emit_gpr_stores(const std::vector<uint8_t>& regs)
{
    for (uint8_t r : regs)
    {
        std::string plo = gptr_lo(r);
        m_out += "  store i64 " + reg_lo(r) + ", ptr " + plo + "\n";
        std::string phi = gptr_hi(r);
        m_out += "  store i64 " + reg_hi(r) + ", ptr " + phi + "\n";
    }
}

// ---- MMI instruction emission (vector types) ----

void module_builder::emit_mmi_insn(const ee::s_ee_instruction& ins)
{
    // R5900 MMI class in funct: 0x08=MMI0, 0x28=MMI1,
    // 0x09=MMI2, 0x29=MMI3. The sub-op is in sa.
    // Use the block-local register cache so earlier writes in this block are
    // visible to MTHI1/MTLO1 and packed operations that read the same GPR.
    auto load_gpr = [&](uint8_t r) -> std::pair<std::string,std::string> {
        return {src_lo(r), src_hi(r)};
    };
    auto store_gpr = [&](uint8_t r, const std::string& lo, const std::string& hi) {
        set_gpr_temp(r, lo, hi);
    };
    auto build2i64 = [&](const std::string& lo, const std::string& hi) {
        std::string v = newtemp();
        m_out += "  " + v + " = insertelement <2 x i64> undef, i64 " + lo + ", i32 0\n";
        std::string v2 = newtemp();
        m_out += "  " + v2 + " = insertelement <2 x i64> " + v + ", i64 " + hi + ", i32 1\n";
        return v2;
    };
    auto extract2i64 = [&](const std::string& v) {
        std::string lo = newtemp(), hi = newtemp();
        m_out += "  " + lo + " = extractelement <2 x i64> " + v + ", i32 0\n";
        m_out += "  " + hi + " = extractelement <2 x i64> " + v + ", i32 1\n";
        return std::make_pair(lo, hi);
    };
    auto build4i32 = [&](const std::string& lo, const std::string& hi) {
        // bitcast 2xi64 → 4xi32
        std::string v2 = build2i64(lo, hi);
        std::string v4 = newtemp();
        m_out += "  " + v4 + " = bitcast <2 x i64> " + v2 + " to <4 x i32>\n";
        return v4;
    };
    auto extract4i32 = [&](const std::string& v4) {
        std::string v2 = newtemp();
        m_out += "  " + v2 + " = bitcast <4 x i32> " + v4 + " to <2 x i64>\n";
        return extract2i64(v2);
    };
    auto build8i16 = [&](const std::string& lo, const std::string& hi) {
        std::string v2 = build2i64(lo, hi);
        std::string v8 = newtemp();
        m_out += "  " + v8 + " = bitcast <2 x i64> " + v2 + " to <8 x i16>\n";
        return v8;
    };
    auto extract8i16 = [&](const std::string& v8) {
        std::string v2 = newtemp();
        m_out += "  " + v2 + " = bitcast <8 x i16> " + v8 + " to <2 x i64>\n";
        return extract2i64(v2);
    };
    auto build16i8 = [&](const std::string& lo, const std::string& hi) {
        std::string v2 = build2i64(lo, hi);
        std::string v16 = newtemp();
        m_out += "  " + v16 + " = bitcast <2 x i64> " + v2 + " to <16 x i8>\n";
        return v16;
    };
    auto extract16i8 = [&](const std::string& v16) {
        std::string v2 = newtemp();
        m_out += "  " + v2 + " = bitcast <16 x i8> " + v16 + " to <2 x i64>\n";
        return extract2i64(v2);
    };

    auto a = load_gpr(ins.rs);
    auto b = load_gpr(ins.rt);

    auto store_vec2i64 = [&](const std::string& vr) {
        auto r = extract2i64(vr);
        store_gpr(ins.rd, r.first, r.second);
    };
    auto store_vec4i32 = [&](const std::string& vr) {
        auto r = extract4i32(vr);
        store_gpr(ins.rd, r.first, r.second);
    };
    auto store_vec8i16 = [&](const std::string& vr) {
        auto r = extract8i16(vr);
        store_gpr(ins.rd, r.first, r.second);
    };
    auto store_vec16i8 = [&](const std::string& vr) {
        auto r = extract16i8(vr);
        store_gpr(ins.rd, r.first, r.second);
    };

    const uint8_t fn = ins.funct;
    const uint8_t sa = ins.sa & 31;

    // Primary MMI packed shifts operate on all lanes of rt (not rs).
    // The hardware masks halfword shifts to four bits, word shifts to five.
    if (fn == 0x34 || fn == 0x36 || fn == 0x37 ||
        fn == 0x3c || fn == 0x3e || fn == 0x3f)
    {
        const bool halfword = fn == 0x34 || fn == 0x36 || fn == 0x37;
        const char* operation = (fn == 0x34 || fn == 0x3c) ? "shl" :
                                (fn == 0x36 || fn == 0x3e) ? "lshr" : "ashr";
        const std::string result = newtemp();
        const std::string input = halfword
            ? build8i16(b.first, b.second) : build4i32(b.first, b.second);
        const std::string type = halfword ? "<8 x i16>" : "<4 x i32>";
        const std::string scalar_type = halfword ? "i16" : "i32";
        const uint32_t lanes = halfword ? 8u : 4u;
        const uint32_t shift = sa & (halfword ? 15u : 31u);
        std::string amounts = "<";
        for (uint32_t lane = 0u; lane < lanes; ++lane)
        {
            if (lane != 0u)
            {
                amounts += ", ";
            }
            amounts += scalar_type + " " + std::to_string(shift);
        }
        amounts += ">";
        m_out += "  " + result + " = " + operation + " " + type +
                 " " + input + ", " + amounts + "\n";
        if (halfword)
        {
            store_vec8i16(result);
        }
        else
        {
            store_vec4i32(result);
        }
        return;
    }

    if (fn == 0x04) // PLZCW: count leading sign bits except the sign bit.
    {
        // The R5900 writes only rd words 0 and 1, preserving words 2/3.
        const std::string source = build4i32(a.first, a.second);
        std::string halves[2];
        for (uint32_t lane = 0u; lane < 2u; ++lane)
        {
            const std::string word = newtemp();
            const std::string sign = newtemp();
            const std::string normalized = newtemp();
            const std::string leading = newtemp();
            const std::string count = newtemp();
            const std::string extended = newtemp();
            m_out += "  " + word + " = extractelement <4 x i32> " +
                     source + ", i32 " + std::to_string(lane) + "\n";
            m_out += "  " + sign + " = ashr i32 " + word + ", 31\n";
            m_out += "  " + normalized + " = xor i32 " + word + ", " +
                     sign + "\n";
            m_out += "  " + leading + " = call i32 @llvm.ctlz.i32(i32 " +
                     normalized + ", i1 false)\n";
            m_out += "  " + count + " = sub i32 " + leading + ", 1\n";
            m_out += "  " + extended + " = zext i32 " + count + " to i64\n";
            halves[lane] = extended;
        }
        const std::string upper = newtemp();
        const std::string result = newtemp();
        m_out += "  " + upper + " = shl i64 " + halves[1] + ", 32\n";
        m_out += "  " + result + " = or i64 " + halves[0] + ", " + upper + "\n";
        set_gpr_lo64(ins.rd, result);
        return;
    }

    // PMFHL reads both 128-bit HI and LO register banks. The EE state
    // exposes each bank as two i64 fields: HI (1), LO (2), HI1 (3), LO1 (4).
    // The five submodes reproduce PCSX2's MMI interpreter word layout.
    if (fn == 0x30)
    {
        const std::string hi0 = newtemp();
        const std::string lo0 = newtemp();
        const std::string hi1 = newtemp();
        const std::string lo1 = newtemp();
        m_out += "  " + hi0 + " = load i64, ptr " + state_field(1) + "\n";
        m_out += "  " + lo0 + " = load i64, ptr " + state_field(2) + "\n";
        m_out += "  " + hi1 + " = load i64, ptr " + state_field(3) + "\n";
        m_out += "  " + lo1 + " = load i64, ptr " + state_field(4) + "\n";
        const std::string halves[2][2] = {{lo0, hi0}, {lo1, hi1}};

        if (sa == 0 || sa == 1) // PMFHL.LW / PMFHL.UW
        {
            std::string vector = "undef";
            for (uint32_t lane = 0; lane < 4; ++lane)
            {
                const std::string& source = halves[lane / 2][lane % 2];
                std::string word_source = source;
                if (sa == 1)
                {
                    const std::string shifted = newtemp();
                    m_out += "  " + shifted + " = lshr i64 " + source + ", 32\n";
                    word_source = shifted;
                }
                const std::string word = newtemp();
                const std::string updated = newtemp();
                m_out += "  " + word + " = trunc i64 " + word_source + " to i32\n";
                m_out += "  " + updated + " = insertelement <4 x i32> " +
                         vector + ", i32 " + word + ", i32 " +
                         std::to_string(lane) + "\n";
                vector = updated;
            }
            store_vec4i32(vector);
            return;
        }

        if (sa == 2) // PMFHL.SLW: signed 64-bit pair -> saturated 32-bit
        {
            std::string outputs[2];
            for (uint32_t index = 0; index < 2; ++index)
            {
                const std::string low_word = newtemp();
                const std::string high_word = newtemp();
                const std::string high_shifted = newtemp();
                const std::string pair = newtemp();
                const std::string too_high = newtemp();
                const std::string too_low = newtemp();
                const std::string upper_clamp = newtemp();
                const std::string lower_clamp = newtemp();

                m_out += "  " + low_word + " = and i64 " + halves[index][0] +
                         ", 4294967295\n";
                m_out += "  " + high_word + " = and i64 " + halves[index][1] +
                         ", 4294967295\n";
                m_out += "  " + high_shifted + " = shl i64 " + high_word +
                         ", 32\n";
                m_out += "  " + pair + " = or i64 " + high_shifted +
                         ", " + low_word + "\n";
                m_out += "  " + too_high + " = icmp sgt i64 " + pair +
                         ", 2147483647\n";
                m_out += "  " + too_low + " = icmp slt i64 " + pair +
                         ", -2147483648\n";
                m_out += "  " + upper_clamp + " = select i1 " + too_high +
                         ", i64 2147483647, i64 " + pair + "\n";
                m_out += "  " + lower_clamp + " = select i1 " + too_low +
                         ", i64 -2147483648, i64 " + upper_clamp + "\n";
                outputs[index] = lower_clamp;
            }
            store_gpr(ins.rd, outputs[0], outputs[1]);
            return;
        }

        if (sa == 3 || sa == 4) // PMFHL.LH / PMFHL.SH
        {
            std::string vector = "undef";
            for (uint32_t lane = 0; lane < 8; ++lane)
            {
                const uint32_t bank = lane / 4;
                const uint32_t word = lane % 4;
                const std::string& source = halves[bank][word / 2];
                std::string source_word = source;
                if ((word & 1u) != 0)
                {
                    const std::string shifted = newtemp();
                    m_out += "  " + shifted + " = lshr i64 " + source + ", 32\n";
                    source_word = shifted;
                }

                std::string value;
                if (sa == 3)
                {
                    value = newtemp();
                    m_out += "  " + value + " = trunc i64 " + source_word +
                             " to i16\n";
                }
                else
                {
                    // PMFHL.SH saturates each signed 32-bit source word
                    // independently, without touching any HI/LO field.
                    const std::string word32 = newtemp();
                    const std::string word64 = newtemp();
                    const std::string too_high = newtemp();
                    const std::string too_low = newtemp();
                    const std::string high_clamp = newtemp();
                    const std::string low_clamp = newtemp();
                    value = newtemp();
                    m_out += "  " + word32 + " = trunc i64 " +
                             source_word + " to i32\n";
                    m_out += "  " + word64 + " = sext i32 " + word32 +
                             " to i64\n";
                    m_out += "  " + too_high + " = icmp sgt i64 " +
                             word64 + ", 32767\n";
                    m_out += "  " + too_low + " = icmp slt i64 " +
                             word64 + ", -32768\n";
                    m_out += "  " + high_clamp + " = select i1 " +
                             too_high + ", i64 32767, i64 " + word64 + "\n";
                    m_out += "  " + low_clamp + " = select i1 " +
                             too_low + ", i64 -32768, i64 " + high_clamp + "\n";
                    m_out += "  " + value + " = trunc i64 " + low_clamp +
                             " to i16\n";
                }

                const std::string next = newtemp();
                m_out += "  " + next + " = insertelement <8 x i16> " +
                         vector + ", i16 " + value + ", i32 " +
                         std::to_string(lane) + "\n";
                vector = next;
            }
            store_vec8i16(vector);
            return;
        }
    }

    if (fn == 0x31) // PMTHL.LW: write low words, preserve upper 32 bits
    {
        const std::string sources[4] = {a.first, a.first, a.second, a.second};
        const int field_indices[4] = {2, 1, 4, 3}; // LO0, HI0, LO1, HI1
        for (uint32_t lane = 0; lane < 4; ++lane)
        {
            std::string source = sources[lane];
            if ((lane & 1u) != 0)
            {
                const std::string shifted = newtemp();
                m_out += "  " + shifted + " = lshr i64 " + source + ", 32\n";
                source = shifted;
            }
            const std::string low_word = newtemp();
            const std::string previous = newtemp();
            const std::string retained = newtemp();
            const std::string result = newtemp();
            m_out += "  " + low_word + " = and i64 " + source +
                     ", 4294967295\n";
            m_out += "  " + previous + " = load i64, ptr " +
                     state_field(field_indices[lane]) + "\n";
            m_out += "  " + retained + " = and i64 " + previous +
                     ", -4294967296\n";
            m_out += "  " + result + " = or i64 " + retained +
                     ", " + low_word + "\n";
            m_out += "  store i64 " + result + ", ptr " +
                     state_field(field_indices[lane]) + "\n";
        }
        return;
    }

    // PSLLVW / PSRLVW / PSRAVW: two 32-bit shifts, one per doubleword.
    // Each low 32-bit source word comes from rt[31:0] or rt[95:64];
    // the corresponding low word of rs supplies a masked five-bit count.
    // The shifted i32 result is SIGN-extended to the complete i64 rd lane.
    // PSRLVW shifts logically, PSRAVW shifts arithmetically; every
    // final i32-to-i64 extension is signed, including PSRLVW.
    // No HI/LO bank is read or modified, and rd=$zero discards the result.
    if ((fn == 0x09 && (sa == 0x02 || sa == 0x03)) ||
        (fn == 0x29 && sa == 0x03))
    {
        std::string results[2];
        for (uint32_t lane = 0u; lane < 2u; ++lane)
        {
            const std::string& count_source = lane == 0u ? a.first : a.second;
            const std::string& value_source = lane == 0u ? b.first : b.second;
            const std::string count32 = newtemp();
            const std::string count = newtemp();
            const std::string value32 = newtemp();
            const std::string shifted32 = newtemp();
            const std::string extended64 = newtemp();
            m_out += "  " + count32 + " = trunc i64 " + count_source +
                     " to i32\n";
            m_out += "  " + count + " = and i32 " + count32 + ", 31\n";
            m_out += "  " + value32 + " = trunc i64 " + value_source +
                     " to i32\n";
            m_out += "  " + shifted32 + " = " +
                     (fn == 0x29 ? "ashr" : sa == 0x02 ? "shl" : "lshr") +
                     " i32 " +
                     value32 + ", " + count + "\n";
            m_out += "  " + extended64 + " = sext i32 " +
                     shifted32 + " to i64\n";
            results[lane] = extended64;
        }
        store_gpr(ins.rd, results[0], results[1]);
        return;
    }

    // MMI2 / MMI3 share one architectural 128-bit HI/LO register pair.
    // MMI lanes 0 and 1 map to HI0:LO0 and HI1:LO1 respectively; each
    // accumulator half contains a sign-extended 32-bit word for PMULT/PDIV.
    if ((fn == 0x09 || fn == 0x29) &&
        (sa == 0x08 || sa == 0x09)) // PMFHI, PMFLO, PMTHI, PMTLO
    {
        const bool hi_bank = sa == 0x08;
        const uint32_t lower_field = hi_bank ? 1u : 2u;
        const uint32_t upper_field = hi_bank ? 3u : 4u;
        const std::string ptr_lo = state_field(lower_field);
        const std::string ptr_hi = state_field(upper_field);
        if (fn == 0x09) // PMFHI / PMFLO: full 128-bit GPR result
        {
            const std::string result_lo = newtemp();
            const std::string result_hi = newtemp();
            m_out += "  " + result_lo + " = load i64, ptr " + ptr_lo + "\n";
            m_out += "  " + result_hi + " = load i64, ptr " + ptr_hi + "\n";
            store_gpr(ins.rd, result_lo, result_hi);
        }
        else // PMTHI / PMTLO: source GPR -> full 128-bit accumulator
        {
            m_out += "  store i64 " + a.first + ", ptr " + ptr_lo + "\n";
            m_out += "  store i64 " + a.second + ", ptr " + ptr_hi + "\n";
        }
        return;
    }

    // PDIVBW (MMI2 sa=0x1d): four signed i32 numerators from rs are
    // divided independently by ONE signed i16 denominator from rt[15:0].
    // Each 32-bit quotient replaces a word of the 128-bit LO bank; each
    // corresponding remainder replaces the same word of HI. rd is unused.
    // PCSX2 specifies deterministic zero-divisor results, although the
    // EE hardware manuals do not define divide-by-zero behaviour.
    // Perform division in i64 to make INT_MIN/-1 well-defined in LLVM.
    if (fn == 0x09 && sa == 0x1d)
    {
        const std::string numerators = build4i32(a.first, a.second);
        const std::string divisor16 = newtemp();
        const std::string divisor64 = newtemp();
        const std::string divisor_zero = newtemp();
        const std::string safe_divisor = newtemp();
        m_out += "  " + divisor16 + " = trunc i64 " + b.first + " to i16\n";
        m_out += "  " + divisor64 + " = sext i16 " +
                 divisor16 + " to i64\n";
        m_out += "  " + divisor_zero + " = icmp eq i16 " +
                 divisor16 + ", 0\n";
        m_out += "  " + safe_divisor + " = select i1 " +
                 divisor_zero + ", i64 1, i64 " + divisor64 + "\n";

        std::string packed_quotients = "undef";
        std::string packed_remainders = "undef";
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            const std::string numerator32 = newtemp();
            const std::string numerator64 = newtemp();
            const std::string quotient64 = newtemp();
            const std::string remainder64 = newtemp();
            const std::string quotient32 = newtemp();
            const std::string remainder32 = newtemp();
            const std::string numerator_negative = newtemp();
            const std::string zero_quotient = newtemp();
            const std::string selected_quotient = newtemp();
            const std::string selected_remainder = newtemp();
            const std::string next_quotients = newtemp();
            const std::string next_remainders = newtemp();

            m_out += "  " + numerator32 + " = extractelement <4 x i32> " +
                     numerators + ", i32 " + std::to_string(lane) + "\n";
            m_out += "  " + numerator64 + " = sext i32 " +
                     numerator32 + " to i64\n";
            m_out += "  " + quotient64 + " = sdiv i64 " +
                     numerator64 + ", " + safe_divisor + "\n";
            m_out += "  " + remainder64 + " = srem i64 " +
                     numerator64 + ", " + safe_divisor + "\n";
            m_out += "  " + quotient32 + " = trunc i64 " +
                     quotient64 + " to i32\n";
            m_out += "  " + remainder32 + " = trunc i64 " +
                     remainder64 + " to i32\n";
            m_out += "  " + numerator_negative + " = icmp slt i32 " +
                     numerator32 + ", 0\n";
            m_out += "  " + zero_quotient + " = select i1 " +
                     numerator_negative + ", i32 1, i32 -1\n";
            m_out += "  " + selected_quotient + " = select i1 " +
                     divisor_zero + ", i32 " + zero_quotient +
                     ", i32 " + quotient32 + "\n";
            m_out += "  " + selected_remainder + " = select i1 " +
                     divisor_zero + ", i32 " + numerator32 +
                     ", i32 " + remainder32 + "\n";
            m_out += "  " + next_quotients + " = insertelement <4 x i32> " +
                     packed_quotients + ", i32 " + selected_quotient +
                     ", i32 " + std::to_string(lane) + "\n";
            m_out += "  " + next_remainders + " = insertelement <4 x i32> " +
                     packed_remainders + ", i32 " + selected_remainder +
                     ", i32 " + std::to_string(lane) + "\n";
            packed_quotients = next_quotients;
            packed_remainders = next_remainders;
        }

        // Bitcast preserves all four 32-bit words including upper 32-bit
        // halves of each 64-bit bank. Unlike PDIVW, sign extension of
        // individual HI/LO words to 64 bits would lose two result lanes.
        const std::string quotient_banks = newtemp();
        const std::string remainder_banks = newtemp();
        m_out += "  " + quotient_banks + " = bitcast <4 x i32> " +
                 packed_quotients + " to <2 x i64>\n";
        m_out += "  " + remainder_banks + " = bitcast <4 x i32> " +
                 packed_remainders + " to <2 x i64>\n";
        for (uint32_t bank = 0u; bank < 2u; ++bank)
        {
            const std::string quotient_bank = newtemp();
            const std::string remainder_bank = newtemp();
            m_out += "  " + quotient_bank + " = extractelement <2 x i64> " +
                     quotient_banks + ", i32 " + std::to_string(bank) + "\n";
            m_out += "  " + remainder_bank + " = extractelement <2 x i64> " +
                     remainder_banks + ", i32 " + std::to_string(bank) + "\n";
            m_out += "  store i64 " + quotient_bank + ", ptr " +
                     state_field(bank == 0u ? 2u : 4u) + "\n";
            m_out += "  store i64 " + remainder_bank + ", ptr " +
                     state_field(bank == 0u ? 1u : 3u) + "\n";
        }
        return;
    }

    // Halfword MMI2 multiplies eight signed i16 lanes into eight i32
    // accumulator words (LO0[0:1], HI0[0:1], LO1[0:1], HI1[0:1]).
    // PMADDH/PMSUBH wrap independently per 32-bit word and read prior
    // accumulators. PMULTH, PHMADH and PHMSBH replace all eight words.
    // PHMADH/PHMSBH upper words follow the PCSX2 interpreter and x86
    // recompiler's deterministic model; hardware marks them undocumented.
    if (fn == 0x09 &&
        (sa == 0x10 || sa == 0x11 || sa == 0x14 ||
         sa == 0x15 || sa == 0x1c))
    {
        const std::string lhs_vec = build8i16(a.first, a.second);
        const std::string rhs_vec = build8i16(b.first, b.second);
        const uint32_t field_indices[4] = {2u, 1u, 4u, 3u};
        std::string even_results[4];

        for (uint32_t bank = 0u; bank < 4u; ++bank)
        {
            std::string products[2];
            for (uint32_t half = 0u; half < 2u; ++half)
            {
                const uint32_t index = bank * 2u + half;
                const std::string lhs16 = newtemp();
                const std::string rhs16 = newtemp();
                const std::string lhs32 = newtemp();
                const std::string rhs32 = newtemp();
                const std::string product = newtemp();

                m_out += "  " + lhs16 + " = extractelement <8 x i16> " +
                         lhs_vec + ", i32 " + std::to_string(index) + "\n";
                m_out += "  " + rhs16 + " = extractelement <8 x i16> " +
                         rhs_vec + ", i32 " + std::to_string(index) + "\n";
                m_out += "  " + lhs32 + " = sext i16 " + lhs16 + " to i32\n";
                m_out += "  " + rhs32 + " = sext i16 " + rhs16 + " to i32\n";
                m_out += "  " + product + " = mul i32 " + lhs32 +
                         ", " + rhs32 + "\n";
                products[half] = product;
            }

            std::string lower_word;
            std::string upper_word;
            if (sa == 0x10 || sa == 0x14) // PMADDH / PMSUBH
            {
                const std::string prior = newtemp();
                const std::string prior_upper_shifted = newtemp();
                const std::string prior_lower = newtemp();
                const std::string prior_upper = newtemp();
                lower_word = newtemp();
                upper_word = newtemp();
                m_out += "  " + prior + " = load i64, ptr " +
                         state_field(field_indices[bank]) + "\n";
                m_out += "  " + prior_lower + " = trunc i64 " +
                         prior + " to i32\n";
                m_out += "  " + prior_upper_shifted + " = lshr i64 " +
                         prior + ", 32\n";
                m_out += "  " + prior_upper + " = trunc i64 " +
                         prior_upper_shifted + " to i32\n";
                const char* arithmetic = sa == 0x10 ? "add" : "sub";
                m_out += "  " + lower_word + " = " + arithmetic +
                         " i32 " + prior_lower + ", " + products[0] + "\n";
                m_out += "  " + upper_word + " = " + arithmetic +
                         " i32 " + prior_upper + ", " + products[1] + "\n";
            }
            else if (sa == 0x11) // PHMADH
            {
                lower_word = newtemp();
                m_out += "  " + lower_word + " = add i32 " + products[0] +
                         ", " + products[1] + "\n";
                upper_word = products[1];
            }
            else if (sa == 0x15) // PHMSBH
            {
                lower_word = newtemp();
                upper_word = newtemp();
                m_out += "  " + lower_word + " = sub i32 " + products[1] +
                         ", " + products[0] + "\n";
                m_out += "  " + upper_word + " = xor i32 " + products[1] +
                         ", -1\n";
            }
            else // PMULTH
            {
                lower_word = products[0];
                upper_word = products[1];
            }

            const std::string lower_bits = newtemp();
            const std::string upper_bits = newtemp();
            const std::string upper_shifted = newtemp();
            const std::string combined = newtemp();
            m_out += "  " + lower_bits + " = zext i32 " + lower_word +
                     " to i64\n";
            m_out += "  " + upper_bits + " = zext i32 " + upper_word +
                     " to i64\n";
            m_out += "  " + upper_shifted + " = shl i64 " + upper_bits +
                     ", 32\n";
            m_out += "  " + combined + " = or i64 " + upper_shifted +
                     ", " + lower_bits + "\n";
            m_out += "  store i64 " + combined + ", ptr " +
                     state_field(field_indices[bank]) + "\n";
            even_results[bank] = lower_word;
        }

        // GPR rd contains only the even accumulator words, interleaved
        // LO0[0], HI0[0], LO1[0], HI1[0] in four packed 32-bit lanes.
        std::string destination = "undef";
        for (uint32_t word = 0u; word < 4u; ++word)
        {
            const std::string next = newtemp();
            m_out += "  " + next + " = insertelement <4 x i32> " +
                     destination + ", i32 " + even_results[word] +
                     ", i32 " + std::to_string(word) + "\n";
            destination = next;
        }
        store_vec4i32(destination);
        return;
    }

    // PMADDW, PMSUBW and PMADDUW reconstruct two 64-bit HI:LO
    // accumulators from the low words of each sign-extended bank.
    // PCSX2's x86 recompiler uses wrapping 64-bit arithmetic; its older
    // interpreter contains unexplained signed MAC correction heuristics.
    // This is the independently testable recompiler model, not a claim
    // of bit-exact hardware multiply behaviour.
    if ((fn == 0x09 && (sa == 0x00 || sa == 0x04)) ||
        (fn == 0x29 && sa == 0x00))
    {
        const bool unsigned_product = fn == 0x29;
        const bool subtract = sa == 0x04;
        const char* extension = unsigned_product ? "zext" : "sext";
        std::string results[2];
        for (uint32_t lane = 0u; lane < 2u; ++lane)
        {
            const std::string& lhs = lane == 0u ? a.first : a.second;
            const std::string& rhs = lane == 0u ? b.first : b.second;
            const std::string lo_ptr = state_field(lane == 0u ? 2u : 4u);
            const std::string hi_ptr = state_field(lane == 0u ? 1u : 3u);
            const std::string lhs32 = newtemp(), rhs32 = newtemp();
            const std::string lhs64 = newtemp(), rhs64 = newtemp();
            const std::string lo_before = newtemp(), hi_before = newtemp();
            const std::string lo32_before = newtemp(), hi32_before = newtemp();
            const std::string lo_bits = newtemp(), hi_bits = newtemp();
            const std::string hi_shifted = newtemp(), accum = newtemp();
            const std::string product = newtemp(), updated = newtemp();
            const std::string lo32_after = newtemp(), hi_shifted_after = newtemp();
            const std::string hi32_after = newtemp();
            const std::string lo_after = newtemp(), hi_after = newtemp();

            m_out += "  " + lhs32 + " = trunc i64 " + lhs + " to i32\n";
            m_out += "  " + rhs32 + " = trunc i64 " + rhs + " to i32\n";
            m_out += "  " + lhs64 + " = " + extension + " i32 " + lhs32 + " to i64\n";
            m_out += "  " + rhs64 + " = " + extension + " i32 " + rhs32 + " to i64\n";
            m_out += "  " + lo_before + " = load i64, ptr " + lo_ptr + "\n";
            m_out += "  " + hi_before + " = load i64, ptr " + hi_ptr + "\n";
            m_out += "  " + lo32_before + " = trunc i64 " + lo_before + " to i32\n";
            m_out += "  " + hi32_before + " = trunc i64 " + hi_before + " to i32\n";
            m_out += "  " + lo_bits + " = zext i32 " + lo32_before + " to i64\n";
            m_out += "  " + hi_bits + " = zext i32 " + hi32_before + " to i64\n";
            m_out += "  " + hi_shifted + " = shl i64 " + hi_bits + ", 32\n";
            m_out += "  " + accum + " = or i64 " + hi_shifted + ", " + lo_bits + "\n";
            m_out += "  " + product + " = mul i64 " + lhs64 + ", " + rhs64 + "\n";
            // No LLVM nsw/nuw: two's-complement arithmetic wraps modulo 2^64.
            m_out += "  " + updated + " = " + (subtract ? "sub" : "add") +
                     " i64 " + accum + ", " + product + "\n";
            m_out += "  " + lo32_after + " = trunc i64 " + updated + " to i32\n";
            m_out += "  " + hi_shifted_after + " = lshr i64 " + updated + ", 32\n";
            m_out += "  " + hi32_after + " = trunc i64 " + hi_shifted_after + " to i32\n";
            m_out += "  " + lo_after + " = sext i32 " + lo32_after + " to i64\n";
            m_out += "  " + hi_after + " = sext i32 " + hi32_after + " to i64\n";
            m_out += "  store i64 " + lo_after + ", ptr " + lo_ptr + "\n";
            m_out += "  store i64 " + hi_after + ", ptr " + hi_ptr + "\n";
            results[lane] = updated;
        }
        store_gpr(ins.rd, results[0], results[1]);
        return;
    }

    if ((fn == 0x09 || fn == 0x29) &&
        (sa == 0x0c || sa == 0x0d)) // PMULTW/UW and PDIVW/UW
    {
        const bool is_unsigned = fn == 0x29;
        const bool multiply = sa == 0x0c;
        const char* extension = is_unsigned ? "zext" : "sext";
        std::string full_results[2];
        for (uint32_t lane = 0u; lane < 2u; ++lane)
        {
            const std::string& lhs = lane == 0u ? a.first : a.second;
            const std::string& rhs = lane == 0u ? b.first : b.second;
            const std::string lhs32 = newtemp();
            const std::string rhs32 = newtemp();
            const std::string lhs64 = newtemp();
            const std::string rhs64 = newtemp();
            m_out += "  " + lhs32 + " = trunc i64 " + lhs + " to i32\n";
            m_out += "  " + rhs32 + " = trunc i64 " + rhs + " to i32\n";
            m_out += "  " + lhs64 + " = " + extension + " i32 " +
                     lhs32 + " to i64\n";
            m_out += "  " + rhs64 + " = " + extension + " i32 " +
                     rhs32 + " to i64\n";

            std::string lo32;
            std::string hi32;
            if (multiply)
            {
                const std::string product = newtemp();
                const std::string high_shift = newtemp();
                lo32 = newtemp();
                hi32 = newtemp();
                m_out += "  " + product + " = mul i64 " + lhs64 +
                         ", " + rhs64 + "\n";
                m_out += "  " + lo32 + " = trunc i64 " + product + " to i32\n";
                m_out += "  " + high_shift + " = lshr i64 " + product + ", 32\n";
                m_out += "  " + hi32 + " = trunc i64 " + high_shift +
                         " to i32\n";
                full_results[lane] = product;
            }
            else
            {
                // All hardware division-by-zero results are undefined.
                // Follow PCSX2's explicit deterministic values rather than
                // generate LLVM's undefined sdiv/udiv on a zero divisor.
                // Signed INT_MIN/-1 is evaluated in i64, then narrowed.
                const std::string zero = newtemp();
                const std::string safe_rhs = newtemp();
                const std::string quotient = newtemp();
                const std::string remainder = newtemp();
                const std::string quotient32 = newtemp();
                const std::string remainder32 = newtemp();
                const std::string fallback_q = newtemp();
                lo32 = newtemp();
                hi32 = newtemp();
                m_out += "  " + zero + " = icmp eq i32 " + rhs32 + ", 0\n";
                m_out += "  " + safe_rhs + " = select i1 " + zero +
                         ", i64 1, i64 " + rhs64 + "\n";
                m_out += "  " + quotient + " = " +
                         (is_unsigned ? "udiv" : "sdiv") +
                         " i64 " + lhs64 + ", " + safe_rhs + "\n";
                m_out += "  " + remainder + " = " +
                         (is_unsigned ? "urem" : "srem") +
                         " i64 " + lhs64 + ", " + safe_rhs + "\n";
                m_out += "  " + quotient32 + " = trunc i64 " +
                         quotient + " to i32\n";
                m_out += "  " + remainder32 + " = trunc i64 " +
                         remainder + " to i32\n";
                if (is_unsigned)
                {
                    m_out += "  " + fallback_q + " = or i32 0, -1\n";
                }
                else
                {
                    const std::string negative = newtemp();
                    m_out += "  " + negative + " = icmp slt i32 " +
                             lhs32 + ", 0\n";
                    m_out += "  " + fallback_q + " = select i1 " +
                             negative + ", i32 1, i32 -1\n";
                }
                m_out += "  " + lo32 + " = select i1 " + zero +
                         ", i32 " + fallback_q + ", i32 " +
                         quotient32 + "\n";
                m_out += "  " + hi32 + " = select i1 " + zero +
                         ", i32 " + lhs32 + ", i32 " +
                         remainder32 + "\n";
            }

            const std::string lo64 = newtemp();
            const std::string hi64 = newtemp();
            m_out += "  " + lo64 + " = sext i32 " + lo32 + " to i64\n";
            m_out += "  " + hi64 + " = sext i32 " + hi32 + " to i64\n";
            m_out += "  store i64 " + lo64 + ", ptr " +
                     state_field(lane == 0u ? 2u : 4u) + "\n";
            m_out += "  store i64 " + hi64 + ", ptr " +
                     state_field(lane == 0u ? 1u : 3u) + "\n";
        }
        if (multiply) // PDIVW/UW has no GPR destination.
        {
            store_gpr(ins.rd, full_results[0], full_results[1]);
        }
        return;
    }

    // Top-level MMI (not class)
    if (fn == 0x18 || fn == 0x19)
    {
        // MULT1/MULTU1 write the 64-bit product to the upper HI1:LO1
        // accumulator pair and the sign-extended low word to rd.
        const bool is_unsigned = fn == 0x19;
        std::string lhs32 = newtemp();
        std::string rhs32 = newtemp();
        std::string lhs64 = newtemp();
        std::string rhs64 = newtemp();
        std::string product = newtemp();
        std::string lo32 = newtemp();
        std::string hi_shifted = newtemp();
        std::string hi32 = newtemp();
        std::string lo64 = newtemp();
        std::string hi64 = newtemp();
        std::string hi_ptr = state_field(3);
        std::string lo_ptr = state_field(4);

        m_out += "  " + lhs32 + " = trunc i64 " + a.first + " to i32\n";
        m_out += "  " + rhs32 + " = trunc i64 " + b.first + " to i32\n";
        const char* extend = is_unsigned ? "zext" : "sext";
        m_out += "  " + lhs64 + " = " + extend + " i32 " + lhs32 + " to i64\n";
        m_out += "  " + rhs64 + " = " + extend + " i32 " + rhs32 + " to i64\n";
        m_out += "  " + product + " = mul i64 " + lhs64 + ", " + rhs64 + "\n";
        m_out += "  " + lo32 + " = trunc i64 " + product + " to i32\n";
        m_out += "  " + hi_shifted + " = lshr i64 " + product + ", 32\n";
        m_out += "  " + hi32 + " = trunc i64 " + hi_shifted + " to i32\n";
        m_out += "  " + lo64 + " = sext i32 " + lo32 + " to i64\n";
        m_out += "  " + hi64 + " = sext i32 " + hi32 + " to i64\n";
        m_out += "  store i64 " + hi64 + ", ptr " + hi_ptr + "\n";
        m_out += "  store i64 " + lo64 + ", ptr " + lo_ptr + "\n";
        set_gpr_lo64(ins.rd, lo64);
        return;
    }
    if (fn == 0x1a || fn == 0x1b)
    {
        // DIV1/DIVU1 write HI1=rem and LO1=quotient from low 32-bit operands.
        // Match the hardware-observed zero-divisor results of DIV/DIVU,
        // preserving the independent HI/LO bank and avoiding LLVM poison.
        // Signed INT_MIN/-1 is defined by the EE as INT_MIN with remainder 0.
        const bool is_unsigned = fn == 0x1b;
        std::string lhs32 = newtemp(), rhs32 = newtemp(), div_zero = newtemp();
        std::string safe_rhs = newtemp(), quotient_raw = newtemp(), remainder_raw = newtemp();
        std::string quotient = newtemp(), remainder = newtemp();
        std::string hi_ptr = state_field(3), lo_ptr = state_field(4);
        m_out += "  " + lhs32 + " = trunc i64 " + a.first + " to i32\n";
        m_out += "  " + rhs32 + " = trunc i64 " + b.first + " to i32\n";
        m_out += "  " + div_zero + " = icmp eq i32 " + rhs32 + ", 0\n";
        m_out += "  " + safe_rhs + " = select i1 " + div_zero + ", i32 1, i32 " + rhs32 + "\n";
        if (is_unsigned)
        {
            m_out += "  " + quotient_raw + " = udiv i32 " + lhs32 + ", " + safe_rhs + "\n";
            m_out += "  " + remainder_raw + " = urem i32 " + lhs32 + ", " + safe_rhs + "\n";
            m_out += "  " + quotient + " = select i1 " + div_zero + ", i32 -1, i32 " + quotient_raw + "\n";
            m_out += "  " + remainder + " = select i1 " + div_zero + ", i32 " + lhs32 + ", i32 " + remainder_raw + "\n";
        }
        else
        {
            std::string is_min = newtemp(), is_neg_one = newtemp(), overflow = newtemp();
            std::string safe_lhs = newtemp(), safe_signed_rhs = newtemp();
            std::string quotient_select = newtemp(), remainder_select = newtemp();
            m_out += "  " + is_min + " = icmp eq i32 " + lhs32 + ", -2147483648\n";
            m_out += "  " + is_neg_one + " = icmp eq i32 " + rhs32 + ", -1\n";
            m_out += "  " + overflow + " = and i1 " + is_min + ", " + is_neg_one + "\n";
            m_out += "  " + safe_lhs + " = select i1 " + overflow + ", i32 0, i32 " + lhs32 + "\n";
            m_out += "  " + safe_signed_rhs + " = select i1 " + overflow + ", i32 1, i32 " + safe_rhs + "\n";
            m_out += "  " + quotient_raw + " = sdiv i32 " + safe_lhs + ", " + safe_signed_rhs + "\n";
            m_out += "  " + remainder_raw + " = srem i32 " + safe_lhs + ", " + safe_signed_rhs + "\n";
            m_out += "  " + quotient_select + " = select i1 " + overflow + ", i32 -2147483648, i32 " + quotient_raw + "\n";
            m_out += "  " + remainder_select + " = select i1 " + overflow + ", i32 0, i32 " + remainder_raw + "\n";
            const std::string negative = newtemp(), fallback = newtemp();
            m_out += "  " + negative + " = icmp slt i32 " + lhs32 + ", 0\n";
            m_out += "  " + fallback + " = select i1 " + negative + ", i32 1, i32 -1\n";
            m_out += "  " + quotient + " = select i1 " + div_zero + ", i32 " + fallback + ", i32 " + quotient_select + "\n";
            m_out += "  " + remainder + " = select i1 " + div_zero + ", i32 " + lhs32 + ", i32 " + remainder_select + "\n";
        }
        std::string quotient64 = newtemp(), remainder64 = newtemp();
        m_out += "  " + quotient64 + " = sext i32 " + quotient + " to i64\n";
        m_out += "  " + remainder64 + " = sext i32 " + remainder + " to i64\n";
        m_out += "  store i64 " + remainder64 + ", ptr " + hi_ptr + "\n";
        m_out += "  store i64 " + quotient64 + ", ptr " + lo_ptr + "\n";
        return;
    }
    if (fn == 0x00 || fn == 0x01 || fn == 0x20 || fn == 0x21)
    {
        // MADD/MADDU accumulate into the low 64-bit HI:LO pair. MADD1/
        // MADDU1 use the upper pair HI1:LO1. The accumulator is formed from
        // the low 32 bits of each special register; each resulting word is
        // sign-extended back to the 64-bit architectural register.
        const bool pipeline1 = fn == 0x20 || fn == 0x21;
        const bool is_unsigned = fn == 0x01 || fn == 0x21;
        const int hi_index = pipeline1 ? 3 : 1;
        const int lo_index = pipeline1 ? 4 : 2;

        std::string lhs32 = newtemp();
        std::string rhs32 = newtemp();
        std::string lhs64 = newtemp();
        std::string rhs64 = newtemp();
        std::string product = newtemp();
        std::string hi_ptr = state_field(hi_index);
        std::string lo_ptr = state_field(lo_index);
        std::string old_hi64 = newtemp();
        std::string old_lo64 = newtemp();
        std::string old_hi32 = newtemp();
        std::string old_lo32 = newtemp();
        std::string hi_part = newtemp();
        std::string lo_part = newtemp();
        std::string hi_shifted = newtemp();
        std::string accumulator = newtemp();
        std::string result = newtemp();
        std::string result_lo32 = newtemp();
        std::string result_hi_shifted = newtemp();
        std::string result_hi32 = newtemp();
        std::string result_lo64 = newtemp();
        std::string result_hi64 = newtemp();

        m_out += "  " + lhs32 + " = trunc i64 " + a.first + " to i32\n";
        m_out += "  " + rhs32 + " = trunc i64 " + b.first + " to i32\n";
        const char* extend = is_unsigned ? "zext" : "sext";
        m_out += "  " + lhs64 + " = " + extend + " i32 " + lhs32 + " to i64\n";
        m_out += "  " + rhs64 + " = " + extend + " i32 " + rhs32 + " to i64\n";
        m_out += "  " + product + " = mul i64 " + lhs64 + ", " + rhs64 + "\n";

        m_out += "  " + old_hi64 + " = load i64, ptr " + hi_ptr + "\n";
        m_out += "  " + old_lo64 + " = load i64, ptr " + lo_ptr + "\n";
        m_out += "  " + old_hi32 + " = trunc i64 " + old_hi64 + " to i32\n";
        m_out += "  " + old_lo32 + " = trunc i64 " + old_lo64 + " to i32\n";
        m_out += "  " + hi_part + " = zext i32 " + old_hi32 + " to i64\n";
        m_out += "  " + lo_part + " = zext i32 " + old_lo32 + " to i64\n";
        m_out += "  " + hi_shifted + " = shl i64 " + hi_part + ", 32\n";
        m_out += "  " + accumulator + " = or i64 " + hi_shifted + ", " + lo_part + "\n";
        m_out += "  " + result + " = add i64 " + accumulator + ", " + product + "\n";

        m_out += "  " + result_lo32 + " = trunc i64 " + result + " to i32\n";
        m_out += "  " + result_hi_shifted + " = lshr i64 " + result + ", 32\n";
        m_out += "  " + result_hi32 + " = trunc i64 " + result_hi_shifted + " to i32\n";
        m_out += "  " + result_lo64 + " = sext i32 " + result_lo32 + " to i64\n";
        m_out += "  " + result_hi64 + " = sext i32 " + result_hi32 + " to i64\n";
        m_out += "  store i64 " + result_hi64 + ", ptr " + hi_ptr + "\n";
        m_out += "  store i64 " + result_lo64 + ", ptr " + lo_ptr + "\n";
        set_gpr_lo64(ins.rd, result_lo64);
        return;
    }
    if (fn == 0x10) { // MFHI1
        std::string v = newtemp();
        m_out += "  " + v + " = load i64, ptr " + state_field(3) + "\n";
        set_gpr_lo64(ins.rd, v);
        return;
    }
    if (fn == 0x12) { // MFLO1
        std::string v = newtemp();
        m_out += "  " + v + " = load i64, ptr " + state_field(4) + "\n";
        set_gpr_lo64(ins.rd, v);
        return;
    }
    if (fn == 0x11) { // MTHI1
        m_out += "  store i64 " + a.first + ", ptr " + state_field(3) + "\n";
        return;
    }
    if (fn == 0x13) { // MTLO1
        m_out += "  store i64 " + a.first + ", ptr " + state_field(4) + "\n";
        return;
    }

    // Class-based MMI0/1/2/3
    if (fn == 0x08 || fn == 0x09 || fn == 0x28 || fn == 0x29) {
        std::string va4 = build4i32(a.first, a.second);
        std::string vb4 = build4i32(b.first, b.second);
        std::string va2 = build2i64(a.first, a.second);
        std::string vb2 = build2i64(b.first, b.second);
        std::string va8 = build8i16(a.first, a.second);
        std::string vb8 = build8i16(b.first, b.second);
        std::string va16 = build16i8(a.first, a.second);
        std::string vb16 = build16i8(b.first, b.second);
        std::string vr = newtemp();

        // Unary MMI2 lane permutations use rt exclusively. The 128-bit
        // value is assembled before writing rd, including rd==rt.
        if (fn == 0x09 &&
            (sa == 0x1a || sa == 0x1b || sa == 0x1e || sa == 0x1f))
        {
            const bool halfword = sa == 0x1a || sa == 0x1b;
            const char* mask = sa == 0x1a
                ? "<i32 2, i32 1, i32 0, i32 3, i32 6, i32 5, i32 4, i32 7>"
                : sa == 0x1b
                ? "<i32 3, i32 2, i32 1, i32 0, i32 7, i32 6, i32 5, i32 4>"
                : sa == 0x1e
                ? "<i32 2, i32 1, i32 0, i32 3>"
                : "<i32 1, i32 2, i32 0, i32 3>";
            const std::string type = halfword ? "<8 x i16>" : "<4 x i32>";
            const std::string& input = halfword ? vb8 : vb4;
            const std::string result = newtemp();
            m_out += "  " + result + " = shufflevector " + type + " " +
                     input + ", " + type + " " + input + ", " +
                     (halfword ? "<8 x i32> " : "<4 x i32> ") +
                     mask + "\n";
            if (halfword)
            {
                store_vec8i16(result);
            }
            else
            {
                store_vec4i32(result);
            }
            return;
        }

        // MMI2 PINTH and MMI3 PINTEH use distinct full-128-bit
        // halfword interleaves. Element indexes 0..7 refer to rs;
        // indexes 8..15 refer to rt. Snapshot both sources before
        // writing rd so aliases with rs/rt remain exact.
        if ((fn == 0x09 || fn == 0x29) && sa == 0x0a)
        {
            const std::string result = newtemp();
            const char* mask = fn == 0x09
                ? "<i32 8, i32 4, i32 9, i32 5, i32 10, i32 6, i32 11, i32 7>"
                : "<i32 8, i32 0, i32 10, i32 2, i32 12, i32 4, i32 14, i32 6>";
            m_out += "  " + result + " = shufflevector <8 x i16> " +
                     va8 + ", <8 x i16> " + vb8 + ", <8 x i32> " +
                     mask + "\n";
            store_vec8i16(result);
            return;
        }

        // MMI0 (0x08): independent packed lanes. Saturating operations
        // widen before arithmetic so LLVM never sees overflow/poison.
        if (fn == 0x08) {
            const auto splat = [](const std::string& scalar, uint32_t lanes,
                                  int64_t value) -> std::string
            {
                std::string result = "<";
                for (uint32_t lane = 0u; lane < lanes; ++lane)
                {
                    if (lane != 0u)
                    {
                        result += ", ";
                    }
                    result += std::string(scalar) + " " + std::to_string(value);
                }
                return result + ">";
            };
            const auto emit_signed_saturation =
                [&](const std::string& lhs, const std::string& rhs,
                    const std::string& narrow_type,
                    const std::string& wide_type,
                    const std::string& wide_scalar,
                    uint32_t lanes, int64_t low, int64_t high,
                    bool addition) -> std::string
            {
                const std::string lhs_wide = newtemp();
                const std::string rhs_wide = newtemp();
                const std::string calculated = newtemp();
                const std::string above = newtemp();
                const std::string below = newtemp();
                const std::string capped = newtemp();
                const std::string clamped = newtemp();
                const std::string result = newtemp();
                const std::string predicate_type =
                    "<" + std::to_string(lanes) + " x i1>";
                const std::string limits_high = splat(wide_scalar, lanes, high);
                const std::string limits_low = splat(wide_scalar, lanes, low);
                m_out += "  " + lhs_wide + " = sext " + narrow_type +
                         " " + lhs + " to " + wide_type + "\n";
                m_out += "  " + rhs_wide + " = sext " + narrow_type +
                         " " + rhs + " to " + wide_type + "\n";
                m_out += "  " + calculated + " = " +
                         (addition ? "add" : "sub") + " " + wide_type +
                         " " + lhs_wide + ", " + rhs_wide + "\n";
                m_out += "  " + above + " = icmp sgt " + wide_type +
                         " " + calculated + ", " + limits_high + "\n";
                m_out += "  " + below + " = icmp slt " + wide_type +
                         " " + calculated + ", " + limits_low + "\n";
                m_out += "  " + capped + " = select " + predicate_type +
                         " " + above + ", " + wide_type + " " +
                         limits_high + ", " + wide_type + " " +
                         calculated + "\n";
                m_out += "  " + clamped + " = select " + predicate_type +
                         " " + below + ", " + wide_type + " " +
                         limits_low + ", " + wide_type + " " + capped + "\n";
                m_out += "  " + result + " = trunc " + wide_type + " " +
                         clamped + " to " + narrow_type + "\n";
                return result;
            };
            switch (sa) {
            case 0: // PADDW
                m_out += "  " + vr + " = add <4 x i32> " + va4 + ", " + vb4 + "\n";
                store_vec4i32(vr); return;
            case 1: // PSUBW
                m_out += "  " + vr + " = sub <4 x i32> " + va4 + ", " + vb4 + "\n";
                store_vec4i32(vr); return;
            case 2: // PCGTW (signed greater-than; true lanes all ones)
            {
                const std::string predicate = newtemp();
                const std::string result = newtemp();
                m_out += "  " + predicate + " = icmp sgt <4 x i32> " +
                         va4 + ", " + vb4 + "\n";
                m_out += "  " + result + " = sext <4 x i1> " +
                         predicate + " to <4 x i32>\n";
                store_vec4i32(result);
                return;
            }
            case 3: // PMAXW (signed)
                m_out += "  " + vr + " = call <4 x i32> @llvm.smax.v4i32(<4 x i32> " + va4 + ", <4 x i32> " + vb4 + ")\n";
                store_vec4i32(vr); return;
            case 4: // PADDH
                m_out += "  " + vr + " = add <8 x i16> " + va8 + ", " + vb8 + "\n";
                store_vec8i16(vr); return;
            case 5: // PSUBH
                m_out += "  " + vr + " = sub <8 x i16> " + va8 + ", " + vb8 + "\n";
                store_vec8i16(vr); return;
            case 6: // PCGTH
            {
                const std::string predicate = newtemp();
                const std::string result = newtemp();
                m_out += "  " + predicate + " = icmp sgt <8 x i16> " +
                         va8 + ", " + vb8 + "\n";
                m_out += "  " + result + " = sext <8 x i1> " +
                         predicate + " to <8 x i16>\n";
                store_vec8i16(result);
                return;
            }
            case 7: // PMAXH
                m_out += "  " + vr + " = call <8 x i16> @llvm.smax.v8i16(<8 x i16> " + va8 + ", <8 x i16> " + vb8 + ")\n";
                store_vec8i16(vr); return;
            case 8: // PADDB
                m_out += "  " + vr + " = add <16 x i8> " + va16 + ", " + vb16 + "\n";
                store_vec16i8(vr); return;
            case 9: // PSUBB
                m_out += "  " + vr + " = sub <16 x i8> " + va16 + ", " + vb16 + "\n";
                store_vec16i8(vr); return;
            case 10: // PCGTB
            {
                const std::string predicate = newtemp();
                const std::string result = newtemp();
                m_out += "  " + predicate + " = icmp sgt <16 x i8> " +
                         va16 + ", " + vb16 + "\n";
                m_out += "  " + result + " = sext <16 x i1> " +
                         predicate + " to <16 x i8>\n";
                store_vec16i8(result);
                return;
            }
            case 16: case 17: // PADDSW/PSUBSW
                store_vec4i32(emit_signed_saturation(
                    va4, vb4, "<4 x i32>", "<4 x i64>", "i64", 4u,
                    -2147483648LL, 2147483647LL, sa == 16u));
                return;
            case 18: // PEXTLW: low-word interleave rt0,rs0,rt1,rs1
            {
                const std::string result = newtemp();
                m_out += "  " + result + " = shufflevector <4 x i32> " +
                         va4 + ", <4 x i32> " + vb4 +
                         ", <4 x i32> <i32 4, i32 0, i32 5, i32 1>\n";
                store_vec4i32(result);
                return;
            }
            case 19: // PPACW: rt even words, then rs even words
            {
                const std::string result = newtemp();
                m_out += "  " + result + " = shufflevector <4 x i32> " +
                         va4 + ", <4 x i32> " + vb4 +
                         ", <4 x i32> <i32 4, i32 6, i32 0, i32 2>\n";
                store_vec4i32(result);
                return;
            }
            case 20: case 21: // PADDSH/PSUBSH
                store_vec8i16(emit_signed_saturation(
                    va8, vb8, "<8 x i16>", "<8 x i32>", "i32", 8u,
                    -32768LL, 32767LL, sa == 20u));
                return;
            case 22: // PEXTLH: low-halfword interleave
            {
                const std::string result = newtemp();
                m_out += "  " + result + " = shufflevector <8 x i16> " +
                         va8 + ", <8 x i16> " + vb8 +
                         ", <8 x i32> <i32 8, i32 0, i32 9, i32 1, i32 10, i32 2, i32 11, i32 3>\n";
                store_vec8i16(result);
                return;
            }
            case 23: // PPACH: packed even halfwords
            {
                const std::string result = newtemp();
                m_out += "  " + result + " = shufflevector <8 x i16> " +
                         va8 + ", <8 x i16> " + vb8 +
                         ", <8 x i32> <i32 8, i32 10, i32 12, i32 14, i32 0, i32 2, i32 4, i32 6>\n";
                store_vec8i16(result);
                return;
            }
            case 24: case 25: // PADDSB/PSUBSB
                store_vec16i8(emit_signed_saturation(
                    va16, vb16, "<16 x i8>", "<16 x i16>", "i16", 16u,
                    -128LL, 127LL, sa == 24u));
                return;
            case 26: // PEXTLB: low-byte interleave
            {
                const std::string result = newtemp();
                m_out += "  " + result + " = shufflevector <16 x i8> " +
                         va16 + ", <16 x i8> " + vb16 +
                         ", <16 x i32> <i32 16, i32 0, i32 17, i32 1, i32 18, i32 2, i32 19, i32 3, i32 20, i32 4, i32 21, i32 5, i32 22, i32 6, i32 23, i32 7>\n";
                store_vec16i8(result);
                return;
            }
            case 27: // PPACB: rt even bytes, then rs even bytes
            {
                const std::string result = newtemp();
                m_out += "  " + result + " = shufflevector <16 x i8> " +
                         va16 + ", <16 x i8> " + vb16 +
                         ", <16 x i32> <i32 16, i32 18, i32 20, i32 22, i32 24, i32 26, i32 28, i32 30, i32 0, i32 2, i32 4, i32 6, i32 8, i32 10, i32 12, i32 14>\n";
                store_vec16i8(result);
                return;
            }
            case 30: case 31: // PEXT5 / PPAC5, unary pixel conversion
            {
                const bool pack = sa == 31u;
                const uint32_t masks[] = {0x1fu, 0x3e0u, 0x7c00u, 0x8000u};
                const uint32_t shifts[] = {3u, 6u, 9u, 16u};
                std::string combined;
                for (uint32_t component = 0u; component < 4u; ++component)
                {
                    const std::string intermediate = newtemp();
                    const std::string next = newtemp();
                    const std::string shifted = newtemp();
                    const std::string lane_mask = splat("i32", 4u, masks[component]);
                    const std::string lane_shift = splat("i32", 4u, shifts[component]);
                    if (pack)
                    {
                        m_out += "  " + shifted + " = lshr <4 x i32> " + vb4 +
                                 ", " + lane_shift + "\n";
                        m_out += "  " + intermediate + " = and <4 x i32> " +
                                 shifted + ", " + lane_mask + "\n";
                    }
                    else
                    {
                        m_out += "  " + shifted + " = and <4 x i32> " + vb4 +
                                 ", " + lane_mask + "\n";
                        m_out += "  " + intermediate + " = shl <4 x i32> " +
                                 shifted + ", " + lane_shift + "\n";
                    }
                    if (component == 0u)
                    {
                        combined = intermediate;
                    }
                    else
                    {
                        m_out += "  " + next + " = or <4 x i32> " + combined +
                                 ", " + intermediate + "\n";
                        combined = next;
                    }
                }
                store_vec4i32(combined);
                return;
            }
            default:
                // Generic: 32-bit parallel add as fallback
                m_out += "  " + vr + " = add <4 x i32> " + va4 + ", " + vb4 + "\n";
                store_vec4i32(vr); return;
            }
        }
        // MMI2 (0x09): PCPYLD / PAND / PXOR
        if (fn == 0x09) {
            switch (sa) {
            case 0x0e: // PCPYLD: rd[63:0]=rt[63:0], rd[127:64]=rs[63:0]
                store_gpr(ins.rd, b.first, a.first);
                return;
            case 0x12: // PAND
                m_out += "  " + vr + " = and <2 x i64> " + va2 + ", " + vb2 + "\n";
                store_vec2i64(vr); return;
            case 0x13: // PXOR (MMI2 has xor at different place; treat)
                m_out += "  " + vr + " = xor <2 x i64> " + va2 + ", " + vb2 + "\n";
                store_vec2i64(vr); return;
            case 0x14: // spare
                m_out += "  " + vr + " = or <2 x i64> " + va2 + ", " + vb2 + "\n";
                store_vec2i64(vr); return;
            default:
                m_out += "  " + vr + " = and <2 x i64> " + va2 + ", " + vb2 + "\n";
                store_vec2i64(vr); return;
            }
        }
        // MMI1 (0x28): only operations with exact lane semantics are
        // enabled in the decoder. PADDU*/PSUBU* use widened lane arithmetic
        // so saturation is explicit and independent for every element.
        if (fn == 0x28)
        {
            auto emit_unsigned_sat = [&](const std::string& lhs,
                                         const std::string& rhs,
                                         const char* narrow_type,
                                         const char* wide_type,
                                         const char* predicate_type,
                                         const char* wide_scalar,
                                         unsigned lane_count,
                                         uint64_t lane_max,
                                         bool is_add) -> std::string
            {
                const std::string lhs_wide = newtemp();
                const std::string rhs_wide = newtemp();
                const std::string arithmetic = newtemp();
                const std::string condition = newtemp();
                const std::string saturated = newtemp();
                const std::string result = newtemp();

                std::string max_vector = "<";
                for (unsigned i = 0; i < lane_count; ++i)
                {
                    if (i != 0) max_vector += ", ";
                    max_vector += std::string(wide_scalar) + " " + std::to_string(lane_max);
                }
                max_vector += ">";

                m_out += "  " + lhs_wide + " = zext " + narrow_type + " " + lhs +
                         " to " + wide_type + "\n";
                m_out += "  " + rhs_wide + " = zext " + narrow_type + " " + rhs +
                         " to " + wide_type + "\n";
                if (is_add)
                {
                    m_out += "  " + arithmetic + " = add " + wide_type + " " +
                             lhs_wide + ", " + rhs_wide + "\n";
                    m_out += "  " + condition + " = icmp ugt " + wide_type + " " +
                             arithmetic + ", " + max_vector + "\n";
                    m_out += "  " + saturated + " = select " + predicate_type + " " +
                             condition + ", " + wide_type + " " + max_vector +
                             ", " + wide_type + " " + arithmetic + "\n";
                }
                else
                {
                    m_out += "  " + arithmetic + " = sub " + wide_type + " " +
                             lhs_wide + ", " + rhs_wide + "\n";
                    m_out += "  " + condition + " = icmp ult " + wide_type + " " +
                             lhs_wide + ", " + rhs_wide + "\n";
                    m_out += "  " + saturated + " = select " + predicate_type + " " +
                             condition + ", " + wide_type + " zeroinitializer, " +
                             wide_type + " " + arithmetic + "\n";
                }
                m_out += "  " + result + " = trunc " + wide_type + " " +
                         saturated + " to " + narrow_type + "\n";
                return result;
            };

            switch (sa)
            {
            case 0x01: // PABSW: unary rt -> rd; clamp INT_MIN to INT_MAX
            {
                const std::string negative = newtemp();
                const std::string negated = newtemp();
                const std::string absolute = newtemp();
                const std::string is_min = newtemp();
                const std::string result = newtemp();
                m_out += "  " + negative + " = icmp slt <4 x i32> " + vb4 +
                         ", zeroinitializer\n";
                m_out += "  " + negated + " = sub <4 x i32> zeroinitializer, " +
                         vb4 + "\n";
                m_out += "  " + absolute + " = select <4 x i1> " + negative +
                         ", <4 x i32> " + negated + ", <4 x i32> " + vb4 + "\n";
                m_out += "  " + is_min + " = icmp eq <4 x i32> " + vb4 +
                         ", <i32 -2147483648, i32 -2147483648, i32 -2147483648, i32 -2147483648>\n";
                m_out += "  " + result + " = select <4 x i1> " + is_min +
                         ", <4 x i32> <i32 2147483647, i32 2147483647, i32 2147483647, i32 2147483647>, <4 x i32> " +
                         absolute + "\n";
                store_vec4i32(result);
                return;
            }
            case 0x05: // PABSH: unary rt -> rd; clamp INT_MIN to INT_MAX
            {
                const std::string wide = newtemp();
                const std::string negative = newtemp();
                const std::string negated = newtemp();
                const std::string absolute = newtemp();
                const std::string is_min = newtemp();
                const std::string saturated = newtemp();
                const std::string result = newtemp();
                m_out += "  " + wide + " = sext <8 x i16> " + vb8 + " to <8 x i32>\n";
                m_out += "  " + negative + " = icmp slt <8 x i32> " + wide +
                         ", zeroinitializer\n";
                m_out += "  " + negated + " = sub <8 x i32> zeroinitializer, " +
                         wide + "\n";
                m_out += "  " + absolute + " = select <8 x i1> " + negative +
                         ", <8 x i32> " + negated + ", <8 x i32> " + wide + "\n";
                m_out += "  " + is_min + " = icmp eq <8 x i32> " + wide +
                         ", <i32 -32768, i32 -32768, i32 -32768, i32 -32768, i32 -32768, i32 -32768, i32 -32768, i32 -32768>\n";
                m_out += "  " + saturated + " = select <8 x i1> " + is_min +
                         ", <8 x i32> <i32 32767, i32 32767, i32 32767, i32 32767, i32 32767, i32 32767, i32 32767, i32 32767>, <8 x i32> " +
                         absolute + "\n";
                m_out += "  " + result + " = trunc <8 x i32> " + saturated +
                         " to <8 x i16>\n";
                store_vec8i16(result);
                return;
            }
            case 0x02: // PCEQW
                m_out += "  " + vr + " = icmp eq <4 x i32> " + va4 + ", " + vb4 + "\n";
                {
                    const std::string result = newtemp();
                    m_out += "  " + result + " = sext <4 x i1> " + vr + " to <4 x i32>\n";
                    store_vec4i32(result);
                }
                return;
            case 0x03: // PMINW (signed)
                {
                    const std::string cmp = newtemp();
                    m_out += "  " + cmp + " = icmp slt <4 x i32> " + va4 + ", " + vb4 + "\n";
                    m_out += "  " + vr + " = select <4 x i1> " + cmp +
                             ", <4 x i32> " + va4 + ", <4 x i32> " + vb4 + "\n";
                    store_vec4i32(vr);
                }
                return;
            case 0x06: // PCEQH
                {
                    const std::string cmp = newtemp();
                    m_out += "  " + cmp + " = icmp eq <8 x i16> " + va8 + ", " + vb8 + "\n";
                    const std::string result = newtemp();
                    m_out += "  " + result + " = sext <8 x i1> " + cmp + " to <8 x i16>\n";
                    store_vec8i16(result);
                }
                return;
            case 0x07: // PMINH (signed)
                {
                    const std::string cmp = newtemp();
                    m_out += "  " + cmp + " = icmp slt <8 x i16> " + va8 + ", " + vb8 + "\n";
                    m_out += "  " + vr + " = select <8 x i1> " + cmp +
                             ", <8 x i16> " + va8 + ", <8 x i16> " + vb8 + "\n";
                    store_vec8i16(vr);
                }
                return;
            case 0x0a: // PCEQB
                {
                    const std::string cmp = newtemp();
                    m_out += "  " + cmp + " = icmp eq <16 x i8> " + va16 + ", " + vb16 + "\n";
                    const std::string result = newtemp();
                    m_out += "  " + result + " = sext <16 x i1> " + cmp + " to <16 x i8>\n";
                    store_vec16i8(result);
                }
                return;
            case 0x04: // PADSBH: subtract low halfwords and add high halfwords.
            {
                const std::string diff = newtemp();
                const std::string sum = newtemp();
                const std::string result = newtemp();
                m_out += "  " + diff + " = sub <8 x i16> " + va8 + ", " + vb8 + "\n";
                m_out += "  " + sum + " = add <8 x i16> " + va8 + ", " + vb8 + "\n";
                m_out += "  " + result + " = shufflevector <8 x i16> " + diff +
                         ", <8 x i16> " + sum +
                         ", <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 12, i32 13, i32 14, i32 15>\n";
                store_vec8i16(result);
                return;
            }
            case 0x12: // PEXTUW: interleave upper two words from rs and rt.
            {
                const std::string result = newtemp();
                m_out += "  " + result + " = shufflevector <4 x i32> " + va4 +
                         ", <4 x i32> " + vb4 +
                         ", <4 x i32> <i32 6, i32 2, i32 7, i32 3>\n";
                store_vec4i32(result);
                return;
            }
            case 0x16: // PEXTUH: interleave upper four halfwords from rs and rt.
            {
                const std::string result = newtemp();
                m_out += "  " + result + " = shufflevector <8 x i16> " + va8 +
                         ", <8 x i16> " + vb8 +
                         ", <8 x i32> <i32 12, i32 4, i32 13, i32 5, i32 14, i32 6, i32 15, i32 7>\n";
                store_vec8i16(result);
                return;
            }
            case 0x1a: // PEXTUB: interleave upper eight bytes from rs and rt.
            {
                const std::string result = newtemp();
                m_out += "  " + result + " = shufflevector <16 x i8> " + va16 +
                         ", <16 x i8> " + vb16 +
                         ", <16 x i32> <i32 24, i32 8, i32 25, i32 9, i32 26, i32 10, i32 27, i32 11, i32 28, i32 12, i32 29, i32 13, i32 30, i32 14, i32 31, i32 15>\n";
                store_vec16i8(result);
                return;
            }
            case 0x1b: // QFSRV: right funnel shift of {rs,rt} by the SA register.
            {
                const std::string a_vec = build2i64(a.first, a.second);
                const std::string b_vec = build2i64(b.first, b.second);
                const std::string source_a = newtemp();
                const std::string source_b = newtemp();
                const std::string sa_raw = newtemp();
                const std::string sa_masked = newtemp();
                const std::string is_zero = newtemp();
                const std::string sa_safe = newtemp();
                const std::string sa128 = newtemp();
                const std::string inverse = newtemp();
                const std::string upper = newtemp();
                const std::string lower = newtemp();
                const std::string combined = newtemp();
                const std::string result = newtemp();
                const std::string result_lo = newtemp();
                const std::string result_hi_shifted = newtemp();
                const std::string result_hi = newtemp();
                const std::string sa_ptr = state_field(5);

                m_out += "  " + source_a + " = bitcast <2 x i64> " + a_vec + " to i128\n";
                m_out += "  " + source_b + " = bitcast <2 x i64> " + b_vec + " to i128\n";
                m_out += "  " + sa_raw + " = load i32, ptr " + sa_ptr + "\n";
                // MTSAB/MTSAH generate aligned bit counts in 0..120. Keep
                // malformed saved state from producing an out-of-range IR shift.
                m_out += "  " + sa_masked + " = and i32 " + sa_raw + ", 127\n";
                m_out += "  " + is_zero + " = icmp eq i32 " + sa_masked + ", 0\n";
                m_out += "  " + sa_safe + " = select i1 " + is_zero + ", i32 1, i32 " + sa_masked + "\n";
                m_out += "  " + sa128 + " = zext i32 " + sa_safe + " to i128\n";
                m_out += "  " + inverse + " = sub i128 128, " + sa128 + "\n";
                m_out += "  " + upper + " = shl i128 " + source_a + ", " + inverse + "\n";
                m_out += "  " + lower + " = lshr i128 " + source_b + ", " + sa128 + "\n";
                m_out += "  " + combined + " = or i128 " + upper + ", " + lower + "\n";
                m_out += "  " + result + " = select i1 " + is_zero + ", i128 " + source_b + ", i128 " + combined + "\n";
                m_out += "  " + result_lo + " = trunc i128 " + result + " to i64\n";
                m_out += "  " + result_hi_shifted + " = lshr i128 " + result + ", 64\n";
                m_out += "  " + result_hi + " = trunc i128 " + result_hi_shifted + " to i64\n";
                store_gpr(ins.rd, result_lo, result_hi);
                return;
            }
            case 0x10: // PADDUW
                store_vec4i32(emit_unsigned_sat(va4, vb4, "<4 x i32>", "<4 x i64>",
                                                "<4 x i1>", "i64", 4, 0xFFFFFFFFull, true));
                return;
            case 0x11: // PSUBUW
                store_vec4i32(emit_unsigned_sat(va4, vb4, "<4 x i32>", "<4 x i64>",
                                                "<4 x i1>", "i64", 4, 0xFFFFFFFFull, false));
                return;
            case 0x14: // PADDUH
                store_vec8i16(emit_unsigned_sat(va8, vb8, "<8 x i16>", "<8 x i32>",
                                                "<8 x i1>", "i32", 8, 0xFFFFull, true));
                return;
            case 0x15: // PSUBUH
                store_vec8i16(emit_unsigned_sat(va8, vb8, "<8 x i16>", "<8 x i32>",
                                                "<8 x i1>", "i32", 8, 0xFFFFull, false));
                return;
            case 0x18: // PADDUB
                store_vec16i8(emit_unsigned_sat(va16, vb16, "<16 x i8>", "<16 x i16>",
                                                "<16 x i1>", "i16", 16, 0xFFull, true));
                return;
            case 0x19: // PSUBUB
                store_vec16i8(emit_unsigned_sat(va16, vb16, "<16 x i8>", "<16 x i16>",
                                                "<16 x i1>", "i16", 16, 0xFFull, false));
                return;
            default:
                // The decoder leaves every other MMI1 sub-op unsupported.
                m_out += "  ; unsupported MMI1 sub-op " + std::to_string(sa) + "\n";
                return;
            }
        }
        // MMI3 (0x29)
        if (fn == 0x29) {
            switch (sa) {
            case 0x0e: // PCPYUD: low64=rs.upper64, high64=rt.upper64
                // Capture both source halves before writing rd, including
                // cases where the destination aliases rs or rt.
                store_gpr(ins.rd, a.second, b.second);
                return;
            case 0x12: // POR
                m_out += "  " + vr + " = or <2 x i64> " + va2 + ", " + vb2 + "\n";
                store_vec2i64(vr); return;
            case 0x13: // PNOR
                {
                    std::string vo = newtemp();
                    m_out += "  " + vo + " = or <2 x i64> " + va2 + ", " + vb2 + "\n";
                    m_out += "  " + vr + " = xor <2 x i64> " + vo + ", <i64 -1, i64 -1>\n";
                }
                store_vec2i64(vr); return;
            case 0x1b: // PCPYH: each doubleword broadcasts its own low halfword
            {
                const std::string low_halfword = newtemp();
                const std::string high_halfword = newtemp();
                const std::string result_lo = newtemp();
                const std::string result_hi = newtemp();
                m_out += "  " + low_halfword + " = and i64 " + b.first + ", 65535\n";
                m_out += "  " + high_halfword + " = and i64 " + b.second + ", 65535\n";
                m_out += "  " + result_lo + " = mul i64 " + low_halfword +
                         ", 281479271743489\n"; // 0x0001000100010001
                m_out += "  " + result_hi + " = mul i64 " + high_halfword +
                         ", 281479271743489\n";
                store_gpr(ins.rd, result_lo, result_hi);
                return;
            }
            default:
                m_out += "  " + vr + " = or <2 x i64> " + va2 + ", " + vb2 + "\n";
                store_vec2i64(vr); return;
            }
        }
    }

    // Legacy enum-based paths (old FUNCT_MMI_* values) + generic fallback
    switch (static_cast<ee::e_ee_opcode>(fn)) {
    case ee::e_ee_opcode::FUNCT_MMI_AND: {
        std::string vr = newtemp();
        m_out += "  " + vr + " = and <2 x i64> " + build2i64(a.first,a.second) + ", " + build2i64(b.first,b.second) + "\n";
        store_vec2i64(vr); break;
    }
    case ee::e_ee_opcode::FUNCT_MMI_OR: {
        std::string vr = newtemp();
        m_out += "  " + vr + " = or <2 x i64> " + build2i64(a.first,a.second) + ", " + build2i64(b.first,b.second) + "\n";
        store_vec2i64(vr); break;
    }
    case ee::e_ee_opcode::FUNCT_MMI_XOR: {
        std::string vr = newtemp();
        m_out += "  " + vr + " = xor <2 x i64> " + build2i64(a.first,a.second) + ", " + build2i64(b.first,b.second) + "\n";
        store_vec2i64(vr); break;
    }
    default: {
        // Safe fallback: copy rs → rd (keeps 128-bit shape)
        store_gpr(ins.rd, a.first, a.second);
        m_out += "  ; mmi funct=0x" + u32hex(fn) + " sa=" + std::to_string(sa) + " fallback move\n";
        break;
    }
    }
}

void module_builder::emit_function_cfg(const ee::s_ee_function& fn,
                                       const std::map<uint32_t, ee::s_ee_instruction>& insn_map,
                                       const char* func_name)
{
    using ee::e_ee_opcode;
    header();
    declare_memory_helpers();

    // Compute written/rbw per block. block_killed_map is the must-write set
    // used by liveness: a branch-likely delay slot executes only when the
    // branch is taken, so its writes cannot kill a register that the
    // not-taken successor still reads (SDBZ 0x166410 BEQL with delay slot
    // "ld s0,0(sp)" otherwise dropped the preceding "lw s0" result).
    std::map<uint32_t, std::set<uint8_t>> block_written_map, block_rbw_map;
    std::map<uint32_t, std::set<uint8_t>> block_killed_map;
    std::vector<std::vector<ee::s_ee_instruction>> block_insns_vec;
    for (const auto& bb : fn.blocks)
    {
        std::vector<ee::s_ee_instruction> insns;
        for (uint32_t pc = bb.start_pc; pc < bb.end_pc; pc += 4)
        {
            auto it = insn_map.find(pc);
            if (it != insn_map.end()) insns.push_back(it->second);
        }
        block_insns_vec.push_back(insns);
        block_written_map[bb.start_pc] = block_written(insns);
        if (insns.size() >= 2u && insns[insns.size() - 2u].branch_likely &&
            insns[insns.size() - 2u].has_delay_slot)
        {
            const std::vector<ee::s_ee_instruction> unconditional(
                insns.begin(), insns.end() - 1);
            block_killed_map[bb.start_pc] = block_written(unconditional);
        }
        else
        {
            block_killed_map[bb.start_pc] = block_written_map[bb.start_pc];
        }
        std::set<uint8_t> rbw = block_read_before_write(insns);
        // The V0 write trace observes the pre-write SSA value even for an
        // instruction which otherwise kills V0 on entry to the block.
        // Keep that old value live across predecessors whenever this block
        // may write V0; this only strengthens dataflow, not guest semantics.
        if (block_written_map[bb.start_pc].count(2u) != 0u)
        {
            rbw.insert(2u);
        }
        // The diagnostic store hook observes SP/FP/RA via SSA at the guest
        // instruction. Account for these extra reads in predecessor liveness,
        // otherwise a predecessor could discard a dirty register before this
        // block even though the trace needs its actual architectural value.
        for (const auto& ins : insns)
        {
            if (ins.valid != ee::e_ee_valid::VALID)
            {
                continue;
            }
            // A precise EX_Ov/EX_Tr handler can inspect every incoming
            // register, including values not read by the trapping opcode.
            const bool signed_immediate =
                ins.opcode == e_ee_opcode::OP_ADDI ||
                ins.opcode == e_ee_opcode::OP_DADDI;
            const bool regimm_trap =
                ins.opcode == e_ee_opcode::OP_REGIMM &&
                (ins.rt == 0x08 || ins.rt == 0x09 ||
                 ins.rt == 0x0a || ins.rt == 0x0b ||
                 ins.rt == 0x0c || ins.rt == 0x0e);
            const bool special_fault =
                ins.opcode == e_ee_opcode::OP_SPECIAL &&
                (ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_ADD) ||
                 ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_SUB) ||
                 ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_DADD) ||
                 ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_DSUB) ||
                 ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_TGE) ||
                 ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_TGEU) ||
                 ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_TLT) ||
                 ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_TLTU) ||
                 ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_TEQ) ||
                 ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_TNE));
            if (signed_immediate || regimm_trap || special_fault)
            {
                for (uint8_t reg = 1; reg < 32; ++reg)
                {
                    rbw.insert(reg);
                }
            }
            switch (ins.opcode)
            {
            case e_ee_opcode::OP_SB:
            case e_ee_opcode::OP_SH:
            case e_ee_opcode::OP_SW:
            case e_ee_opcode::OP_SD:
            case e_ee_opcode::OP_SQ:
            case e_ee_opcode::OP_SWL:
            case e_ee_opcode::OP_SWR:
            case e_ee_opcode::OP_SDL:
            case e_ee_opcode::OP_SDR:
            case e_ee_opcode::OP_SWC1:
            case e_ee_opcode::OP_SQC2:
                rbw.insert(29u);
                rbw.insert(30u);
                rbw.insert(31u);
                break;
            default:
                break;
            }
        }
        block_rbw_map[bb.start_pc] = rbw;
    }

    // Initialize liveness. Every architectural GPR is observable at a
    // dispatcher/return/exception boundary, even when no later translated
    // instruction in this function reads it. Seed those outgoing edges with
    // all GPRs, then let ordinary backwards liveness carry the requirement to
    // earlier blocks. Without this, a final MFHI1/MFLO1 (or any cached scalar
    // write) can be discarded as dead before the host sees CPU state.
    std::map<uint32_t, std::set<uint8_t>> live_in, live_out;
    std::set<uint32_t> block_pcs;
    std::set<uint8_t> all_arch_gprs;
    for (uint8_t r = 1; r < 32; ++r)
        all_arch_gprs.insert(r);
    for (const auto& bb : fn.blocks)
        block_pcs.insert(bb.start_pc);

    // Only already-discovered backwards intra-function edges need a
    // loop guard. We guard their destination headers so delay slots
    // have fully committed before the watchdog can stop execution.
    std::set<uint32_t> loop_headers;
    for (const auto& bb : fn.blocks)
    {
        for (const uint32_t successor : bb.successors)
        {
            if (successor <= bb.start_pc && block_pcs.count(successor) != 0u)
            {
                loop_headers.insert(successor);
            }
        }
    }

    std::set<uint32_t> dispatch_exit_blocks;
    for (size_t i = 0; i < fn.blocks.size(); ++i)
    {
        const auto& bb = fn.blocks[i];
        const auto& insns = block_insns_vec[i];
        const ee::s_ee_instruction* control = nullptr;
        for (auto it = insns.rbegin(); it != insns.rend(); ++it)
        {
            if (it->flow != ee::e_ee_flow::FLOW_NONE)
            {
                control = &*it;
                break;
            }
        }

        bool exits_to_dispatcher = bb.is_terminator;
        if (control != nullptr)
        {
            switch (control->flow)
            {
            case ee::e_ee_flow::FLOW_BRANCH:
            case ee::e_ee_flow::FLOW_BRANCH_LIKELY:
                exits_to_dispatcher = exits_to_dispatcher ||
                    block_pcs.count(control->branch_target) == 0 ||
                    block_pcs.count(bb.fallthrough_pc) == 0;
                break;
            case ee::e_ee_flow::FLOW_CALL:
                // The callee executes outside this translated function.
                exits_to_dispatcher = true;
                break;
            case ee::e_ee_flow::FLOW_JUMP:
                exits_to_dispatcher = exits_to_dispatcher ||
                    control->jump_target == 0 ||
                    block_pcs.count(control->jump_target) == 0;
                break;
            case ee::e_ee_flow::FLOW_RETURN:
            case ee::e_ee_flow::FLOW_EXCEPTION:
                exits_to_dispatcher = true;
                break;
            default:
                break;
            }
        }
        else if (bb.successors.empty() ||
                 (bb.fallthrough_pc != 0 && block_pcs.count(bb.fallthrough_pc) == 0))
        {
            // Straight-line end of the discovered function: the emitted
            // continuation is a host-dispatcher exit.
            exits_to_dispatcher = true;
        }

        if (exits_to_dispatcher)
            dispatch_exit_blocks.insert(bb.start_pc);
    }

    for (const auto& bb : fn.blocks)
    {
        live_in[bb.start_pc] = {};
        live_out[bb.start_pc] = {};
    }

    // Iterate to fixpoint.
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (size_t i = 0; i < fn.blocks.size(); ++i)
        {
            const auto& bb = fn.blocks[i];
            const auto& insns = block_insns_vec[i];

            // live_out[B] = U live_in[succ]
            std::set<uint8_t> new_out;
            for (uint32_t succ : bb.successors)
            {
                auto it = live_in.find(succ);
                if (it != live_in.end())
                    new_out.insert(it->second.begin(), it->second.end());
            }
            if (dispatch_exit_blocks.count(bb.start_pc) != 0)
                new_out.insert(all_arch_gprs.begin(), all_arch_gprs.end());
            if (new_out != live_out[bb.start_pc])
            {
                live_out[bb.start_pc] = new_out;
                changed = true;
            }

            // live_in[B] = (live_out[B] - killed[B]) U read_before_write[B]
            const auto& written = block_killed_map[bb.start_pc];
            const auto& rbw = block_rbw_map[bb.start_pc];
            std::set<uint8_t> new_in;
            for (uint8_t r : live_out[bb.start_pc])
                if (written.count(r) == 0) new_in.insert(r);
            for (uint8_t r : rbw) new_in.insert(r);
            if (loop_headers.count(bb.start_pc) != 0u)
            {
                // A loop-budget exit can occur at this block *before*
                // it executes. Make all GPRs architecturally observable
                // on incoming edges, not merely those used by the loop.
                new_in.insert(all_arch_gprs.begin(), all_arch_gprs.end());
            }
            if (new_in != live_in[bb.start_pc])
            {
                live_in[bb.start_pc] = new_in;
                changed = true;
            }
        }
    }

    begin_function(func_name);

    if (!fn.blocks.empty())
    {
        // Re-enter at the block named by cpu.m_pc when a callee returns to a
        // continuation inside this translated function. Zero remains accepted
        // as a legacy initial-PC sentinel; every other unknown PC must go back
        // to the host dispatcher rather than restarting the function.
        uint32_t entry_block_pc = fn.entry_pc;
        if (block_pcs.count(entry_block_pc) == 0)
            entry_block_pc = fn.blocks[0].start_pc;

        const std::string pc_ptr = state_field(6);
        const std::string guest_pc = newtemp();
        m_out += "  " + guest_pc + " = load i32, ptr " + pc_ptr + "\n";
        m_out += "  switch i32 " + guest_pc + ", label %dispatch_unknown [\n";
        if (fn.entry_pc != 0)
        {
            m_out += "    i32 " + std::to_string(fn.entry_pc) +
                     ", label %" + block_label(entry_block_pc) + "\n";
        }
        m_out += "    i32 0, label %" + block_label(entry_block_pc) + "\n";
        for (const auto& bb : fn.blocks)
        {
            if (bb.start_pc == entry_block_pc || bb.start_pc == 0)
                continue;
            m_out += "    i32 " + std::to_string(bb.start_pc) +
                     ", label %" + block_label(bb.start_pc) + "\n";
        }
        m_out += "  ]\n";
        m_out += "dispatch_unknown:\n";
        const std::string exit_ptr = state_field(7);
        m_out += "  store i32 3, ptr " + exit_ptr + "\n";
        m_out += "  ret i32 3\n";
    }
    else
    {
        m_out += "  br label %" + block_label(0) + "\n";
    }

    std::set<uint32_t> defined_pcs;
    std::set<uint32_t> referenced_pcs;
    for (size_t i = 0; i < fn.blocks.size(); ++i) {
        const auto& bb = fn.blocks[i];
        defined_pcs.insert(bb.start_pc);
        if (bb.fallthrough_pc) referenced_pcs.insert(bb.fallthrough_pc);
        if (bb.target_pc) referenced_pcs.insert(bb.target_pc);
        for (uint32_t s : bb.successors) referenced_pcs.insert(s);
        for (const auto& ins : block_insns_vec[i]) {
            if (ins.branch_target) referenced_pcs.insert(ins.branch_target);
            if (ins.jump_target) referenced_pcs.insert(ins.jump_target);
            if (ins.has_delay_slot) referenced_pcs.insert(ins.pc + 8);
        }
    }
    referenced_pcs.insert(0);

    for (size_t i = 0; i < fn.blocks.size(); ++i) {
        const auto& bb = fn.blocks[i];
        emit_block(block_insns_vec[i], i == fn.blocks.size() - 1, fn.entry_pc,
                   live_in[bb.start_pc], live_out[bb.start_pc],
                   bb.fallthrough_pc, bb.is_terminator,
                   loop_headers.count(bb.start_pc) != 0u);
    }

    // External / unresolved targets return to the host dispatcher. Returning
    // through the zero sentinel loses the guest target PC and makes direct
    // branches/calls into other translated functions look like a normal return.
    for (uint32_t pc : referenced_pcs) {
        if (pc == 0 || defined_pcs.count(pc)) continue;
        m_out += block_label(pc) + ":\n";
        const std::string pc_ptr = state_field(6); // s_ee_cpu_state::m_pc
        m_out += "  store i32 " + std::to_string(pc) + ", ptr " + pc_ptr + "\n";
        const std::string exit_ptr = state_field(7); // m_exit_reason
        m_out += "  store i32 3, ptr " + exit_ptr + "\n";
        m_out += "  ret i32 3\n"; // EE_EXIT_DISPATCH
    }

    m_out += block_label(0);
    m_out += ":\n";
    m_out += "  ret i32 0\n";

    m_out += "\n";
    end_function();

    if (m_out.find("call i32 @ee_cop1_rsqrt_bits(") != std::string::npos ||
        m_out.find("call i32 @ee_cop1_radix_quotient(") != std::string::npos ||
        m_out.find("call i32 @ee_cop1_radix_root(") != std::string::npos)
    {
        m_out += k_ee_cop1_rsqrt_ir;
    }
    if (m_out.find("@ee_cop1_mac_bits(") != std::string::npos)
    {
        m_out += k_ee_cop1_mac_ir;
    }
    if (m_out.find("@ee_cop1_add_bits(") != std::string::npos)
    {
        m_out += k_ee_cop1_add_ir;
    }
    if (m_out.find("@ee_cop1_mul_bits(") != std::string::npos)
    {
        m_out += k_ee_cop1_mul_ir;
    }
    m_ctx.ir(optimize_ir(m_out, m_opt_level));
}

  // ---- IR text optimisation passes (Phase 7) ----

namespace {

bool is_pure_binop_def(const std::string& line, std::string* def_out,
                       std::string* rhs_out)
{
    // Match: "  %tN = <op> <ty> <ops...>"  (no call, load, store, br, ret)
    size_t p = 0;
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
    if (p >= line.size() || line[p] != '%') return false;
    // Only optimizer-generated temporaries (%t followed by digits) are
    // eligible for textual CSE/DCE. Named SSA values such as %trace_enabled,
    // %trace_ptr_field and %trace_ptr are part of the instruction-count
    // helper and must remain defined. The previous prefix-only check
    // mistakenly classified any %trace_* value as a disposable %tN.
    if (p + 2 >= line.size() || line[p + 1] != 't' ||
        !std::isdigit(static_cast<unsigned char>(line[p + 2])))
    {
        return false;
    }
    size_t name_end = p + 3;
    while (name_end < line.size() &&
           std::isdigit(static_cast<unsigned char>(line[name_end])))
    {
        ++name_end;
    }
    if (name_end < line.size() &&
        (std::isalnum(static_cast<unsigned char>(line[name_end])) ||
         line[name_end] == '_'))
    {
        return false;
    }
    size_t eq = line.find('=', name_end);
    if (eq == std::string::npos) return false;
    std::string def = line.substr(p, name_end - p);

    size_t rhs = eq + 1;
    while (rhs < line.size() && (line[rhs] == ' ' || line[rhs] == '\t')) ++rhs;
    // Reject side-effecting / memory ops.
    static const char* ban[] = {
        "call ", "load ", "store ", "br ", "ret ", "phi ", "alloca ",
        "invoke ", "landingpad ", "atomicrmw ", "cmpxchg "
    };
    for (const char* b : ban) {
        if (line.compare(rhs, std::strlen(b), b) == 0) return false;
    }
    if (def_out) *def_out = def;
    if (rhs_out) *rhs_out = line.substr(rhs);
    return true;
}

} // namespace

std::string module_builder::optimize_ir(const std::string& ir, int level)
{
    if (level <= 0) return ir;

    // Split into lines (preserve empties for readability).
    std::vector<std::string> lines;
    {
        std::string::size_type start = 0;
        while (start < ir.size()) {
            auto nl = ir.find('\n', start);
            if (nl == std::string::npos) {
                lines.push_back(ir.substr(start));
                break;
            }
            lines.push_back(ir.substr(start, nl - start));
            start = nl + 1;
        }
    }

    // ---- Level 1: preserve architectural state stores ----
    // Every store emitted by the current EE backend writes architectural CPU
    // or VU0 state.  The old "dead store" pass treated a value used only by a
    // store as dead, but that store is itself an observable side effect.  It
    // also failed to canonicalise distinct GEP temporaries that can alias the
    // same HI/LO or GPR field, so it could remove writes that a later load
    // observes (e.g. MTHI1 followed by MFHI1).
    //
    // Keep all stores until we have memory-effect/alias analysis capable of
    // proving a write redundant.  Level 1 intentionally performs no store
    // elimination; higher levels still optimise pure SSA expressions.

    // ---- Level 2: local common-subexpression elimination ----
    // Within a basic block (between labels / terminators), if two pure binops
    // share an identical RHS, rewrite later uses of the second def to the first.
    if (level >= 2) {
        std::map<std::string, std::string> rewrite; // old %t -> canonical %t
        std::map<std::string, std::string> rhs_to_def; // rhs text -> first def
        std::vector<std::string> cse_out;
        cse_out.reserve(lines.size());

        auto flush_block = [&]() {
            rhs_to_def.clear();
        };

        for (const auto& line : lines) {
            size_t lead = 0;
            while (lead < line.size() &&
                   (line[lead] == ' ' || line[lead] == '\t'))
                ++lead;

            // Label or terminator → end of local CSE scope.
            bool is_label = !line.empty() && line.back() == ':' &&
                            line.find('=') == std::string::npos;
            bool is_term =
                line.compare(lead, 3, "br ") == 0 ||
                line.compare(lead, 4, "ret ") == 0;
            if (is_label || is_term) {
                if (is_term) {
                    // Still emit terminator after applying rewrites.
                } else {
                    flush_block();
                    cse_out.push_back(line);
                    continue;
                }
            }

            std::string def, rhs;
            if (is_pure_binop_def(line, &def, &rhs)) {
                // Apply prior rewrites to RHS so CSE chains.
                std::string rhs2 = rhs;
                for (const auto& kv : rewrite) {
                    size_t p = 0;
                    while ((p = rhs2.find(kv.first, p)) != std::string::npos) {
                        // Ensure token boundary (simple digit/alpha end).
                        size_t end = p + kv.first.size();
                        if (end < rhs2.size() &&
                            (std::isalnum(static_cast<unsigned char>(rhs2[end])) ||
                             rhs2[end] == '_')) {
                            p = end;
                            continue;
                        }
                        rhs2.replace(p, kv.first.size(), kv.second);
                        p += kv.second.size();
                    }
                }
                auto it = rhs_to_def.find(rhs2);
                if (it != rhs_to_def.end()) {
                    rewrite[def] = it->second;
                    continue; // drop duplicate pure def
                }
                rhs_to_def[rhs2] = def;
                // Rebuild line with rewritten RHS if needed.
                if (rhs2 != rhs) {
                    size_t eq = line.find('=');
                    cse_out.push_back(line.substr(0, eq + 1) + " " + rhs2);
                } else {
                    cse_out.push_back(line);
                }
                if (is_term) flush_block();
                continue;
            }

            // Rewrite operands in non-pure lines.
            std::string rewritten = line;
            for (const auto& kv : rewrite) {
                size_t p = 0;
                while ((p = rewritten.find(kv.first, p)) != std::string::npos) {
                    size_t end = p + kv.first.size();
                    if (end < rewritten.size() &&
                        (std::isalnum(static_cast<unsigned char>(rewritten[end])) ||
                         rewritten[end] == '_')) {
                        p = end;
                        continue;
                    }
                    rewritten.replace(p, kv.first.size(), kv.second);
                    p += kv.second.size();
                }
            }
            cse_out.push_back(rewritten);
            if (is_term) flush_block();
        }
        lines.swap(cse_out);
    }

    // ---- Level 3: dead temp elimination ----
    // Drop pure %tN = ... definitions whose result is never used.
    if (level >= 3) {
        // Iterate to fixed point (removing a def can kill more).
        for (int pass = 0; pass < 4; ++pass) {
            std::set<std::string> used;
            for (const auto& line : lines) {
                auto eq = line.find('=');
                size_t scan = (eq != std::string::npos) ? eq + 1 : 0;
                size_t pos = scan;
                while ((pos = line.find("%t", pos)) != std::string::npos) {
                    size_t end = pos + 2;
                    while (end < line.size() &&
                           std::isdigit(static_cast<unsigned char>(line[end])))
                        ++end;
                    if (end > pos + 2)
                        used.insert(line.substr(pos, end - pos));
                    pos = end;
                }
            }
            std::vector<std::string> kept;
            kept.reserve(lines.size());
            bool removed = false;
            for (const auto& line : lines) {
                std::string def, rhs;
                if (is_pure_binop_def(line, &def, &rhs) &&
                    used.find(def) == used.end()) {
                    removed = true;
                    continue;
                }
                kept.push_back(line);
            }
            lines.swap(kept);
            if (!removed) break;
        }
    }

    std::string result;
    result.reserve(ir.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        result += lines[i];
        result += '\n';
    }
    return result;
}

} // namespace llvm
