#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

namespace
{
uint32_t make_cop0(uint8_t fmt, uint8_t gpr, uint8_t reg)
{
    return (0x10u << 26u) |
           (static_cast<uint32_t>(fmt) << 21u) |
           (static_cast<uint32_t>(gpr) << 16u) |
           (static_cast<uint32_t>(reg) << 11u);
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: ee_cop0_write_ir_fixture output.ll\n");
        return 2;
    }

    constexpr uint32_t k_entry_pc = 0x00134000u;
    const std::vector<uint32_t> words =
    {
        make_cop0(4u, 8u, 11u),  // mtc0 t0, Compare
        make_cop0(0u, 9u, 13u),  // mfc0 t1, Cause
        make_cop0(0u, 10u, 11u), // mfc0 t2, Compare
        make_cop0(4u, 12u, 12u), // mtc0 t4, Status
        make_cop0(0u, 13u, 12u), // mfc0 t5, Status
        make_cop0(4u, 14u, 9u),  // mtc0 t6, Count
        make_cop0(0u, 15u, 9u),  // mfc0 t7, Count
        make_cop0(4u, 16u, 13u), // mtc0 s0, Cause (read-only)
        make_cop0(0u, 17u, 13u), // mfc0 s1, Cause
    };

    std::map<uint32_t, ee::s_ee_instruction> instructions;
    for (size_t i = 0; i < words.size(); ++i)
    {
        const uint32_t pc = k_entry_pc + static_cast<uint32_t>(i * 4u);
        const ee::s_ee_instruction decoded = ee::decode(pc, words[i]);
        if (decoded.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr, "COP0 instruction at 0x%08x invalid: %s\n",
                         pc, decoded.mnemonic);
            return 3;
        }
        instructions.emplace(pc, decoded);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(instructions, {k_entry_pc});
    if (functions.size() != 1 || functions[0].blocks.size() != 1 ||
        functions[0].blocks[0].insn_indices.size() != words.size())
    {
        std::fprintf(stderr, "COP0 fixture CFG analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("ee_cop0_write_test");
    llvm::module_builder builder(context);
    builder.emit_function_cfg(functions[0], instructions, "guest_cop0_write_test");
    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "could not write COP0 LLVM fixture\n");
        return 5;
    }

    return 0;
}
