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
uint32_t make_i(uint8_t opcode, uint8_t rs, uint8_t rt, int16_t immediate)
{
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t make_r(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct)
{
    return (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) |
           (static_cast<uint32_t>(sa) << 6) |
           funct;
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: ee_scalar_specialize_ir_fixture output.ll\n");
        return 2;
    }

    constexpr uint32_t entry_pc = 0x00120000u;
    const std::vector<uint32_t> words =
    {
        make_i(0x0f, 0, 3, -32768),        // LUI r3 = sign-extended 0x80000000
        make_i(0x09, 0, 4, -1),            // ADDIU r4 = -1
        make_r(3, 4, 5, 0, 0x21),          // ADDU r5 = 0x7fffffff
        make_r(3, 4, 6, 0, 0x2d),          // DADDU r6 = -2147483649
        make_r(10, 10, 7, 0, 0x23),        // SUBU identical = 0
        make_r(10, 10, 8, 0, 0x26),        // XOR identical = 0
        make_r(10, 0, 9, 0, 0x24),         // AND zero
        make_r(10, 0, 11, 0, 0x25),        // OR identity
        make_r(10, 0, 12, 0, 0x2b),        // SLTU x,0
        make_r(4, 3, 13, 0, 0x04),         // SLLV known value/count
        make_r(4, 3, 14, 0, 0x17),         // DSRAV known value/count
        make_r(0, 10, 15, 0, 0x00),        // SLL x,0 still sign extends
        make_r(4, 10, 16, 0, 0x14),        // DSLLV constant count 63
        make_i(0x0c, 10, 17, 0),           // ANDI x,0
        make_i(0x19, 10, 18, 0),           // DADDIU identity
        make_r(10, 0, 19, 0, 0x21),        // ADDU x,0 still sign extends
        make_i(0x0b, 10, 20, 0),           // SLTIU x,0
        make_r(10, 0, 21, 0, 0x27),        // NOR x,0
        make_i(0x0d, 4, 2, 0),             // V0 write still observed
        make_r(3, 4, 0, 0, 0x21),          // discarded pure ALU, still counted
        make_i(0x04, 0, 0, 1),             // BEQ to next block
        make_r(3, 4, 22, 0, 0x21),         // delay slot
        make_r(3, 4, 23, 0, 0x2d),         // external continuation must read state
        make_r(0, 3, 24, 0, 0x3c),         // DSLL32
        make_i(0x0c, 10, 25, 31),          // ANDI produces a bounded count
        make_r(25, 10, 26, 0, 0x14),       // DSLLV needs no count mask
        make_i(0x0b, 25, 27, 32),          // SLTIU range proven true
        make_i(0x0c, 25, 28, 0x100),       // disjoint bits -> zero
        make_i(0x0d, 25, 29, 31),          // known result 31
        make_r(29, 10, 30, 0, 0x04),       // SLLV fixed count
        make_r(0, 30, 31, 0, 0x00)         // word identity needs no trunc/sext
    };

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    for (size_t i = 0; i < words.size(); ++i)
    {
        const uint32_t pc = entry_pc + static_cast<uint32_t>(i * 4u);
        const ee::s_ee_instruction ins = ee::decode(pc, words[i]);
        if (ins.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr, "scalar specialization fixture instruction at 0x%08x is not VALID: %s\n",
                         pc, ins.mnemonic);
            return 3;
        }
        insn_map.emplace(pc, ins);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {entry_pc});
    if (functions.size() != 1 || functions[0].blocks.size() != 2)
    {
        std::fprintf(stderr, "scalar specialization fixture CFG analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("ee_scalar_specialize_test");
    llvm::module_builder builder(context);
    builder.emit_function_cfg(functions[0], insn_map, "guest_scalar_specialize_test");

    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[1]);
        return 5;
    }

    return 0;
}
