#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace
{
uint32_t make_special(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct)
{
    return (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) |
           (static_cast<uint32_t>(sa) << 6) |
           funct;
}

uint32_t make_mmi(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct)
{
    return (0x1cu << 26) | make_special(rs, rt, rd, sa, funct);
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: ee_div1_ir_fixture output.ll opt_level case\n");
        return 2;
    }

    const int opt_level = std::atoi(argv[2]);
    const int test_case = std::atoi(argv[3]);
    if (opt_level < 0 || opt_level > 3 || test_case < 0 || test_case > 17)
    {
        std::fprintf(stderr, "opt_level must be 0..3 and case must be 0..17\n");
        return 2;
    }

    // Nine cases per independent HI/LO bank, with identical operands.
    const bool primary = test_case >= 9;
    const int variant = test_case % 9;
    const uint8_t funct = variant == 3 || variant == 4 || variant == 6
        ? 0x1b : 0x1a;
    const std::vector<uint32_t> words =
    {
        primary ? make_special(4, 5, 0, 0, funct)
                : make_mmi(4, 5, 0, 0, funct),    // DIV/DIVU or DIV1/DIVU1
        primary ? make_special(0, 0, 6, 0, 0x10)
                : make_mmi(0, 0, 6, 0, 0x10),     // MFHI $a2 / MFHI1
        primary ? make_special(0, 0, 7, 0, 0x12)
                : make_mmi(0, 0, 7, 0, 0x12)      // MFLO $a3 / MFLO1
    };

    constexpr uint32_t entry_pc = 0x00110000u;
    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    for (size_t i = 0; i < words.size(); ++i)
    {
        const uint32_t pc = entry_pc + static_cast<uint32_t>(i * 4);
        ee::s_ee_instruction ins = ee::decode(pc, words[i]);
        if (ins.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr, "DIV fixture instruction at 0x%08x is not VALID: %s\n",
                         pc, ins.mnemonic);
            return 3;
        }
        insn_map.emplace(pc, ins);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {entry_pc});
    if (functions.size() != 1 || functions[0].blocks.empty())
    {
        std::fprintf(stderr, "DIV fixture function analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("ee_div1_runtime_test");
    llvm::module_builder builder(context, opt_level);
    builder.emit_function_cfg(functions[0], insn_map, "guest_div1_test");
    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[1]);
        return 5;
    }

    return 0;
}
