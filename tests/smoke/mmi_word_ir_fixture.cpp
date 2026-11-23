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
constexpr uint32_t k_entry_pc = 0x00180000u;

uint32_t make_mmi(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa,
                  uint8_t funct)
{
    return (0x1cu << 26) | (static_cast<uint32_t>(rs) << 21) |
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
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr,
                     "usage: ee_mmi_word_ir_fixture output.ll mode opt_level\n");
        return 2;
    }
    const int opt_level = std::atoi(argv[3]);
    if (opt_level < 0 || opt_level > 3)
    {
        std::fprintf(stderr, "opt_level must be 0..3\n");
        return 2;
    }

    std::string mode = argv[2];
    const auto ends_with = [](const std::string& value,
                              const std::string& suffix) -> bool
    {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(),
                             suffix.size(), suffix) == 0;
    };
    const auto starts_with = [](const std::string& value,
                                const std::string& prefix) -> bool
    {
        return value.compare(0, prefix.size(), prefix) == 0;
    };
    const std::string rs_alias = "_alias_rs";
    const std::string rt_alias = "_alias_rt";
    const std::string zero_dest = "_zero_dest";
    bool chain = false;
    bool source_prior = false;
    bool cross_block = false;
    bool zero_rs = false;
    bool zero_rt = false;
    uint8_t rd = 6u;

    if (mode == "pmthi_then_pmfhi" || mode == "pmtlo_then_pmflo")
    {
        chain = true;
        mode = mode == "pmthi_then_pmfhi" ? "pmthi" : "pmtlo";
    }
    else if (mode == "pmthi_from_prior" || mode == "pmthi_from_prior_cross" ||
             mode == "pmtlo_from_prior" || mode == "pmtlo_from_prior_cross" ||
             mode == "pmultw_from_prior_cross")
    {
        cross_block = ends_with(mode, "_cross");
        source_prior = true;
        if (starts_with(mode, "pmthi"))
        {
            mode = "pmthi";
        }
        else if (starts_with(mode, "pmtlo"))
        {
            mode = "pmtlo";
        }
        else
        {
            mode = "pmultw";
        }
    }
    else if (ends_with(mode, "_from_prior_cross"))
    {
        mode.resize(mode.size() - std::string("_from_prior_cross").size());
        source_prior = true;
        cross_block = true;
    }
    else if (ends_with(mode, rs_alias))
    {
        mode.resize(mode.size() - rs_alias.size());
        rd = 4u;
    }
    else if (ends_with(mode, rt_alias))
    {
        mode.resize(mode.size() - rt_alias.size());
        rd = 5u;
    }
    else if (ends_with(mode, zero_dest))
    {
        mode.resize(mode.size() - zero_dest.size());
        rd = 0u;
    }
    else if (ends_with(mode, "_zero_rs"))
    {
        mode.resize(mode.size() - 8u);
        zero_rs = true;
    }
    else if (ends_with(mode, "_zero_rt"))
    {
        mode.resize(mode.size() - 8u);
        zero_rt = true;
    }

    bool source_prior_rt = false;
    if (ends_with(mode, "_from_prior_rt_cross"))
    {
        mode.resize(mode.size() - std::string("_from_prior_rt_cross").size());
        source_prior_rt = true;
        cross_block = true;
    }

    bool same_source = false;
    if (ends_with(mode, "_same_source"))
    {
        mode.resize(mode.size() - std::string("_same_source").size());
        same_source = true;
    }

    bool repeat = false;
    if (ends_with(mode, "_twice"))
    {
        mode.resize(mode.size() - std::string("_twice").size());
        repeat = true;
    }

    uint8_t funct = 0u;
    uint8_t sa = 0u;
    uint8_t rs = 4u;
    uint8_t rt = 5u;
    if (mode == "pmfhi" || mode == "pmflo")
    {
        funct = 0x09u;
        sa = mode == "pmfhi" ? 0x08u : 0x09u;
        rs = 0u;
        rt = 0u;
    }
    else if (mode == "pmthi" || mode == "pmtlo")
    {
        funct = 0x29u;
        sa = mode == "pmthi" ? 0x08u : 0x09u;
        rt = 0u;
        rd = 0u;
    }
    else if (mode == "pmaddw" || mode == "pmsubw" || mode == "pmadduw")
    {
        funct = mode == "pmadduw" ? 0x29u : 0x09u;
        sa = mode == "pmsubw" ? 0x04u : 0x00u;
    }
    else if (mode == "pmaddh" || mode == "pmsubh" ||
             mode == "phmadh" || mode == "phmsbh" || mode == "pmulth")
    {
        funct = 0x09u;
        sa = mode == "pmaddh" ? 0x10u :
             mode == "phmadh" ? 0x11u :
             mode == "pmsubh" ? 0x14u :
             mode == "phmsbh" ? 0x15u : 0x1cu;
    }
    else if (mode == "psllvw" || mode == "psrlvw")
    {
        funct = 0x09u;
        sa = mode == "psllvw" ? 0x02u : 0x03u;
    }
    else if (mode == "pdivbw")
    {
        funct = 0x09u;
        sa = 0x1du;
        rd = 0u; // PDIVBW writes HI/LO only, never a GPR.
    }
    else if (mode == "pmultw" || mode == "pmultuw" ||
             mode == "pdivw" || mode == "pdivuw")
    {
        funct = mode == "pmultw" || mode == "pdivw" ? 0x09u : 0x29u;
        sa = starts_with(mode, "pmult") ? 0x0cu : 0x0du;
        if (sa == 0x0du)
        {
            rd = 0u;
        }
    }
    else
    {
        std::fprintf(stderr, "unknown packed MMI mode %s\n", argv[2]);
        return 2;
    }

    const std::vector<uint32_t> prefix =
    {
        make_i(0x09u, 0u, 10u, 0x1357) // Preceding ADDIU must retire.
    };
    std::vector<uint32_t> words = prefix;
    if (source_prior)
    {
        words.push_back(make_i(0x09u, 0u, 4u, 0x1234));
    }
    if (source_prior_rt)
    {
        words.push_back(make_i(0x09u, 0u, 5u, 0x4321));
    }
    if (cross_block)
    {
        words.push_back(make_i(0x04u, 0u, 0u, 1));
        words.push_back(0u); // Branch delay slot.
    }

    rs = zero_rs ? 0u : rs;
    rt = zero_rt ? 0u : (same_source ? rs : rt);
    words.push_back(make_mmi(rs, rt, rd, sa, funct));
    if (repeat)
    {
        words.push_back(make_mmi(rs, rt, rd, sa, funct));
    }
    if (chain)
    {
        const bool hi = sa == 0x08u;
        words.push_back(make_mmi(0, 0, 6, hi ? 0x08u : 0x09u, 0x09u));
    }
    words.push_back((31u << 21) | 0x08u); // JR $ra.
    words.push_back(0u);

    std::map<uint32_t, ee::s_ee_instruction> insns;
    for (size_t index = 0; index < words.size(); ++index)
    {
        const uint32_t pc = k_entry_pc + static_cast<uint32_t>(index * 4);
        const auto decoded = ee::decode(pc, words[index]);
        if (decoded.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr,
                         "MMI word fixture rejected %08x at %08x (%s)\n",
                         words[index], pc, decoded.mnemonic);
            return 3;
        }
        insns.emplace(pc, decoded);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insns, {k_entry_pc});
    if (functions.size() != 1 || functions[0].blocks.empty())
    {
        std::fprintf(stderr, "MMI word CFG analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("mmi_word_native_test");
    llvm::module_builder builder(context, opt_level);
    builder.emit_function_cfg(functions[0], insns, "guest_mmi_word_test");
    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "cannot write packed MMI IR\n");
        return 5;
    }
    return 0;
}
