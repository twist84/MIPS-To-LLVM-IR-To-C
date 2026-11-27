#pragma once

#include "llvm_context.h"
#include "ee_instruction.h"
#include "ee_strict_native_word.h"
#include <cstddef>
#include <sstream>

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
} // namespace strict_transfer_ir

// Separate single-instruction lowering. Never selected by the retail
// builder. All transfer blocks follow successful live preparation.
inline bool emit_strict_word_entry(llvm_context& ctx, const ee::s_ee_instruction& ins)
{
    const auto transfer = ee::ee_decode_strict_transfer(ins.opcode_word);
    if (ins.valid != ee::e_ee_valid::VALID ||
        ins.flow != ee::e_ee_flow::FLOW_NONE || ins.has_delay_slot ||
        (ins.pc & 3u) != 0u || transfer.m_width == 0u)
    {
        return false;
    }
    const uint32_t rt = (ins.opcode_word >> 16u) & 31u;
    const size_t reg_offset = offsetof(ee::s_ee_cpu_state, m_gpr) +
        rt * sizeof(ee::s_ee_u128) + offsetof(ee::s_ee_u128, m_lo);
    std::ostringstream out;
    out << "declare i32 @ee_strict_native_prepare_transfer(ptr, ptr, i32, i32, ptr, ptr)\n"
        << "define i32 @guest_strict_word(ptr %cpu, ptr %context) {\n"
        << "entry:\n"
        << "  %out = alloca ptr\n"
        << "  %lane = alloca i32\n"
        << "  %status = call i32 @ee_strict_native_prepare_transfer(ptr %cpu, ptr %context, i32 "
        << ins.pc << ", i32 " << ins.opcode_word << ", ptr %out, ptr %lane)\n"
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
    out << "committed:\n  %pc = getelementptr i8, ptr %cpu, i64 " << offsetof(ee::s_ee_cpu_state, m_pc) << "\n"
        << "  store i32 " << static_cast<uint32_t>(ins.pc + 4u) << ", ptr %pc, align 4\n"
        << "  ret i32 0\n}\n";
    ctx.ir(out.str());
    return true;
}

} // namespace llvm
