#include "ee_decoder.h"
#include "ee_instruction.h"
#include "function_analyzer.h"
#include "llvm_context.h"
#include "module_builder.h"
#include "ee_runtime.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

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
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: ee_control_flow_ir_fixture case output.ll\n");
        return 2;
    }

    constexpr uint32_t entry_pc = 0x00100000u;
    const std::string mode = argv[1];
    const uint32_t function_entry_pc =
        mode == "callee_return" ? entry_pc + 0x2000u : entry_pc;
    uint32_t control_word = 0;
    uint32_t delay_word = 0;
    uint32_t continuation_word = 0;
    bool has_continuation = false;
    bool is_recursive = false;
    bool is_branch_liveness = false;
    bool is_cross_block_special = false;
    const char* function_name = nullptr;

    if (mode == "jr")
    {
        // JR $v0; the delay slot overwrites $v0. The dispatcher must receive
        // the target captured before the delay slot executes.
        control_word = make_r(2, 0, 0, 0, 0x08);
        delay_word = make_i(0x09, 0, 2, 0x1234);
        function_name = "guest_jr_delay_test";
    }
    else if (mode == "jrra")
    {
        // JR $ra returns to the captured target even though its delay slot
        // overwrites the architectural link register.
        control_word = make_r(31, 0, 0, 0, 0x08);
        delay_word = make_i(0x09, 0, 31, 0x1234);
        function_name = "guest_jr_return_test";
    }
    else if (mode == "jr_daddu")
    {
        // The sound pool returns a pointer using DADDU v0,s3,zero.
        // Preserve the full pointer through a native JR boundary rather
        // than reusing a prior callee's scalar status value.
        control_word = make_r(31, 0, 0, 0, 0x08);
        delay_word = make_r(19, 0, 2, 0, 0x2d);
        function_name = "guest_jr_daddu_return_test";
    }
    else if (mode == "branch_liveness")
    {
        // A pointer written by a branch delay slot must reach BEQL in the
        // next block, even though BEQL's own delay slot writes the same GPR.
        control_word = make_i(0x04, 0, 0, 1); // beq zero,zero,entry+8
        delay_word = make_r(19, 0, 2, 0, 0x2d); // daddu v0,s3,zero
        is_branch_liveness = true;
        function_name = "guest_branch_liveness_test";
    }
    else if (mode == "movz_liveness" || mode == "movn_liveness" ||
             mode == "srav_liveness")
    {
        // Internal CFG edge: producer is a branch delay-slot DADDU,
        // and a following basic block consumes the value. If SPECIAL
        // read-before-write accounting misses this, the consumer sees
        // the original architectural value instead of the SSA result.
        control_word = make_i(0x04, 0, 0, 1); // beq zero,zero,entry+8
        delay_word = make_r(8, 0, 19, 0, 0x2d); // daddu s3,t0,zero
        is_cross_block_special = true;
        function_name =
            mode == "movz_liveness" ? "guest_movz_liveness_test" :
            mode == "movn_liveness" ? "guest_movn_liveness_test" :
            "guest_srav_liveness_test";
    }
    else if (mode == "jal")
    {
        // JAL writes $ra before its delay slot; the slot reads that link value.
        constexpr uint32_t target = entry_pc + 0x2000u;
        control_word = (0x03u << 26) | ((target >> 2) & 0x03ffffffu);
        delay_word = make_i(0x09, 31, 8, 0);
        function_name = "guest_jal_delay_test";
    }
    else if (mode == "jalr")
    {
        // JALR $t0,($t0): capture the target before rd aliases rs and receives
        // PC+8. The delay slot then reads the newly written link in $t0.
        control_word = make_r(8, 0, 8, 0, 0x09);
        delay_word = make_i(0x09, 8, 9, 1);
        function_name = "guest_jalr_delay_test";
    }
    else if (mode == "jalr_rd")
    {
        // JALR $s0,($t0): honour encoded rd independently of target register.
        control_word = make_r(8, 0, 16, 0, 0x09);
        delay_word = make_i(0x09, 16, 9, 1);
        function_name = "guest_jalr_rd_delay_test";
    }
    else if (mode == "recursive")
    {
        // One recursive activation: the initial BEQ enters the call block;
        // the recursive activation increments $v0 and takes the base return.
        control_word = make_i(0x04, 2, 0, 4); // beq $v0,$zero, entry+0x14
        delay_word = make_i(0x09, 2, 2, 1);   // addiu $v0,$v0,1
        is_recursive = true;
        function_name = "guest_recursive_call_test";
    }
    else if (mode == "caller_resume")
    {
        // The JAL dispatches to a separate callee. On JR $ra, the host
        // dispatcher re-enters this function at PC+8 and must select block 2.
        constexpr uint32_t target = entry_pc + 0x2000u;
        control_word = (0x03u << 26) | ((target >> 2) & 0x03ffffffu);
        delay_word = make_i(0x09, 0, 16, 0x55); // $s0 = 0x55
        continuation_word = make_i(0x09, 0, 17, 0x66); // $s1 = 0x66
        has_continuation = true;
        function_name = "guest_caller_resume_test";
    }
    else if (mode == "caller_jalr")
    {
        // JALR uses $t0 as its dynamic target and $ra as its link. The delay
        // slot copies the new link into $t0, so dispatch must use the target
        // sampled before the slot overwrites rs.
        control_word = make_r(8, 0, 31, 0, 0x09); // jalr $ra,($t0)
        delay_word = make_r(31, 0, 8, 0, 0x21);   // addu $t0,$ra,$zero
        continuation_word = make_i(0x09, 0, 17, 0x66); // $s1 = 0x66
        has_continuation = true;
        function_name = "guest_caller_jalr_test";
    }
    else if (mode == "callee_return")
    {
        // Return through the caller's link. The host should resume at PC+8.
        control_word = make_r(31, 0, 0, 0, 0x08);
        delay_word = make_i(0x09, 0, 8, 0x77); // $t0 = 0x77
        function_name = "guest_callee_return_test";
    }
    else if (mode == "loop_yield")
    {
        // Infinite real guest control flow, with one ADDIU delay slot per
        // taken backwards branch. The translated header must yield before
        // re-executing the first instruction of the next iteration.
        control_word = make_i(0x04, 0, 0, -1);
        delay_word = make_i(0x09, 8, 8, 1);
        function_name = "guest_loop_yield_test";
    }
    else if (mode == "beq_taken")
    {
        // BEQ must sample $v0 before its delay slot increments it.
        control_word = make_i(0x04, 2, 0, 4);
        delay_word = make_i(0x09, 2, 2, 1);
        function_name = "guest_beq_taken_delay_test";
    }
    else if (mode == "beq_not_taken")
    {
        // BEQ must remain not-taken even though the delay slot makes $v0 zero.
        control_word = make_i(0x04, 2, 0, 4);
        delay_word = make_i(0x09, 0, 2, 0);
        function_name = "guest_beq_not_taken_delay_test";
    }
    else if (mode == "bltzal")
    {
        // BLTZAL $v0, +4; its delay slot executes on both branch outcomes and
        // observes the unconditional PC+8 link value.
        control_word = make_i(0x01, 2, 0x10, 4);
        delay_word = make_i(0x09, 31, 8, 0);
        function_name = "guest_bltzal_delay_test";
    }
    else if (mode == "bltzall")
    {
        // BLTZALL has an annulled delay slot on the not-taken path. The link
        // is still written unconditionally before the branch outcome.
        control_word = make_i(0x01, 2, 0x12, 4);
        delay_word = make_i(0x09, 31, 8, 0);
        function_name = "guest_bltzall_delay_test";
    }
    else if (mode == "bgezal")
    {
        control_word = make_i(0x01, 2, 0x11, 4);
        delay_word = make_i(0x09, 31, 8, 0);
        function_name = "guest_bgezal_delay_test";
    }
    else if (mode == "bgezall")
    {
        control_word = make_i(0x01, 2, 0x13, 4);
        delay_word = make_i(0x09, 31, 8, 0);
        function_name = "guest_bgezall_delay_test";
    }
    else
    {
        std::fprintf(stderr, "unknown control-flow fixture case: %s\n", mode.c_str());
        return 2;
    }

    std::map<uint32_t, ee::s_ee_instruction> insn_map;
    const ee::s_ee_instruction control = ee::decode(function_entry_pc, control_word);
    const ee::s_ee_instruction delay = ee::decode(function_entry_pc + 4u, delay_word);

    if (control.valid != ee::e_ee_valid::VALID || !control.has_delay_slot)
    {
        std::fprintf(stderr, "%s control transfer failed to decode: %s (valid=%u)\n",
                     mode.c_str(), control.mnemonic, static_cast<unsigned>(control.valid));
        return 3;
    }
    if (delay.valid != ee::e_ee_valid::VALID)
    {
        std::fprintf(stderr, "%s delay-slot instruction failed to decode: %s\n",
                     mode.c_str(), delay.mnemonic);
        return 4;
    }

    insn_map.emplace(control.pc, control);
    insn_map.emplace(delay.pc, delay);
    if (has_continuation)
    {
        const ee::s_ee_instruction continuation =
            ee::decode(function_entry_pc + 8u, continuation_word);
        if (continuation.valid != ee::e_ee_valid::VALID)
        {
            std::fprintf(stderr, "%s continuation instruction failed to decode: %s\n",
                         mode.c_str(), continuation.mnemonic);
            return 5;
        }
        insn_map.emplace(continuation.pc, continuation);
    }
    if (is_recursive)
    {
        const uint32_t recursive_pcs[] =
        {
            entry_pc + 8u, entry_pc + 12u, entry_pc + 20u,
            entry_pc + 24u, entry_pc + 28u, entry_pc + 32u,
            entry_pc + 36u
        };
        const uint32_t recursive_words[] =
        {
            make_r(31, 0, 0, 0, 0x08),                         // jr $ra
            make_i(0x09, 0, 8, 0x77),                          // delay: addiu $t0,$zero,0x77
            make_r(31, 0, 26, 0, 0x21),                        // addu $k0,$ra,$zero
            (0x03u << 26) | ((entry_pc >> 2) & 0x03ffffffu),   // jal entry_pc
            make_i(0x09, 0, 9, 0x11),                          // delay: addiu $t1,$zero,0x11
            make_r(26, 0, 0, 0, 0x08),                         // jr $k0
            make_i(0x09, 0, 10, 0x22)                          // delay: addiu $t2,$zero,0x22
        };
        for (size_t i = 0; i < sizeof(recursive_pcs) / sizeof(recursive_pcs[0]); ++i)
        {
            const ee::s_ee_instruction extra = ee::decode(recursive_pcs[i], recursive_words[i]);
            if (extra.valid != ee::e_ee_valid::VALID)
            {
                std::fprintf(stderr, "%s recursive instruction at 0x%08x failed to decode: %s\n",
                             mode.c_str(), recursive_pcs[i], extra.mnemonic);
                return 6;
            }
            insn_map.emplace(extra.pc, extra);
        }
    }

    if (is_cross_block_special)
    {
        const uint32_t special =
            mode == "movz_liveness" ? make_r(19, 9, 2, 0, 0x0a) :
            mode == "movn_liveness" ? make_r(19, 9, 2, 0, 0x0b) :
            make_r(19, 17, 2, 0, 0x07);
        const uint32_t pcs[] = {entry_pc + 8u, entry_pc + 12u, entry_pc + 16u};
        const uint32_t words[] =
        {
            special,
            make_r(31, 0, 0, 0, 0x08), // jr ra
            make_i(0x09, 0, 18, 0x55)  // delay-slot marker
        };
        for (size_t index = 0; index < sizeof(pcs) / sizeof(pcs[0]); ++index)
        {
            const ee::s_ee_instruction extra = ee::decode(pcs[index], words[index]);
            if (extra.valid != ee::e_ee_valid::VALID)
            {
                std::fprintf(stderr, "special liveness instruction at 0x%08x failed to decode\n",
                             pcs[index]);
                return 6;
            }
            insn_map.emplace(extra.pc, extra);
        }
    }
    if (is_branch_liveness)
    {
        const uint32_t pcs[] =
        {
            entry_pc + 8u, entry_pc + 12u,
            entry_pc + 16u, entry_pc + 20u,
            entry_pc + 24u, entry_pc + 28u
        };
        const uint32_t words[] =
        {
            make_i(0x14, 2, 0, 3),     // beql v0,zero,entry+24
            make_i(0x09, 0, 2, 0x15),  // taken slot: overwrite v0
            make_r(31, 0, 0, 0, 0x08), // not-taken: jr ra
            make_i(0x09, 0, 17, 0x66), // not-taken marker
            make_r(31, 0, 0, 0, 0x08), // taken: jr ra
            make_i(0x09, 0, 17, 0x77)  // taken marker
        };
        for (size_t i = 0; i < sizeof(pcs) / sizeof(pcs[0]); ++i)
        {
            const ee::s_ee_instruction extra = ee::decode(pcs[i], words[i]);
            if (extra.valid != ee::e_ee_valid::VALID)
            {
                std::fprintf(stderr, "branch-liveness instruction at 0x%08x failed to decode\n",
                             pcs[i]);
                return 6;
            }
            insn_map.emplace(extra.pc, extra);
        }
    }

    ee::function_analyzer analyzer;
    const auto functions = analyzer.analyze(insn_map, {function_entry_pc});
    const size_t expected_blocks =
        (is_recursive || is_branch_liveness) ? 4u :
        (has_continuation || is_cross_block_special) ? 2u : 1u;
    bool block_layout_valid =
        !functions.empty() && functions[0].blocks.size() == expected_blocks &&
        !functions[0].blocks.empty() &&
        functions[0].blocks[0].insn_indices.size() == 2;
    if (block_layout_valid && has_continuation)
    {
        block_layout_valid =
            functions[0].blocks[1].start_pc == function_entry_pc + 8u &&
            functions[0].blocks[1].insn_indices.size() == 1;
    }
    if (block_layout_valid && is_recursive)
    {
        const auto& blocks = functions[0].blocks;
        block_layout_valid =
            blocks[1].start_pc == entry_pc + 8u && blocks[1].insn_indices.size() == 2 &&
            blocks[2].start_pc == entry_pc + 20u && blocks[2].insn_indices.size() == 3 &&
            blocks[3].start_pc == entry_pc + 32u && blocks[3].insn_indices.size() == 2;
    }
    if (block_layout_valid && is_cross_block_special)
    {
        const auto& blocks = functions[0].blocks;
        block_layout_valid =
            blocks[1].start_pc == entry_pc + 8u &&
            blocks[1].insn_indices.size() == 3u;
    }
    if (block_layout_valid && is_branch_liveness)
    {
        const auto& blocks = functions[0].blocks;
        for (size_t index = 1; index < 4u; ++index)
        {
            if (blocks[index].start_pc != entry_pc + static_cast<uint32_t>(index * 8u) ||
                blocks[index].insn_indices.size() != 2u)
            {
                block_layout_valid = false;
            }
        }
    }
    if (functions.size() != 1 || !block_layout_valid)
    {
        std::fprintf(stderr, "%s fixture CFG did not retain expected delay/continuation blocks\n",
                     mode.c_str());
        return 6;
    }

    llvm::llvm_context context(mode + "_delay_slot_test");
    llvm::module_builder builder(context);
    builder.emit_function_cfg(functions[0], insn_map, function_name);

    if (mode == "bltzall")
    {
        // The Python JIT test reads authoritative runtime byte offsets
        // rather than guessing the layout of a large C++ host structure.
        std::printf(
            "TIMING_OBSERVER_LAYOUT %zu %zu %zu %zu %zu %zu\n",
            sizeof(ee::s_ee_runtime),
            offsetof(ee::s_ee_runtime, m_timing_observer_user),
            offsetof(ee::s_ee_runtime, m_timing_observer),
            offsetof(ee::s_ee_runtime, m_timing_observer_failed),
            offsetof(ee::s_ee_runtime, m_guest_instruction_attempts),
            offsetof(ee::s_ee_runtime, m_reference_instruction_slots));
    }

    if (mode == "loop_yield")
    {
        // The Python JIT fixture allocates the full C++ runtime from these
        // authoritative compile-time offsets, never guessed host padding.
        std::printf("LOOP_RUNTIME_OFFSETS %zu %zu %zu %zu %zu\n",
                    offsetof(ee::s_ee_runtime, m_loop_safepoint_limit),
                    offsetof(ee::s_ee_runtime, m_loop_safepoint_remaining),
                    offsetof(ee::s_ee_runtime, m_guest_instruction_attempts),
                    offsetof(ee::s_ee_runtime, m_loop_safepoint_yield_enabled),
                    offsetof(ee::s_ee_runtime, m_reference_instruction_slots));
    }

    if (!context.write_ir_to_file(argv[2]))
    {
        std::fprintf(stderr, "failed to write LLVM fixture to %s\n", argv[2]);
        return 6;
    }

    return 0;
}
