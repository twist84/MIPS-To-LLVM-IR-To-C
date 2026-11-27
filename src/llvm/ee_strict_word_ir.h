#pragma once

#include "llvm_context.h"
#include "ee_instruction.h"
#include "ee_strict_native_word.h"
#include <cstddef>
#include <sstream>
#include <vector>

namespace llvm
{
namespace strict_transfer_ir
{
inline constexpr uint64_t low_mask(uint32_t bytes)
{
    return bytes == 8u ? ~uint64_t{0} : (uint64_t{1} << (bytes * 8u)) - 1u;
}

static_assert(low_mask(8u) == 0xffffffffffffffffull);
static_assert(low_mask(3u) == 0xffffffull);

// Emit a validated contiguous byte span into one GPR half. Unique
// names per case/half avoid SSA collisions across partial variants.
inline void emit_half(std::ostringstream& out, const std::string& prefix,
    size_t reg_offset, uint32_t rt, bool store, uint32_t memory_offset,
    uint32_t first_lane, uint32_t count, bool partial, bool sign_extend)
{
    if (rt != 0u)
    {
        out << "  %" << prefix << "reg = getelementptr i8, ptr %cpu, i64 " << reg_offset << "\n";
    }
    if (store)
    {
        if (rt != 0u)
        {
            out << "  %" << prefix << "value = load i64, ptr %" << prefix << "reg, align 8\n";
        }
        for (uint32_t i = 0u; i < count; ++i)
        {
            out << "  %" << prefix << "addr" << i << " = getelementptr i8, ptr %bytes, i64 "
                << memory_offset + i << "\n";
            if (rt != 0u)
            {
                out << "  %" << prefix << "shift" << i << " = lshr i64 %" << prefix
                    << "value, " << (first_lane + i) * 8u << "\n"
                    << "  %" << prefix << "byte" << i << " = trunc i64 %" << prefix
                    << "shift" << i << " to i8\n"
                    << "  store i8 %" << prefix << "byte" << i << ", ptr %" << prefix
                    << "addr" << i << ", align 1\n";
            }
            else
            {
                out << "  store i8 0, ptr %" << prefix << "addr" << i << ", align 1\n";
            }
        }
        return;
    }
    // Translation remains mandatory for a load to zero, but isolated
    // RAM has no side-effectful read after successful preparation.
    if (rt == 0u)
    {
        return;
    }
    std::string merged = "0";
    if (partial)
    {
        const uint64_t mask = low_mask(count) << (first_lane * 8u);
        out << "  %" << prefix << "old = load i64, ptr %" << prefix << "reg, align 8\n"
            << "  %" << prefix << "keep = and i64 %" << prefix << "old, " << ~mask << "\n";
        merged = "%" + prefix + "keep";
    }
    for (uint32_t i = 0u; i < count; ++i)
    {
        out << "  %" << prefix << "addr" << i << " = getelementptr i8, ptr %bytes, i64 "
            << memory_offset + i << "\n"
            << "  %" << prefix << "byte" << i << " = load i8, ptr %" << prefix << "addr" << i << ", align 1\n"
            << "  %" << prefix << "wide" << i << " = zext i8 %" << prefix << "byte" << i << " to i64\n"
            << "  %" << prefix << "shift" << i << " = shl i64 %" << prefix << "wide" << i
            << ", " << (first_lane + i) * 8u << "\n"
            << "  %" << prefix << "merge" << i << " = or i64 " << merged << ", %" << prefix << "shift" << i << "\n";
        merged = "%" + prefix + "merge" + std::to_string(i);
    }
    if (sign_extend)
    {
        const uint32_t bits = partial ? 32u : count * 8u;
        out << "  %" << prefix << "narrow = trunc i64 " << merged << " to i" << bits << "\n"
            << "  %" << prefix << "signed = sext i" << bits << " %" << prefix << "narrow to i64\n";
        merged = "%" + prefix + "signed";
    }
    out << "  store i64 " << merged << ", ptr %" << prefix << "reg, align 8\n";
}

inline constexpr bool supported_instruction(const ee::s_ee_instruction& ins)
{
    return ins.valid == ee::e_ee_valid::VALID &&
           ins.flow == ee::e_ee_flow::FLOW_NONE && !ins.has_delay_slot &&
           (ins.pc & 3u) == 0u &&
           ee::ee_decode_strict_transfer(ins.opcode_word).m_width != 0u;
}

inline void declare_prepare(std::ostringstream& out)
{
    out << "declare i32 @ee_strict_native_prepare_transfer(ptr, ptr, i32, i32, ptr, ptr)\n";
}

// Each instruction owns a complete commit boundary. The optional
// internal linkage allows the normal LLVM pipeline to inline the
// transfer into a sequence without weakening the checked helper edge.
inline void emit_transfer_function(std::ostringstream& out,
    const ee::s_ee_instruction& ins, const std::string& name, bool internal,
    bool delay = false)
{
    const auto transfer = ee::ee_decode_strict_transfer(ins.opcode_word);
    const uint32_t rt = (ins.opcode_word >> 16u) & 31u;
    const size_t reg_offset = offsetof(ee::s_ee_cpu_state, m_gpr) +
        rt * sizeof(ee::s_ee_u128) + offsetof(ee::s_ee_u128, m_lo);
    out << "define " << (internal ? "internal " : "") << "i32 @" << name
        << "(ptr %cpu, ptr %context) {\n"
        << "entry:\n"
        << "  %out = alloca ptr\n"
        << "  %lane = alloca i32\n"
        << "  %status = call i32 @" << (delay ? "ee_strict_native_prepare_delay_transfer"
                                               : "ee_strict_native_prepare_transfer")
        << "(ptr %cpu, ptr %context, i32 " << ins.pc << ", i32 " << ins.opcode_word << ", ptr %out, ptr %lane)\n"
        << "  %ready = icmp eq i32 %status, 0\n"
        << "  br i1 %ready, label %transfer, label %rejected\n"
        << "rejected:\n  ret i32 %status\n"
        << "transfer:\n  %bytes = load ptr, ptr %out\n";
    if (transfer.m_partial)
    {
        out << "  %offset = load i32, ptr %lane, align 4\n"
            << "  switch i32 %offset, label %invalid_lane [\n";
        for (uint32_t offset = 0u; offset < transfer.m_width; ++offset)
        {
            out << "    i32 " << offset << ", label %lane" << offset << "\n";
        }
        out << "  ]\ninvalid_lane:\n  ret i32 "
            << static_cast<uint32_t>(ee::e_ee_strict_ram_step_result::UNMODELED_ACCESS) << "\n";
        for (uint32_t offset = 0u; offset < transfer.m_width; ++offset)
        {
            const uint32_t count = transfer.m_left ? offset + 1u : transfer.m_width - offset;
            const uint32_t first = transfer.m_left ? transfer.m_width - count : 0u;
            const bool sign = transfer.m_width == 4u && (transfer.m_left || offset == 0u);
            out << "lane" << offset << ":\n";
            strict_transfer_ir::emit_half(out, "l" + std::to_string(offset) + "_",
                reg_offset, rt, transfer.m_store, 0u, first, count, true, sign);
            out << "  br label %committed\n";
        }
    }
    else
    {
        const uint32_t count = transfer.m_width == 16u ? 8u : transfer.m_width;
        strict_transfer_ir::emit_half(out, "lo_", reg_offset, rt, transfer.m_store,
            0u, 0u, count, false, transfer.m_signed_load);
        if (transfer.m_width == 16u)
        {
            strict_transfer_ir::emit_half(out, "hi_", reg_offset +
                offsetof(ee::s_ee_u128, m_hi) - offsetof(ee::s_ee_u128, m_lo),
                rt, transfer.m_store, 8u, 0u, 8u, false, false);
        }
        out << "  br label %committed\n";
    }
    out << "committed:\n";
    if (!delay)
    {
        out << "  %pc = getelementptr i8, ptr %cpu, i64 " << offsetof(ee::s_ee_cpu_state, m_pc) << "\n"
            << "  store i32 " << static_cast<uint32_t>(ins.pc + 4u) << ", ptr %pc, align 4\n";
    }
    out << "  ret i32 0\n}\n";
}
} // namespace strict_transfer_ir

// Separate single-instruction lowering, never selected by retail.
inline bool emit_strict_word_entry(llvm_context& ctx, const ee::s_ee_instruction& ins)
{
    if (!strict_transfer_ir::supported_instruction(ins))
    {
        return false;
    }
    std::ostringstream out;
    strict_transfer_ir::declare_prepare(out);
    strict_transfer_ir::emit_transfer_function(out, ins, "guest_strict_word", false);
    ctx.ir(out.str());
    return true;
}

// Bounded straight-line CFG. No branches, delay slots, or speculative
// fetch. The caller retains isolated-boundary evidence throughout.
// Each successful instruction publishes PC/GPR/RAM before the next
// prepare call. Any nonzero result exits at that instruction, keeping
// the completed prefix intact. A live opcode check at every boundary
// also stops self-modifying sequences before stale native execution.
inline bool emit_strict_memory_sequence(llvm_context& ctx,
    const std::vector<ee::s_ee_instruction>& instructions)
{
    constexpr size_t max_instructions = 256u;
    if (instructions.empty() || instructions.size() > max_instructions)
    {
        return false;
    }
    const uint64_t first = instructions.front().pc;
    for (size_t i = 0u; i < instructions.size(); ++i)
    {
        if (!strict_transfer_ir::supported_instruction(instructions[i]) ||
            static_cast<uint64_t>(instructions[i].pc) != first + i * 4u ||
            instructions[i].pc == 0xfffffffcu)
        {
            return false;
        }
    }

    std::ostringstream out;
    strict_transfer_ir::declare_prepare(out);
    for (size_t i = 0u; i < instructions.size(); ++i)
    {
        strict_transfer_ir::emit_transfer_function(out, instructions[i],
            "strict_step_" + std::to_string(i), true);
    }
    out << "define i32 @guest_strict_word(ptr %cpu, ptr %context) {\n"
        << "entry:\n  br label %step0\n";
    for (size_t i = 0u; i < instructions.size(); ++i)
    {
        out << "step" << i << ":\n"
            << "  %status" << i << " = call i32 @strict_step_" << i
            << "(ptr %cpu, ptr %context)\n"
            << "  %ok" << i << " = icmp eq i32 %status" << i << ", 0\n"
            << "  br i1 %ok" << i << ", label %"
            << (i + 1u == instructions.size() ? "complete" : "step" + std::to_string(i + 1u))
            << ", label %stop" << i << "\n"
            << "stop" << i << ":\n  ret i32 %status" << i << "\n";
    }
    out << "complete:\n  ret i32 0\n}\n";
    ctx.ir(out.str());
    return true;
}

// A bounded branch terminator and exactly one integer memory delay
// instruction. No branch-in-slot or speculative target fetch.
// Both branch operands are sampled before the delay instruction. An
// ordinary not-taken branch still executes its slot; a not-taken likely
// branch skips the slot helper entirely, including its fetch checks.
inline bool emit_strict_memory_branch(llvm_context& ctx,
    const ee::s_ee_instruction& branch, const ee::s_ee_instruction& slot)
{
    if (branch.valid != ee::e_ee_valid::VALID || !branch.has_delay_slot ||
        (branch.pc & 3u) != 0u || branch.pc > 0xfffffff4u ||
        !ee::ee_strict_branch_supported(branch.opcode_word) ||
        !strict_transfer_ir::supported_instruction(slot) || slot.pc != branch.pc + 4u)
    {
        return false;
    }
    const uint32_t primary = branch.opcode_word >> 26u;
    const bool indirect = primary == 0u;
    const bool jump = indirect || primary == 2u || primary == 3u;
    const bool likely = !jump && (primary & 0x10u) != 0u;
    const bool link = primary == 3u || (indirect && (branch.opcode_word & 63u) == 9u);
    const uint32_t link_reg = !link ? 0u : indirect ? (branch.opcode_word >> 11u) & 31u : 31u;
    if ((!jump && branch.flow != (likely ? ee::e_ee_flow::FLOW_BRANCH_LIKELY : ee::e_ee_flow::FLOW_BRANCH)) ||
        (jump && branch.flow != ee::e_ee_flow::FLOW_JUMP &&
         branch.flow != ee::e_ee_flow::FLOW_CALL && branch.flow != ee::e_ee_flow::FLOW_RETURN))
    {
        return false;
    }
    const uint32_t displacement = static_cast<uint32_t>(
        static_cast<int32_t>(static_cast<int16_t>(branch.opcode_word & 0xffffu))) << 2u;
    const uint32_t target = branch.pc + 4u + displacement;
    const uint32_t fallthrough = branch.pc + 8u;
    std::ostringstream out;
    out << "declare i32 @ee_strict_native_check_branch(ptr, ptr, i32, i32)\n"
        << "declare i32 @ee_strict_native_prepare_delay_transfer(ptr, ptr, i32, i32, ptr, ptr)\n";
    strict_transfer_ir::emit_transfer_function(out, slot, "strict_delay", true, true);
    out << "define i32 @guest_strict_word(ptr %cpu, ptr %context) {\n"
        << "entry:\n"
        << "  %branch_status = call i32 @ee_strict_native_check_branch(ptr %cpu, ptr %context, i32 "
        << branch.pc << ", i32 " << branch.opcode_word << ")\n"
        << "  %branch_ready = icmp eq i32 %branch_status, 0\n"
        << "  br i1 %branch_ready, label %condition, label %branch_stop\n"
        << "branch_stop:\n  ret i32 %branch_status\ncondition:\n";
    const auto operand = [&](uint32_t reg, const std::string& name) -> std::string
    {
        if (reg == 0u)
        {
            return "0";
        }
        const size_t offset = offsetof(ee::s_ee_cpu_state, m_gpr) +
            reg * sizeof(ee::s_ee_u128) + offsetof(ee::s_ee_u128, m_lo);
        out << "  %" << name << "_ptr = getelementptr i8, ptr %cpu, i64 " << offset << "\n"
            << "  %" << name << " = load i64, ptr %" << name << "_ptr, align 8\n";
        return "%" + name;
    };
    std::string next = "%next";
    if (indirect)
    {
        const auto source = operand((branch.opcode_word >> 21u) & 31u, "jump_source");
        out << "  %next = trunc i64 " << source << " to i32\n";
    }
    else if (jump)
    {
        next = std::to_string(((branch.pc + 4u) & 0xf0000000u) |
            ((branch.opcode_word & 0x03ffffffu) << 2u));
    }
    else
    {
        const auto left = operand((branch.opcode_word >> 21u) & 31u, "lhs");
        const auto right = operand((branch.opcode_word >> 16u) & 31u, "rhs");
        out << "  %taken = icmp " << ((primary & 1u) == 0u ? "eq" : "ne")
            << " i64 " << left << ", " << right << "\n"
            << "  %next = select i1 %taken, i32 " << target << ", i32 " << fallthrough << "\n";
    }
    if (link_reg != 0u)
    {
        const size_t offset = offsetof(ee::s_ee_cpu_state, m_gpr) +
            link_reg * sizeof(ee::s_ee_u128) + offsetof(ee::s_ee_u128, m_lo);
        out << "  %link_ptr = getelementptr i8, ptr %cpu, i64 " << offset << "\n"
            << "  %old_link = load i64, ptr %link_ptr, align 8\n"
            << "  store i64 " << fallthrough << ", ptr %link_ptr, align 8\n";
    }
    if (likely)
    {
        out << "  br i1 %taken, label %delay, label %complete\n";
    }
    else
    {
        out << "  br label %delay\n";
    }
    out << "delay:\n  %slot_status = call i32 @strict_delay(ptr %cpu, ptr %context)\n"
        << "  %slot_ready = icmp eq i32 %slot_status, 0\n"
        << "  br i1 %slot_ready, label %complete, label %slot_stop\n"
        << "slot_stop:\n";
    if (link_reg != 0u)
    {
        // Guest faults preserve the already completed link. Host-only
        // refusal leaves the entire branch restart state untouched.
        out << "  %guest_fault = icmp eq i32 %slot_status, "
            << static_cast<uint32_t>(ee::e_ee_strict_ram_step_result::FAULT_COMMITTED) << "\n"
            << "  br i1 %guest_fault, label %slot_exit, label %restore_link\n"
            << "restore_link:\n  store i64 %old_link, ptr %link_ptr, align 8\n"
            << "  br label %slot_exit\nslot_exit:\n";
    }
    out << "  ret i32 %slot_status\n"
        << "complete:\n  %pc = getelementptr i8, ptr %cpu, i64 "
        << offsetof(ee::s_ee_cpu_state, m_pc) << "\n"
        << "  store i32 " << next << ", ptr %pc, align 4\n  ret i32 0\n}\n";
    ctx.ir(out.str());
    return true;
}

// Combine a contiguous memory prefix and a branch pair. Reuse the
// existing checked step and branch emitters rather than duplicate
// either instruction semantics. No arbitrary CFG joins or loops.
inline bool emit_strict_memory_terminated_block(llvm_context& ctx,
    const std::vector<ee::s_ee_instruction>& instructions)
{
    if (instructions.size() < 2u || instructions.size() > 256u)
    {
        return false;
    }
    const size_t prefix_count = instructions.size() - 2u;
    const uint64_t first = instructions.front().pc;
    for (size_t i = 0u; i < instructions.size(); ++i)
    {
        if (static_cast<uint64_t>(instructions[i].pc) != first + i * 4u ||
            (i < prefix_count && !strict_transfer_ir::supported_instruction(instructions[i])))
        {
            return false;
        }
    }
    llvm_context terminator("strict_terminator");
    if (!emit_strict_memory_branch(terminator, instructions[prefix_count], instructions.back()))
    {
        return false;
    }
    if (prefix_count == 0u)
    {
        ctx.ir(terminator.ir());
        return true;
    }
    // The branch module owns the checked branch declarations. Add the
    // ordinary transfer declaration and internalize its single entry.
    std::string branch_ir = terminator.ir();
    const std::string public_entry = "define i32 @guest_strict_word(";
    const auto position = branch_ir.find(public_entry);
    if (position == std::string::npos)
    {
        return false;
    }
    branch_ir.replace(position, public_entry.size(), "define internal i32 @strict_terminator(");
    std::ostringstream out;
    strict_transfer_ir::declare_prepare(out);
    out << branch_ir;
    for (size_t i = 0u; i < prefix_count; ++i)
    {
        strict_transfer_ir::emit_transfer_function(out, instructions[i],
            "strict_prefix_" + std::to_string(i), true);
    }
    out << "define i32 @guest_strict_word(ptr %cpu, ptr %context) {\n"
        << "entry:\n  br label %prefix0\n";
    for (size_t i = 0u; i < prefix_count; ++i)
    {
        out << "prefix" << i << ":\n"
            << "  %status" << i << " = call i32 @strict_prefix_" << i
            << "(ptr %cpu, ptr %context)\n"
            << "  %ok" << i << " = icmp eq i32 %status" << i << ", 0\n"
            << "  br i1 %ok" << i << ", label %"
            << (i + 1u == prefix_count ? "terminator" : "prefix" + std::to_string(i + 1u))
            << ", label %stop" << i << "\n"
            << "stop" << i << ":\n  ret i32 %status" << i << "\n";
    }
    out << "terminator:\n  %last = call i32 @strict_terminator(ptr %cpu, ptr %context)\n"
        << "  ret i32 %last\n}\n";
    ctx.ir(out.str());
    return true;
}

} // namespace llvm
