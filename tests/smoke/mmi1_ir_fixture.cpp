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
uint32_t make_mmi(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct)
{
    return (0x1cu << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) |
           (static_cast<uint32_t>(sa) << 6) |
           funct;
}

uint32_t make_regimm(uint8_t rs, uint8_t rt, int16_t immediate)
{
    return (0x01u << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           static_cast<uint16_t>(immediate);
}
}

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
    {
        std::fprintf(stderr, "usage: ee_mmi1_ir_fixture output.ll [opt_level]\n");
        return 2;
    }

    int opt_level = argc == 3 ? std::atoi(argv[2]) : 0;
    if (opt_level < 0 || opt_level > 3)
    {
        std::fprintf(stderr, "opt_level must be between 0 and 3\n");
        return 2;
    }

    constexpr uint32_t entry_pc = 0x00120000u;
    const std::vector<uint32_t> words =
    {
        0x70491ba9u,                     // SDBZ 0x0018E154: PCPYUD $v1,$v0,$t1
        make_mmi(0, 4, 6,  1, 0x28), // PABSW  $a2, $a0
        make_mmi(0, 4, 7,  5, 0x28), // PABSH  $a3, $a0
        make_mmi(4, 5, 8,  2, 0x28), // PCEQW  $t0, $a0, $a1
        make_mmi(4, 5, 9,  6, 0x28), // PCEQH  $t1, $a0, $a1
        make_mmi(4, 5, 10, 10, 0x28), // PCEQB $t2, $a0, $a1
        make_mmi(4, 5, 11, 3, 0x28), // PMINW  $t3, $a0, $a1
        make_mmi(4, 5, 12, 7, 0x28), // PMINH  $t4, $a0, $a1
        make_mmi(4, 5, 13, 16, 0x28), // PADDUW $t5, $a0, $a1
        make_mmi(4, 5, 14, 17, 0x28), // PSUBUW $t6, $a0, $a1
        make_mmi(4, 5, 15, 20, 0x28), // PADDUH $t7, $a0, $a1
        make_mmi(4, 5, 16, 21, 0x28), // PSUBUH $s0, $a0, $a1
        make_mmi(4, 5, 17, 24, 0x28), // PADDUB $s1, $a0, $a1
        make_mmi(4, 5, 18, 25, 0x28), // PSUBUB $s2, $a0, $a1
        make_mmi(4, 5, 19,  4, 0x28), // PADSBH $s3, $a0, $a1
        make_mmi(4, 5, 20, 26, 0x28), // PEXTUB $s4, $a0, $a1
        make_mmi(4, 5, 21, 22, 0x28), // PEXTUH $s5, $a0, $a1
        make_mmi(4, 5, 22, 18, 0x28), // PEXTUW $s6, $a0, $a1
        make_regimm(25, 0x18, 0),    // MTSAB $t9, 0
        0x00000828u,                 // MFSA $at (byte units)
        0x00200029u,                 // MTSA $at (round trip)
        make_mmi(4, 5, 23, 27, 0x28), // QFSRV $s7, $a0, $a1
        0x00000000u,                 // NOP: satisfy the SA read hazard window
        0x00000000u,
        0x00000000u,
        make_regimm(26, 0x19, 0),    // MTSAH $k0, 0
        0x00000828u,                 // MFSA $at
        0x00200029u,                 // MTSA $at
        make_mmi(4, 5, 24, 27, 0x28), // QFSRV $t8, $a0, $a1
        make_mmi(4, 5, 4,  2, 0x28), // PCEQW $a0, $a0, $a1 (rd aliases rs)
        make_mmi(29, 30, 25, 0x0e, 0x09), // PCPYLD $t9, $sp, $fp
        make_mmi(0, 30, 26, 0x1b, 0x29),  // PCPYH $k0, $fp
        make_mmi(0, 29, 27, 0x1b, 0x29),  // PCPYH $k1, $sp
        make_mmi(29, 30, 29, 0x0e, 0x09), // PCPYLD $sp, $sp, $fp (aliases rs)
        make_mmi(0, 29, 28, 0x1b, 0x29),  // PCPYH $gp, $sp (sees latest write)
        make_mmi(29, 30, 31, 0x0e, 0x29), // PCPYUD $ra, $sp, $fp (upper copies)
        make_mmi(29, 30, 30, 0x0e, 0x29), // PCPYUD $fp, $sp, $fp (aliases rt)
        make_mmi(29, 30, 29, 0x0e, 0x29), // PCPYUD $sp, $sp, $fp (aliases rs)
        make_mmi(0, 30, 2, 0x0e, 0x29),   // PCPYUD $v0, $zero, $fp
        make_mmi(29, 30, 0, 0x0e, 0x29),  // PCPYUD $zero, $sp, $fp (discard)
        0x00a00029u,                     // MTSA $a1 (mask to low four bits)
        0x00000028u,                     // MFSA $zero (discard)
        0x00000828u                      // MFSA $at (preserve high half)
    };

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    for (size_t i = 0; i < words.size(); ++i)
    {
        const uint32_t pc = entry_pc + static_cast<uint32_t>(i * 4);
        ee::s_ee_instruction ins = ee::decode(pc, words[i]);
        if (ins.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr, "MMI1 fixture instruction at 0x%08x is not VALID: %s\n",
                         pc, ins.mnemonic);
            return 3;
        }
        insn_map.emplace(pc, ins);
    }

    // PCPYH is unary and requires its reserved rs field to be zero.
    const ee::s_ee_instruction bad_pcpyh =
        ee::decode(entry_pc, make_mmi(4, 5, 6, 0x1b, 0x29));
    if (bad_pcpyh.valid != ee::e_ee_valid::UNSUPPORTED)
    {
        std::fprintf(stderr, "nonzero PCPYH rs unexpectedly accepted\n");
        return 3;
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {entry_pc});
    if (functions.size() != 1 || functions[0].blocks.empty())
    {
        std::fprintf(stderr, "MMI1 fixture function analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("mmi1_runtime_test");
    llvm::module_builder builder(context, opt_level);
    builder.emit_function_cfg(functions[0], insn_map, "guest_mmi1_test");
    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[1]);
        return 5;
    }

    return 0;
}
