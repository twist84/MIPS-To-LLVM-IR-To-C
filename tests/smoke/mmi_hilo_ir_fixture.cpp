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
constexpr uint32_t k_entry_pc = 0x00170000u;

uint32_t make_mmi(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa,
                  uint8_t funct)
{
    return (0x1cu << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) |
           (static_cast<uint32_t>(sa) << 6) | funct;
}

uint32_t make_i(uint8_t op, uint8_t rs, uint8_t rt, int16_t immediate)
{
    return (static_cast<uint32_t>(op) << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           static_cast<uint16_t>(immediate);
}

bool mode_from_name(const std::string& name, uint8_t& mode)
{
    static const char* const names[] = {"lw", "uw", "slw", "lh", "sh"};
    for (uint8_t index = 0; index < 5; ++index)
    {
        if (name == names[index])
        {
            mode = index;
            return true;
        }
    }
    return false;
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr,
                     "usage: ee_mmi_hilo_ir_fixture output.ll mode opt_level\n");
        return 2;
    }

    const int opt_level = std::atoi(argv[3]);
    if (opt_level < 0 || opt_level > 3)
    {
        std::fprintf(stderr, "opt_level must be 0..3\n");
        return 2;
    }

    const std::string mode_name = argv[2];
    const std::string pmfhl_prefix = "pmfhl_";
    const std::string chain_prefix = "pmthl_then_pmfhl_";
    const std::string zero_suffix = "_zero";
    bool pmthl = false;
    bool pmfhl = false;
    bool cross_block = false;
    bool prior_gpr_write = false;
    bool use_zero_source = false;
    uint8_t pmfhl_mode = 0;
    uint8_t destination = 6;

    if (mode_name.rfind(chain_prefix, 0) == 0)
    {
        pmthl = true;
        pmfhl = true;
        if (!mode_from_name(mode_name.substr(chain_prefix.size()), pmfhl_mode))
        {
            return 2;
        }
    }
    else if (mode_name.rfind(pmfhl_prefix, 0) == 0)
    {
        pmfhl = true;
        std::string suffix = mode_name.substr(pmfhl_prefix.size());
        if (suffix.size() > zero_suffix.size() &&
            suffix.substr(suffix.size() - zero_suffix.size()) == zero_suffix)
        {
            destination = 0;
            suffix.resize(suffix.size() - zero_suffix.size());
        }
        if (!mode_from_name(suffix, pmfhl_mode))
        {
            return 2;
        }
    }
    else if (mode_name == "pmthl" || mode_name == "pmthl_zero" ||
             mode_name == "pmthl_from_prior_gpr" ||
             mode_name == "pmthl_from_prior_gpr_cross")
    {
        pmthl = true;
        use_zero_source = mode_name == "pmthl_zero";
        prior_gpr_write = mode_name == "pmthl_from_prior_gpr" ||
                          mode_name == "pmthl_from_prior_gpr_cross";
        cross_block = mode_name == "pmthl_from_prior_gpr_cross";
    }
    else
    {
        std::fprintf(stderr, "unknown PMFHL/PMTHL mode: %s\n", argv[2]);
        return 2;
    }

    // The partial HI/LO writes of PMTHL must observe an immediately prior
    // GPR write, including when the consumer starts a different CFG block.
    std::vector<uint32_t> words =
    {
        make_i(0x09, 0, 10, 0x1357) // ADDIU $t2,$zero,0x1357
    };
    if (prior_gpr_write)
    {
        words.push_back(make_i(0x09, 0, 4, 0x1234));
    }
    if (cross_block)
    {
        words.push_back(make_i(0x04, 0, 0, 1)); // BEQ zero,zero,+1
        words.push_back(0u);                   // delay slot
    }
    if (pmthl)
    {
        words.push_back(make_mmi(use_zero_source ? 0u : 4u, 0, 0, 0, 0x31));
    }
    if (pmfhl)
    {
        words.push_back(make_mmi(0, 0, destination, pmfhl_mode, 0x30));
    }
    words.push_back((31u << 21) | 0x08u); // JR $ra
    words.push_back(0u);                  // delay slot NOP

    std::map<uint32_t, ee::s_ee_instruction> instructions;
    for (size_t index = 0; index < words.size(); ++index)
    {
        const uint32_t pc = k_entry_pc + static_cast<uint32_t>(index * 4u);
        const ee::s_ee_instruction instruction = ee::decode(pc, words[index]);
        if (instruction.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr, "MMI HI/LO fixture rejected 0x%08x at "
                         "0x%08x (%s)\n",
                         words[index], pc, instruction.mnemonic);
            return 3;
        }
        instructions.emplace(pc, instruction);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(instructions, {k_entry_pc});
    if (functions.size() != 1 || functions[0].blocks.empty())
    {
        std::fprintf(stderr, "MMI HI/LO CFG analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("mmi_hilo_native_test");
    llvm::module_builder builder(context, opt_level);
    builder.emit_function_cfg(functions[0], instructions, "guest_mmi_hilo_test");
    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write MMI HI/LO IR\n");
        return 5;
    }
    return 0;
}
