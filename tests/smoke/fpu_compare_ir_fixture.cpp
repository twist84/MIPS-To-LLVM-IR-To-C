#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>

namespace
{
uint32_t make_cop1_s(uint8_t ft, uint8_t fs, uint8_t cc, uint8_t funct)
{
    return (0x11u << 26) |
           (0x10u << 21) |
           (static_cast<uint32_t>(ft) << 16) |
           (static_cast<uint32_t>(fs) << 11) |
           (static_cast<uint32_t>(cc) << 6) |
           funct;
}

uint32_t make_cop1_transfer(uint8_t fmt, uint8_t rt, uint8_t rd)
{
    return (0x11u << 26) |
           (static_cast<uint32_t>(fmt) << 21) |
           (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11);
}
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: ee_fpu_compare_ir_fixture output.ll mode\n");
        return 2;
    }

    const char* mode = argv[2];
    const bool fcr_read_low = std::strcmp(mode, "fcr_read_low") == 0;
    const bool fcr_read_high = std::strcmp(mode, "fcr_read_high") == 0;
    const bool fcr_read_group = fcr_read_low || fcr_read_high;
    uint8_t funct = 0;
    if (std::strcmp(mode, "c_f") == 0) funct = 0x30;
    else if (std::strcmp(mode, "c_eq") == 0) funct = 0x32;
    else if (std::strcmp(mode, "c_lt") == 0) funct = 0x34;
    else if (std::strcmp(mode, "c_le") == 0) funct = 0x36;
    else if (std::strcmp(mode, "cvt_w") == 0) funct = 0x24;
    else if (std::strcmp(mode, "cvt_s_w") == 0) funct = 0x20;
    else if (std::strcmp(mode, "add_s") == 0) funct = 0x00;
    else if (std::strcmp(mode, "sub_s") == 0) funct = 0x01;
    else if (std::strcmp(mode, "mul_s") == 0) funct = 0x02;
    else if (std::strcmp(mode, "div_s") == 0) funct = 0x03;
    else if (std::strcmp(mode, "adda_s") == 0) funct = 0x18;
    else if (std::strcmp(mode, "suba_s") == 0) funct = 0x19;
    else if (std::strcmp(mode, "mula_s") == 0) funct = 0x1a;
    else if (std::strcmp(mode, "madd_s") == 0) funct = 0x1c;
    else if (std::strcmp(mode, "msub_s") == 0) funct = 0x1d;
    else if (std::strcmp(mode, "madda_s") == 0) funct = 0x1e;
    else if (std::strcmp(mode, "msuba_s") == 0) funct = 0x1f;
    else if (std::strcmp(mode, "sqrt_s") == 0) funct = 0x04;
    else if (std::strcmp(mode, "rsqrt_s") == 0) funct = 0x16;
    else if (std::strcmp(mode, "max_s") == 0) funct = 0x28;
    else if (std::strcmp(mode, "min_s") == 0) funct = 0x29;
    else if (std::strcmp(mode, "abs_s") == 0) funct = 0x05;
    else if (std::strcmp(mode, "neg_s") == 0) funct = 0x07;
    else if (std::strcmp(mode, "matrix_chain") == 0) funct = 0;
    else if (fcr_read_group ||
             std::strcmp(mode, "mfc1") == 0 ||
             std::strcmp(mode, "mtc1") == 0 ||
             std::strcmp(mode, "cfc1") == 0 ||
             std::strcmp(mode, "cfc1_fcr0") == 0 ||
             std::strcmp(mode, "cfc1_alias15") == 0 ||
             std::strcmp(mode, "cfc1_alias16") == 0 ||
             std::strcmp(mode, "ctc1_fcr0") == 0 ||
             std::strcmp(mode, "ctc1") == 0 ||
             std::strcmp(mode, "ctc1_after_branch") == 0 ||
             std::strcmp(mode, "bc1_t_delay") == 0 ||
             std::strcmp(mode, "bc1_fl_annul") == 0) funct = 0;
    else
    {
        std::fprintf(stderr, "unknown COP1 comparison mode: %s\n", mode);
        return 2;
    }

    constexpr uint32_t entry_pc = 0x00130000u;
    // Comparisons use CC0. Conversion and arithmetic forms use FD.
    const bool writes_fpr6 =
        std::strcmp(mode, "cvt_w") == 0 ||
        std::strcmp(mode, "cvt_s_w") == 0 ||
        std::strcmp(mode, "add_s") == 0 ||
        std::strcmp(mode, "sub_s") == 0 ||
        std::strcmp(mode, "mul_s") == 0 ||
        std::strcmp(mode, "div_s") == 0 ||
        std::strcmp(mode, "madd_s") == 0 ||
        std::strcmp(mode, "msub_s") == 0 ||
        std::strcmp(mode, "sqrt_s") == 0 ||
        std::strcmp(mode, "rsqrt_s") == 0 ||
        std::strcmp(mode, "max_s") == 0 ||
        std::strcmp(mode, "min_s") == 0 ||
        std::strcmp(mode, "abs_s") == 0 ||
        std::strcmp(mode, "neg_s") == 0;
    const uint8_t destination_or_cc = writes_fpr6 ? 6 : 0;
    uint32_t word = 0;
    if (std::strcmp(mode, "mfc1") == 0)
        word = make_cop1_transfer(0, 6, 1);
    else if (std::strcmp(mode, "mtc1") == 0)
        word = make_cop1_transfer(4, 8, 6);
    else if (std::strcmp(mode, "cfc1") == 0)
        word = make_cop1_transfer(2, 6, 31);
    else if (std::strcmp(mode, "cfc1_fcr0") == 0)
        word = make_cop1_transfer(2, 6, 0);
    else if (std::strcmp(mode, "cfc1_alias15") == 0)
        word = make_cop1_transfer(2, 6, 15);
    else if (std::strcmp(mode, "cfc1_alias16") == 0)
        word = make_cop1_transfer(2, 6, 16);
    else if (std::strcmp(mode, "ctc1_fcr0") == 0)
        word = make_cop1_transfer(6, 8, 0);
    else if (std::strcmp(mode, "ctc1") == 0)
        word = make_cop1_transfer(6, 8, 31);
    else if (std::strcmp(mode, "cvt_s_w") == 0)
        word = (0x11u << 26) | (0x14u << 21) | (1u << 11) |
               (static_cast<uint32_t>(destination_or_cc) << 6) | funct;
    else if (std::strcmp(mode, "sqrt_s") == 0)
        word = make_cop1_s(2, 0, destination_or_cc, funct);
    else
        word = make_cop1_s(2, 1, destination_or_cc, funct);
    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    if (fcr_read_group)
    {
        // Each group reads 16 different control-register encodings into
        // writable GPRs 1..16. No JR or host calls between the reads.
        const uint8_t base = fcr_read_high ? 16u : 0u;
        for (uint8_t i = 0u; i < 16u; ++i)
        {
            const uint32_t pc = entry_pc + 4u * i;
            const auto decoded = ee::decode(pc,
                make_cop1_transfer(2u, i + 1u, base + i));
            if (decoded.valid != ee::e_ee_valid::VALID ||
                decoded.opcode != ee::e_ee_opcode::OP_COP1)
            {
                std::fprintf(stderr, "FCR read alias %u not decoded\n", base + i);
                return 3;
            }
            insn_map.emplace(pc, decoded);
        }
    }
    else if (std::strcmp(mode, "matrix_chain") == 0)
    {
        const uint32_t words[] = {
            make_cop1_s(2, 1, 6, 0x02), // MUL.S f6,f1,f2
            make_cop1_s(2, 1, 0, 0x1a), // MULA.S
            make_cop1_s(2, 1, 0, 0x1e), // MADDA.S
            make_cop1_s(1, 6, 7, 0x1c), // MADD.S f7,f6,f1
            make_cop1_s(0, 7, 8, 0x06), // MOV.S f8,f7
        };
        for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        {
            const auto pc = entry_pc + static_cast<uint32_t>(i * 4u);
            const auto decoded = ee::decode(pc, words[i]);
            if (decoded.valid != ee::e_ee_valid::VALID)
            {
                return 3;
            }
            insn_map.emplace(pc, decoded);
        }
    }
    else if (std::strcmp(mode, "ctc1_after_branch") == 0 ||
        std::strcmp(mode, "bc1_t_delay") == 0 ||
        std::strcmp(mode, "bc1_fl_annul") == 0)
    {
        std::vector<uint32_t> words;
        if (std::strcmp(mode, "ctc1_after_branch") == 0)
        {
            // ADDIU writes $t0 in the first block. CTC1 in the successor block
            // consumes it, exercising read-before-write propagation.
            words = {
                (0x09u << 26) | (8u << 16) | 0x2345u, // addiu $t0,$zero,0x2345
                (0x04u << 26) | 1u,                   // beq $zero,$zero, next block
                0u,                                    // delay-slot nop
                make_cop1_transfer(6, 8, 31)           // ctc1 $t0,FCR31
            };
        }
        else
        {
            const uint32_t branch_kind =
                std::strcmp(mode, "bc1_t_delay") == 0 ? 1u : 2u;
            const uint32_t bc1 =
                (0x11u << 26) | (0x08u << 21) |
                (branch_kind << 16) | 4u;             // BC1T/BC1FL to +0x14
            const uint32_t jump_external =
                (0x02u << 26) | (0x00140000u >> 2);
            words = {
                bc1,
                make_cop1_transfer(6, 8, 31),          // delay slot: ctc1 $t0,FCR31
                (0x09u << 26) | (9u << 16) | 0x1111u, // fallthrough: addiu $t1,$zero,0x1111
                jump_external,
                0u,
                (0x09u << 26) | (10u << 16) | 0x2222u, // taken: addiu $t2,$zero,0x2222
                (31u << 21) | 0x08u,                   // jr $ra
                0u
            };
        }

        for (size_t i = 0; i < words.size(); ++i)
        {
            const uint32_t pc = entry_pc + static_cast<uint32_t>(i * 4);
            const ee::s_ee_instruction decoded = ee::decode(pc, words[i]);
            if (decoded.valid != ee::e_ee_valid::VALID)
            {
                std::fprintf(stderr, "COP1 control-flow instruction at 0x%08x is not VALID: %s\n",
                             pc, decoded.mnemonic);
                return 3;
            }
            insn_map.emplace(pc, decoded);
        }
    }
    else
    {
        const ee::s_ee_instruction ins = ee::decode(entry_pc, word);
        if (ins.valid != ee::e_ee_valid::VALID ||
            ins.opcode != ee::e_ee_opcode::OP_COP1)
        {
            std::fprintf(stderr, "COP1 transfer instruction was not decoded as VALID: %s\n",
                         ins.mnemonic);
            return 3;
        }
        insn_map.emplace(entry_pc, ins);
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {entry_pc});
    if (functions.size() != 1 || functions[0].blocks.empty())
    {
        std::fprintf(stderr, "COP1 fixture CFG analysis failed\n");
        return 4;
    }
    if (fcr_read_group &&
        (functions[0].blocks.size() != 1 ||
         functions[0].blocks[0].insn_indices.size() != 16))
    {
        std::fprintf(stderr, "FCR read group fixture CFG analysis failed\n");
        return 4;
    }
    if (std::strcmp(mode, "matrix_chain") == 0 &&
        (functions[0].blocks.size() != 1 ||
         functions[0].blocks[0].insn_indices.size() != 5))
    {
        std::fprintf(stderr, "matrix-chain COP1 fixture CFG analysis failed\n");
        return 4;
    }
    if (!fcr_read_group &&
        std::strcmp(mode, "matrix_chain") != 0 &&
        std::strcmp(mode, "ctc1_after_branch") != 0 &&
        std::strcmp(mode, "bc1_t_delay") != 0 &&
        std::strcmp(mode, "bc1_fl_annul") != 0 &&
        (functions[0].blocks.size() != 1 ||
         functions[0].blocks[0].insn_indices.size() != 1))
    {
        std::fprintf(stderr, "single-instruction COP1 fixture CFG analysis failed\n");
        return 4;
    }

    llvm::llvm_context context("ee_fpu_compare_test");
    llvm::module_builder builder(context);
    builder.emit_function_cfg(functions[0], insn_map, "guest_fpu_compare_test");

    if (!context.write_ir_to_file(argv[1]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[1]);
        return 5;
    }

    return 0;
}
