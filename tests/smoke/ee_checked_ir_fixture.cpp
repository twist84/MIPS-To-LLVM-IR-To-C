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
constexpr uint32_t k_entry_pc = 0x00150000u;

uint32_t make_special(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t funct)
{
    return (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) | funct;
}

uint32_t make_immediate(uint8_t opcode, uint8_t rs, uint8_t rt, int16_t imm)
{
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           static_cast<uint16_t>(imm);
}

bool instruction_for(const std::string& mode, uint32_t& word)
{
    const uint8_t rs = 4u;
    const uint8_t rt = 5u;
    const uint8_t rd = 6u;
    const std::map<std::string, uint8_t> special =
    {
        {"add", 0x20}, {"sub", 0x22}, {"dadd", 0x2c}, {"dsub", 0x2e},
        {"tge", 0x30}, {"tgeu", 0x31}, {"tlt", 0x32}, {"tltu", 0x33},
        {"teq", 0x34}, {"tne", 0x36}
    };
    const auto found = special.find(mode);
    if (found != special.end())
    {
        word = make_special(rs, rt, rd, found->second);
        return true;
    }
    if (mode == "addi" || mode == "daddi")
    {
        word = make_immediate(mode == "addi" ? 0x08u : 0x18u, rs, rd, 1);
        return true;
    }
    const std::map<std::string, uint8_t> immediate_traps =
    {
        {"tgei", 0x08}, {"tgeiu", 0x09}, {"tlti", 0x0a},
        {"tltiu", 0x0b}, {"teqi", 0x0c}, {"tnei", 0x0e}
    };
    const auto trap = immediate_traps.find(mode);
    if (trap != immediate_traps.end())
    {
        // The signed 16-bit immediate (-1) is sign-extended even for
        // unsigned comparisons (TGEIU/TLTIU).
        word = make_immediate(0x01, rs, trap->second, -1);
        return true;
    }
    return false;
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: ee_checked_ir_fixture output.ll mode[_delay] opt_level\n");
        return 2;
    }

    const int opt_level = std::atoi(argv[3]);
    if (opt_level < 0 || opt_level > 3)
    {
        std::fprintf(stderr, "opt_level must be 0..3\n");
        return 2;
    }
    const std::string full_mode = argv[2];
    const std::string suffix = "_delay";
    const bool delayed = full_mode.size() > suffix.size() &&
        full_mode.substr(full_mode.size() - suffix.size()) == suffix;
    const std::string mode = delayed ?
        full_mode.substr(0, full_mode.size() - suffix.size()) : full_mode;

    uint32_t checked_word = 0;
    if (!instruction_for(mode, checked_word))
    {
        std::fprintf(stderr, "unknown checked instruction: %s\n", argv[2]);
        return 2;
    }

    // Previous ADDIU must be committed even when the following instruction
    // faults. Delayed variants must report the branch PC in EPC and the
    // actual arithmetic/trap PC in the diagnostic m_pad.
    const uint32_t prior = make_immediate(0x09u, 0, 10, 0x1357);
    const uint32_t jr_ra = make_special(31, 0, 0, 0x08);
    const std::vector<uint32_t> words = delayed
        ? std::vector<uint32_t>
          {
              prior, make_immediate(0x04u, 0, 0, 1), checked_word,
              jr_ra, 0
          }
        : std::vector<uint32_t>{prior, checked_word, jr_ra, 0};

    std::map<uint32_t, ee::s_ee_instruction> instructions;
    for (size_t i = 0; i < words.size(); ++i)
    {
        const uint32_t pc = k_entry_pc + static_cast<uint32_t>(4u * i);
        const ee::s_ee_instruction ins = ee::decode(pc, words[i]);
        if (ins.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr,
                         "checked fixture instruction 0x%08x rejected (%s)\n",
                         pc, ins.mnemonic);
            return 3;
        }
        instructions.emplace(pc, ins);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(instructions, {k_entry_pc});
    if (functions.size() != 1 || functions[0].blocks.empty())
    {
        std::fprintf(stderr, "checked fixture CFG analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("ee_checked_exception_test");
    llvm::module_builder builder(context, opt_level);
    builder.emit_function_cfg(functions[0], instructions,
                              "guest_checked_exception_test");
    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write checked LLVM IR\n");
        return 5;
    }
    return 0;
}
