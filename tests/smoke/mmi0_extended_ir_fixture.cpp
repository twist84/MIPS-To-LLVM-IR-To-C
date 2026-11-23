#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t k_entry_pc = 0x00160000u;

uint32_t make_mmi(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct)
{
    return (0x1cu << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) |
           (static_cast<uint32_t>(sa) << 6) | funct;
}

bool parse_mode(std::string mode, uint32_t& word)
{
    const std::string alias_rs = "_alias_rs";
    const std::string alias_rt = "_alias_rt";
    const std::string to_zero = "_zero";
    uint8_t rd = 6u;

    if (mode.size() > alias_rs.size() &&
        mode.substr(mode.size() - alias_rs.size()) == alias_rs)
    {
        rd = 4u;
        mode.resize(mode.size() - alias_rs.size());
    }
    else if (mode.size() > alias_rt.size() &&
             mode.substr(mode.size() - alias_rt.size()) == alias_rt)
    {
        rd = 5u;
        mode.resize(mode.size() - alias_rt.size());
    }
    else if (mode.size() > to_zero.size() &&
             mode.substr(mode.size() - to_zero.size()) == to_zero)
    {
        rd = 0u;
        mode.resize(mode.size() - to_zero.size());
    }

    uint8_t shift = 0u;
    const size_t shift_index = mode.rfind("_s");
    if (shift_index != std::string::npos)
    {
        const std::string shift_text = mode.substr(shift_index + 2u);
        if (shift_text.empty())
        {
            return false;
        }
        for (const char c : shift_text)
        {
            if (c < '0' || c > '9')
            {
                return false;
            }
        }
        const int amount = std::atoi(shift_text.c_str());
        if (amount < 0 || amount > 31)
        {
            return false;
        }
        shift = static_cast<uint8_t>(amount);
        mode.resize(shift_index);
    }

    const std::map<std::string, uint8_t> shifts =
    {
        {"psllh", 0x34u}, {"psrlh", 0x36u}, {"psrah", 0x37u},
        {"psllw", 0x3cu}, {"psrlw", 0x3eu}, {"psraw", 0x3fu}
    };
    const auto packed_shift = shifts.find(mode);
    if (packed_shift != shifts.end())
    {
        word = make_mmi(0, 5, rd, shift, packed_shift->second);
        return true;
    }
    if (mode == "plzcw")
    {
        word = make_mmi(4, 0, rd, 0, 0x04u);
        return true;
    }
    if (shift_index != std::string::npos)
    {
        return false;
    }

    const std::map<std::string, uint8_t> subops =
    {
        {"pcgtw", 0x02u}, {"pcgth", 0x06u}, {"pcgtb", 0x0au},
        {"paddsw", 0x10u}, {"psubsw", 0x11u},
        {"pextlw", 0x12u}, {"ppacw", 0x13u},
        {"paddsh", 0x14u}, {"psubsh", 0x15u},
        {"pextlh", 0x16u}, {"ppach", 0x17u},
        {"paddsb", 0x18u}, {"psubsb", 0x19u},
        {"pextlb", 0x1au}, {"ppacb", 0x1bu},
        {"pext5", 0x1eu}, {"ppac5", 0x1fu}
    };
    const auto operation = subops.find(mode);
    if (operation == subops.end())
    {
        return false;
    }
    const uint8_t rs = mode == "pext5" || mode == "ppac5" ? 0u : 4u;
    word = make_mmi(rs, 5u, rd, operation->second, 0x08u);
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: ee_mmi0_extended_ir_fixture output.ll mode opt_level\n");
        return 2;
    }
    const int opt_level = std::atoi(argv[3]);
    if (opt_level < 0 || opt_level > 3)
    {
        std::fprintf(stderr, "opt_level must be 0..3\n");
        return 2;
    }

    uint32_t word = 0u;
    if (!parse_mode(argv[2], word))
    {
        std::fprintf(stderr, "unknown extended MMI mode %s\n", argv[2]);
        return 2;
    }

    // A preceding dirty GPR write tests retirement across MMI operations.
    const std::vector<uint32_t> words =
    {
        (0x09u << 26) | (10u << 16) | 0x1234u, // ADDIU $t2, zero, 0x1234
        word,
        (31u << 21) | 0x08u,                   // JR $ra
        0u
    };

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    for (size_t index = 0; index < words.size(); ++index)
    {
        const uint32_t pc = k_entry_pc + static_cast<uint32_t>(index * 4u);
        const auto instruction = ee::decode(pc, words[index]);
        if (instruction.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr, "extended MMI fixture rejected %08x at %08x (%s)\n",
                         words[index], pc, instruction.mnemonic);
            return 3;
        }
        insn_map.emplace(pc, instruction);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {k_entry_pc});
    if (functions.size() != 1 || functions[0].blocks.empty())
    {
        std::fprintf(stderr, "extended MMI fixture function analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("mmi0_extended_native_test");
    llvm::module_builder builder(context, opt_level);
    builder.emit_function_cfg(functions[0], insn_map,
                              "guest_mmi0_extended_test");
    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "extended MMI fixture cannot write IR\n");
        return 5;
    }
    return 0;
}
