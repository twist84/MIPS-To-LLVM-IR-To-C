#pragma once

#include "llvm_context.h"
#include "ee_instruction.h"
#include "ee_strict_native_word.h"
#include <cstddef>
#include <sstream>

namespace llvm
{
// Separate single-instruction lowering, never selected by module_builder
// or the retail recompiler. The context ABI is deliberately distinct.
// Use host-layout offsets for CPU fields and little-endian byte operations
// for the guest word. No guest data load/store occurs on the fault edge.
inline bool emit_strict_word_entry(llvm_context& ctx, const ee::s_ee_instruction& ins)
{
    if (ins.valid != ee::e_ee_valid::VALID ||
        ins.flow != ee::e_ee_flow::FLOW_NONE || ins.has_delay_slot ||
        (ins.pc & 3u) != 0u || !ee::ee_strict_word_supported(ins.opcode_word))
    {
        return false;
    }
    const bool store = (ins.opcode_word >> 26u) == 0x2bu;
    const uint32_t rt = (ins.opcode_word >> 16u) & 31u;
    const size_t reg_offset = offsetof(ee::s_ee_cpu_state, m_gpr) +
        rt * sizeof(ee::s_ee_u128) + offsetof(ee::s_ee_u128, m_lo);
    std::ostringstream out;
    out << "declare i32 @ee_strict_native_prepare_word(ptr, ptr, i32, i32, ptr)\n"
        << "define i32 @guest_strict_word(ptr %cpu, ptr %context) {\n"
        << "entry:\n"
        << "  %out = alloca ptr\n"
        << "  %status = call i32 @ee_strict_native_prepare_word(ptr %cpu, ptr %context, i32 "
        << ins.pc << ", i32 " << ins.opcode_word << ", ptr %out)\n"
        << "  %ready = icmp eq i32 %status, 0\n"
        << "  br i1 %ready, label %transfer, label %rejected\n"
        << "rejected:\n  ret i32 %status\n"
        << "transfer:\n  %bytes = load ptr, ptr %out\n";
    if (rt != 0u)
    {
        out << "  %reg = getelementptr i8, ptr %cpu, i64 " << reg_offset << "\n";
    }
    if (store)
    {
        if (rt != 0u)
        {
            out << "  %value = load i64, ptr %reg, align 8\n";
        }
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            out << "  %addr" << i << " = getelementptr i8, ptr %bytes, i64 " << i << "\n";
            if (rt != 0u)
            {
                out << "  %shift" << i << " = lshr i64 %value, " << i * 8u << "\n"
                    << "  %byte" << i << " = trunc i64 %shift" << i << " to i8\n"
                    << "  store i8 %byte" << i << ", ptr %addr" << i << ", align 1\n";
            }
            else
            {
                out << "  store i8 0, ptr %addr" << i << ", align 1\n";
            }
        }
    }
    else
    {
        // For rt=0 the validated RAM read has no guest-visible effect.
        // Translation and fault checks still run, even after optimization.
        std::string merged = "0";
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            out << "  %addr" << i << " = getelementptr i8, ptr %bytes, i64 " << i << "\n"
                << "  %byte" << i << " = load i8, ptr %addr" << i << ", align 1\n"
                << "  %wide" << i << " = zext i8 %byte" << i << " to i32\n"
                << "  %shift" << i << " = shl i32 %wide" << i << ", " << i * 8u << "\n"
                << "  %merge" << i << " = or i32 " << merged << ", %shift" << i << "\n";
            merged = "%merge" + std::to_string(i);
        }
        if (rt != 0u)
        {
            out << "  %value = sext i32 " << merged << " to i64\n"
                << "  store i64 %value, ptr %reg, align 8\n";
        }
    }
    out << "  %pc = getelementptr i8, ptr %cpu, i64 " << offsetof(ee::s_ee_cpu_state, m_pc) << "\n"
        << "  store i32 " << static_cast<uint32_t>(ins.pc + 4u) << ", ptr %pc, align 4\n"
        << "  ret i32 0\n}\n";
    ctx.ir(out.str());
    return true;
}

} // namespace llvm
