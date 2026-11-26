#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t ENTRY_PC = 0x00130000u;

uint32_t make_cop2(uint8_t rs, uint8_t rt, uint8_t rd)
{
    return (0x12u << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11);
}

uint32_t make_addiu(uint8_t rs, uint8_t rt, uint16_t immediate)
{
    return (0x09u << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           immediate;
}

uint32_t make_beq(uint8_t rs, uint8_t rt, int16_t immediate)
{
    return (0x04u << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t make_jr(uint8_t rs)
{
    return (static_cast<uint32_t>(rs) << 21) | 0x08u;
}

uint32_t make_j(uint32_t target)
{
    return (0x02u << 26) | ((target >> 2) & 0x03ffffffu);
}

bool build_words(const std::string& mode, std::vector<uint32_t>& words)
{
    // Synthetic EE load/store fixtures use the same LLVM backend and
    // authentic exception entry helper as the VU0 alignment tests.
    if (mode.rfind("ee_", 0u) == 0u)
    {
        std::string operation = mode.substr(3u);
        if (operation == "sw_trace_context" || operation == "sq_trace_context")
        {
            // Dirty SP before the store; FP is consumed after the cold arm.
            const uint32_t op = operation == "sw_trace_context" ? 0x2bu : 0x1fu;
            const uint32_t store = (op << 26u) | (9u << 21u) | (8u << 16u);
            words = {make_addiu(29u, 29u, -16), store,
                     make_addiu(30u, 10u, 1), make_jr(31u), 0u};
            return true;
        }
        if (operation == "likely_clean_gprs")
        {
            // POR r11,r8,zero commits both halves before BEQL. The
            // taken delay slot reads the already-published SP and
            // changes only r10. Both successor paths return to RA.
            const uint32_t por = (0x1cu << 26u) | (8u << 21u) |
                (11u << 11u) | (0x12u << 6u) | 0x29u;
            const uint32_t beql = (0x14u << 26u) | (9u << 21u) | 3u;
            words = {por, make_addiu(29u, 29u, -16), beql,
                     make_addiu(29u, 10u, 1),
                     make_jr(31u), 0u, make_jr(31u), 0u};
            return true;
        }
        if (operation == "known_aligned" || operation == "known_misaligned" ||
            operation == "partially_aligned")
        {
            // SLL r9,r9,2 (or 1); ADDIU r9,r9,-4; LW r8,4(r9).
            // The value is dynamic, but the alignment can be proven in
            // the first mode. The other modes still use the fault gate.
            const uint32_t sll = (9u << 16u) | (9u << 11u) |
                ((operation == "partially_aligned" ? 1u : 2u) << 6u);
            const uint32_t lw = (0x23u << 26u) | (9u << 21u) | (8u << 16u) |
                (operation == "known_misaligned" ? 5u : 4u);
            words = {sll, make_addiu(9u, 9u, -4), lw};
            return true;
        }
        if (operation == "constant_address" || operation == "constant_misaligned")
        {
            // LUI sign extends; ORI, XORI, ANDI and the signed immediate
            // additions reduce the effective address to a known low word.
            const uint32_t lui = (0x0fu << 26u) | (9u << 16u) | 0xffffu;
            const uint32_t ori = (0x0du << 26u) | (9u << 21u) | (9u << 16u) | 0xfffcu;
            const uint32_t daddiu = (0x19u << 26u) | (9u << 21u) | (9u << 16u) | 0x48u;
            const uint32_t xori = (0x0eu << 26u) | (9u << 21u) | (9u << 16u) | 0x04u;
            const uint32_t andi = (0x0cu << 26u) | (9u << 21u) | (9u << 16u) | 0xffffu;
            const uint32_t lw = (0x23u << 26u) | (9u << 21u) | (8u << 16u) |
                (operation == "constant_misaligned" ? 1u : 0u);
            words = {lui, ori, daddiu, xori, andi, lw};
            return true;
        }
        if (operation == "lw_wrap_positive" || operation == "sw_wrap_negative" ||
            operation == "lw_absolute")
        {
            const bool store = operation == "sw_wrap_negative";
            const uint32_t base = operation == "lw_absolute" ? 0u : 9u;
            const uint16_t offset = store ? 0xfffcu :
                (base == 0u ? 0x40u : 0x44u);
            words = {((store ? 0x2bu : 0x23u) << 26u) |
                     (base << 21u) | (8u << 16u) | offset};
            return true;
        }
        const bool delay = operation.find("_delay") != std::string::npos;
        if (delay)
        {
            operation = operation.substr(0u, operation.find("_delay"));
        }
        static const std::map<std::string, uint32_t> k_load_store_opcodes =
        {
            {"lq", 0x1eu}, {"sq", 0x1fu},
            {"lb", 0x20u}, {"lbu", 0x24u},
            {"lh", 0x21u}, {"lhu", 0x25u}, {"lw", 0x23u},
            {"sb", 0x28u}, {"sh", 0x29u}, {"sw", 0x2bu},
            {"lwc1", 0x31u}, {"ld", 0x37u},
            {"swc1", 0x39u}, {"sd", 0x3fu}
        };
        const bool zero = operation == "lq_zero" || operation == "sq_zero";
        if (zero)
        {
            operation = operation.substr(0u, 2u);
        }
        const auto found = k_load_store_opcodes.find(operation);
        if (found == k_load_store_opcodes.end())
        {
            return false;
        }
        const uint32_t transfer = (found->second << 26u) |
                                  (9u << 21u) | ((zero ? 0u : 8u) << 16u);
        if (delay)
        {
            words = {make_beq(0u, 0u, 1), transfer, make_jr(31u), 0u};
        }
        else
        {
            words = {transfer};
        }
        return true;
    }

    // The same real LLVM builder emits quadword VU0 memory transfers.
    // Use them to check alignment faults and immutable VF00 end-to-end.
    if (mode.rfind("lqc2_vf", 0u) == 0u ||
        mode.rfind("sqc2_vf", 0u) == 0u)
    {
        const bool load = mode.rfind("lqc2", 0u) == 0u;
        const uint32_t vec = mode.find("_vf0") != std::string::npos
            ? 0u : 6u;
        const uint32_t op = load ? 0x36u : 0x3eu;
        const uint32_t transfer = (op << 26u) | (9u << 21u) |
            (vec << 16u); // base r9, vector vf0/vf6, offset zero
        if (mode.find("_delay") != std::string::npos)
        {
            words = {
                make_beq(0, 0, 1),
                transfer,
                make_jr(31),
                0u
            };
        }
        else
        {
            words = {transfer};
        }
        return true;
    }

    if (mode == "bc2_f_delay" || mode == "bc2_t_delay" ||
        mode == "bc2_fl_annul" || mode == "bc2_tl_delay")
    {
        const uint8_t branch_kind = mode == "bc2_f_delay" ? 0
            : mode == "bc2_t_delay" ? 1
            : mode == "bc2_fl_annul" ? 2 : 3;
        const uint32_t branch =
            (0x12u << 26) | (0x08u << 21) |
            (static_cast<uint32_t>(branch_kind) << 16) | 4u;
        words = {
            branch,
            make_addiu(0, 8, 0xaaaa), // delay slot: visible only when executed
            make_addiu(0, 9, 0x1111), // fallthrough
            make_j(0x00140000u),      // leave the fixture after fallthrough
            0u,
            make_addiu(0, 10, 0x2222), // taken target
            make_jr(31),
            0u
        };
        return true;
    }

    const auto vu_macro = [](uint32_t op, uint32_t mask, uint32_t fs, uint32_t ft, uint32_t fd)
    {
        return 0x4a000000u | (mask << 21u) | (ft << 16u) | (fs << 11u) | (fd << 6u) | op;
    };
    const auto vu_special = [&](uint32_t op, uint32_t mask, uint32_t fs, uint32_t ft)
    {
        return vu_macro(0x3cu | (op & 3u), mask, fs, ft, op >> 2u);
    };
    if (mode == "ir_vitof" || mode == "ir_vftoi" || mode == "ir_vclip" ||
        mode == "ir_vabs" || mode == "ir_vmove_mask" || mode == "ir_vmr32_alias")
    {
        const uint32_t op = mode == "ir_vitof" ? 0x10u : mode == "ir_vftoi" ? 0x14u :
                            mode == "ir_vclip" ? 0x1fu : mode == "ir_vabs" ? 0x1du :
                            mode == "ir_vmove_mask" ? 0x30u : 0x31u;
        const uint32_t mask = mode == "ir_vmove_mask" ? 10u : 15u;
        const uint32_t ft = mode == "ir_vmr32_alias" ? 1u : 2u;
        words = {vu_special(op, mask, 1u, ft)};
        return true;
    }
    if (mode == "ir_vmul" || mode == "ir_vadd" || mode == "ir_vmula_madd" ||
        mode == "ir_vdiv_wait" || mode == "ir_vsqrt_wait")
    {
        if (mode == "ir_vmul") { words = {vu_macro(0x2au, 15u, 1u, 2u, 3u)}; }
        else if (mode == "ir_vadd") { words = {vu_macro(0x28u, 15u, 1u, 2u, 1u)}; }
        else if (mode == "ir_vmula_madd")
        {
            words = {vu_special(0x2au, 8u, 1u, 2u), vu_macro(0x29u, 8u, 1u, 4u, 3u)};
        }
        else
        {
            words = {vu_special(mode == "ir_vdiv_wait" ? 0x38u : 0x39u, 0u, 1u, 2u),
                     vu_special(0x3bu, 0u, 0u, 0u)};
        }
        return true;
    }

    uint8_t cop2_rs = 0;
    if (mode.rfind("qmfc2", 0) == 0)
        cop2_rs = 0x01;
    else if (mode.rfind("cfc2", 0) == 0)
        cop2_rs = 0x02;
    else if (mode.rfind("qmtc2", 0) == 0)
        cop2_rs = 0x05;
    else if (mode.rfind("ctc2", 0) == 0)
        cop2_rs = 0x06;
    else
        return false;

    uint8_t vu_index = 6;
    if (mode == "qmfc2_vf0" || mode == "qmtc2_vf0")
        vu_index = 0;
    else if (mode == "cfc2_r" || mode == "ctc2_r")
        vu_index = 20;
    else if (mode == "ctc2_fbrst")
        vu_index = 28;
    else if (mode == "ctc2_cmsar1")
        vu_index = 31;

    const bool interlocked = mode.find("_i") != std::string::npos;
    const uint32_t transfer = make_cop2(cop2_rs, 8, vu_index) |
        (interlocked ? 1u : 0u);
    if (mode == "qmfc2" || mode == "qmtc2" ||
        mode == "cfc2" || mode == "ctc2" ||
        mode == "qmfc2_vf0" || mode == "qmtc2_vf0" ||
        mode == "cfc2_r" || mode == "ctc2_r" ||
        mode == "ctc2_fbrst" || mode == "ctc2_cmsar1" ||
        mode == "qmfc2_i" || mode == "qmtc2_i" ||
        mode == "cfc2_i" || mode == "ctc2_i")
    {
        words = {transfer};
        return true;
    }

    if (mode == "qmfc2_delay" || mode == "qmtc2_delay" ||
        mode == "cfc2_delay" || mode == "ctc2_delay" ||
        mode == "qmfc2_i_delay" || mode == "qmtc2_i_delay" ||
        mode == "cfc2_i_delay" || mode == "ctc2_i_delay")
    {
        words = {
            make_beq(0, 0, 1), // target is the instruction after the delay slot
            transfer,
            make_jr(31),
            0u
        };
        return true;
    }

    if (mode == "qmtc2_after_addiu" || mode == "ctc2_after_addiu")
    {
        words = {
            make_addiu(0, 8, 0x1234),
            transfer
        };
        return true;
    }

    if (mode == "cfc2_then_addiu")
    {
        words = {
            make_cop2(0x02, 9, 6), // CFC2 r9, VI6
            make_addiu(9, 10, 1)  // consumes the value written by the helper
        };
        return true;
    }

    return false;
}
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr,
                     "usage: ee_vu0_transfer_ir_fixture output.ll mode\n");
        return 2;
    }

    const std::string mode = argv[2];
    std::vector<uint32_t> words;
    if (!build_words(mode, words))
    {
        std::fprintf(stderr, "unknown VU0 transfer mode: %s\n", argv[2]);
        return 2;
    }

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    for (size_t i = 0; i < words.size(); ++i)
    {
        const uint32_t pc = ENTRY_PC + static_cast<uint32_t>(i * 4);
        const ee::s_ee_instruction ins = ee::decode(pc, words[i]);
        if (ins.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr,
                         "VU0 fixture instruction at 0x%08x is not VALID: %s\n",
                         pc, ins.mnemonic);
            return 3;
        }
        insn_map.emplace(pc, ins);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {ENTRY_PC});
    if (functions.size() != 1 || functions[0].blocks.empty())
    {
        std::fprintf(stderr, "VU0 transfer fixture CFG analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("ee_vu0_transfer_test");
    llvm::module_builder builder(context);
    builder.emit_function_cfg(functions[0], insn_map,
                              "guest_vu0_transfer_test");

    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[1]);
        return 5;
    }

    return 0;
}
