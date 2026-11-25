#include "function_analyzer.h"
#include "elf_loader.h"
#include "ee_opcode.h"
#include <algorithm>
#include <array>
#include <deque>
#include <set>

namespace ee {

std::set<uint32_t> collect_successors(const s_ee_instruction& ins)
{
    std::set<uint32_t> out;
    if (ins.valid == e_ee_valid::INVALID)
        return out;

    const uint32_t after_delay = ins.pc + (ins.has_delay_slot ? 8u : 4u);
    if (ins.has_delay_slot)
        out.insert(ins.pc + 4u);

    switch (ins.flow)
    {
    case e_ee_flow::FLOW_BRANCH:
    case e_ee_flow::FLOW_BRANCH_LIKELY:
        out.insert(static_cast<uint32_t>(ins.branch_target));
        out.insert(after_delay);
        break;
    case e_ee_flow::FLOW_JUMP:
        // J has an immediate target; JR's register target is dynamic.
        if (ins.opcode == e_ee_opcode::OP_J)
            out.insert(ins.jump_target);
        break;
    case e_ee_flow::FLOW_CALL:
        // JAL has an immediate target; JALR is resolved by the dispatcher.
        if (ins.opcode == e_ee_opcode::OP_JAL)
            out.insert(ins.jump_target);
        out.insert(after_delay);
        break;
    case e_ee_flow::FLOW_RETURN:
        break;
    case e_ee_flow::FLOW_EXCEPTION:
        // SYSCALL/BREAK leave the translated block for the exception handler,
        // but the handler may resume at EPC+4. Discover that continuation so
        // code after a returning exception remains available to the dispatcher.
        out.insert(ins.pc + 4u);
        break;
    case e_ee_flow::FLOW_NONE:
    default:
        out.insert(ins.pc + 4u);
        break;
    }
    return out;
}


namespace
{
struct s_gpr_constant_state
{
    bool m_reachable = false;
    std::array<uint64_t, 32> m_value{};
    std::array<bool, 32> m_known{};
};

void set_gpr_constant(s_gpr_constant_state& state, uint32_t reg, uint64_t value)
{
    if (reg == 0 || reg >= 32)
    {
        return;
    }

    state.m_value[reg] = value;
    state.m_known[reg] = true;
}

void clear_gpr_constant(s_gpr_constant_state& state, uint32_t reg)
{
    if (reg == 0 || reg >= 32)
    {
        return;
    }

    state.m_known[reg] = false;
}

void clear_all_gpr_constants(s_gpr_constant_state& state)
{
    state.m_known.fill(false);
    state.m_known[0] = true;
    state.m_value[0] = 0;
}

void clear_call_clobbered_gprs(s_gpr_constant_state& state)
{
    // R5900/O32 caller-saved temporaries and argument/result registers.
    // A call may change these before control returns to the caller.
    for (uint32_t reg = 1; reg <= 15; ++reg)
        clear_gpr_constant(state, reg);
    for (uint32_t reg = 24; reg <= 27; ++reg)
        clear_gpr_constant(state, reg);
    clear_gpr_constant(state, 31);
}

bool read_gpr_constant(
    const s_gpr_constant_state& state,
    uint32_t reg,
    uint64_t* value
)
{
    if (!value || reg >= 32)
    {
        return false;
    }

    if (reg == 0)
    {
        *value = 0;
        return true;
    }

    if (!state.m_known[reg])
    {
        return false;
    }

    *value = state.m_value[reg];
    return true;
}

uint64_t sign_extend_u32(uint32_t value)
{
    return static_cast<uint64_t>(static_cast<int64_t>(
        static_cast<int32_t>(value)
    ));
}

bool merge_gpr_state(
    s_gpr_constant_state& destination,
    const s_gpr_constant_state& source
)
{
    if (!source.m_reachable)
    {
        return false;
    }

    if (!destination.m_reachable)
    {
        destination = source;
        return true;
    }

    bool changed = false;
    for (uint32_t reg = 1; reg < 32; ++reg)
    {
        if (destination.m_known[reg] &&
            (!source.m_known[reg] ||
             destination.m_value[reg] != source.m_value[reg]))
        {
            destination.m_known[reg] = false;
            changed = true;
        }
    }

    return changed;
}

bool is_special_no_gpr_write(uint8_t funct)
{
    switch (funct)
    {
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_JR):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_SYSCALL):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_BREAK):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_SYNC):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_MTHI):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_MTLO):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_MULT):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_MULTU):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_DIV):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_DIVU):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TGE):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TGEU):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TLT):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TLTU):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TEQ):
    case static_cast<uint8_t>(e_ee_opcode::FUNCT_TNE):
        return true;
    default:
        return false;
    }
}

void invalidate_unmodelled_destination(
    const s_ee_instruction& ins,
    s_gpr_constant_state& state
)
{
    switch (ins.opcode)
    {
    case e_ee_opcode::OP_SPECIAL:
        if (!is_special_no_gpr_write(ins.funct))
        {
            clear_gpr_constant(state, ins.rd);
        }
        return;

    case e_ee_opcode::OP_REGIMM:
        if (ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BLTZAL) ||
            ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BGEZAL) ||
            ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BLTZALL) ||
            ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BGEZALL))
        {
            clear_call_clobbered_gprs(state);
        }
        return;

    case e_ee_opcode::OP_JAL:
        clear_call_clobbered_gprs(state);
        set_gpr_constant(state, 31, sign_extend_u32(ins.pc + 8u));
        return;

    case e_ee_opcode::OP_ADDI:
    case e_ee_opcode::OP_SLTI:
    case e_ee_opcode::OP_SLTIU:
    case e_ee_opcode::OP_ANDI:
    case e_ee_opcode::OP_XORI:
    case e_ee_opcode::OP_LUI:
    case e_ee_opcode::OP_DADDI:
    case e_ee_opcode::OP_DADDIU:
    case e_ee_opcode::OP_LDL:
    case e_ee_opcode::OP_LDR:
    case e_ee_opcode::OP_LQ:
    case e_ee_opcode::OP_LB:
    case e_ee_opcode::OP_LH:
    case e_ee_opcode::OP_LWL:
    case e_ee_opcode::OP_LW:
    case e_ee_opcode::OP_LBU:
    case e_ee_opcode::OP_LHU:
    case e_ee_opcode::OP_LWR:
    case e_ee_opcode::OP_LWU:
    case e_ee_opcode::OP_LD:
        clear_gpr_constant(state, ins.rt);
        return;

    case e_ee_opcode::OP_COP0:
        if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP0_MFC0))
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_COP1:
        if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP1_MFC1) ||
            ins.rs == static_cast<uint8_t>(e_ee_opcode::COP1_CFC1))
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_COP2:
        if (ins.rs == static_cast<uint8_t>(e_ee_opcode::COP2_QMFC2) ||
            ins.rs == static_cast<uint8_t>(e_ee_opcode::COP2_CFC2))
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_MMI:
        // MMI subfamilies use different destination-field conventions.
        // Killing both candidates is conservative and avoids false targets.
        clear_gpr_constant(state, ins.rd);
        clear_gpr_constant(state, ins.rt);
        return;

    default:
        break;
    }

    if (ins.valid != e_ee_valid::VALID)
    {
        // Unknown/invalid semantics may modify any GPR.
        clear_all_gpr_constants(state);
    }
}

void transfer_gpr_constants(
    const s_ee_instruction& ins,
    s_gpr_constant_state& state,
    const elf_loader* memory_image
)
{
    uint64_t source_s = 0;
    uint64_t source_t = 0;
    bool has_s = read_gpr_constant(state, ins.rs, &source_s);
    bool has_t = read_gpr_constant(state, ins.rt, &source_t);
    const uint32_t imm32 = static_cast<uint32_t>(
        static_cast<int32_t>(ins.immediate)
    );

    switch (ins.opcode)
    {
    case e_ee_opcode::OP_LW:
    case e_ee_opcode::OP_LWU:
    {
        uint32_t loaded_value = 0;
        const uint32_t address = static_cast<uint32_t>(source_s) + imm32;
        if (memory_image && has_s && (address & 3u) == 0 &&
            memory_image->read32_const_at(address, &loaded_value))
        {
            const uint64_t value = ins.opcode == e_ee_opcode::OP_LW
                ? sign_extend_u32(loaded_value)
                : static_cast<uint64_t>(loaded_value);
            set_gpr_constant(state, ins.rt, value);
        }
        else
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;
    }

    case e_ee_opcode::OP_LUI:
        set_gpr_constant(
            state, ins.rt,
            sign_extend_u32(static_cast<uint32_t>(ins.uimmediate) << 16)
        );
        return;

    case e_ee_opcode::OP_ORI:
        if (has_s)
        {
            set_gpr_constant(state, ins.rt, source_s | ins.uimmediate);
        }
        else
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_ANDI:
        if (has_s)
        {
            set_gpr_constant(state, ins.rt, source_s & ins.uimmediate);
        }
        else
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_XORI:
        if (has_s)
        {
            set_gpr_constant(state, ins.rt, source_s ^ ins.uimmediate);
        }
        else
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_ADDIU:
        if (has_s)
        {
            const uint32_t result =
                static_cast<uint32_t>(source_s) + imm32;
            set_gpr_constant(state, ins.rt, sign_extend_u32(result));
        }
        else
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_DADDIU:
        if (has_s)
        {
            set_gpr_constant(
                state, ins.rt,
                source_s + static_cast<uint64_t>(
                    static_cast<int64_t>(ins.immediate)
                )
            );
        }
        else
        {
            clear_gpr_constant(state, ins.rt);
        }
        return;

    case e_ee_opcode::OP_SPECIAL:
        break;

    default:
        invalidate_unmodelled_destination(ins, state);
        return;
    }

    using op = e_ee_opcode;
    switch (ins.funct)
    {
    case static_cast<uint8_t>(op::FUNCT_ADDU):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd,
                sign_extend_u32(static_cast<uint32_t>(source_s + source_t)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DADDU):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_s + source_t);
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_SUBU):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd,
                sign_extend_u32(static_cast<uint32_t>(source_s - source_t)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSUBU):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_s - source_t);
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_AND):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_s & source_t);
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_OR):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_s | source_t);
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_XOR):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_s ^ source_t);
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_NOR):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, ~(source_s | source_t));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_SLL):
        if (has_t)
            set_gpr_constant(state, ins.rd, sign_extend_u32(
                static_cast<uint32_t>(source_t) << ins.sa));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_SRL):
        if (has_t)
            set_gpr_constant(state, ins.rd, sign_extend_u32(
                static_cast<uint32_t>(source_t) >> ins.sa));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_SRA):
        if (has_t)
            set_gpr_constant(state, ins.rd, sign_extend_u32(
                static_cast<uint32_t>(
                    static_cast<int32_t>(source_t) >> ins.sa)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSLL):
    case static_cast<uint8_t>(op::FUNCT_DSLL32):
        if (has_t)
            set_gpr_constant(state, ins.rd,
                source_t << (ins.sa + (ins.funct == static_cast<uint8_t>(op::FUNCT_DSLL32) ? 32u : 0u)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSRL):
    case static_cast<uint8_t>(op::FUNCT_DSRL32):
        if (has_t)
            set_gpr_constant(state, ins.rd,
                source_t >> (ins.sa + (ins.funct == static_cast<uint8_t>(op::FUNCT_DSRL32) ? 32u : 0u)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSRA):
    case static_cast<uint8_t>(op::FUNCT_DSRA32):
        if (has_t)
            set_gpr_constant(state, ins.rd, static_cast<uint64_t>(
                static_cast<int64_t>(source_t) >>
                (ins.sa + (ins.funct == static_cast<uint8_t>(op::FUNCT_DSRA32) ? 32u : 0u))));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSLLV):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_t << (source_s & 0x3fu));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSRLV):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, source_t >> (source_s & 0x3fu));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_DSRAV):
        if (has_s && has_t)
            set_gpr_constant(state, ins.rd, static_cast<uint64_t>(
                static_cast<int64_t>(source_t) >> (source_s & 0x3fu)));
        else clear_gpr_constant(state, ins.rd);
        return;

    case static_cast<uint8_t>(op::FUNCT_JALR):
        clear_call_clobbered_gprs(state);
        if (ins.rd == 31)
        {
            set_gpr_constant(state, 31, sign_extend_u32(ins.pc + 8u));
        }
        else
        {
            clear_gpr_constant(state, ins.rd);
        }
        return;

    default:
        invalidate_unmodelled_destination(ins, state);
        return;
    }
}

void transfer_block_constants(
    const s_ee_basic_block& block,
    const std::map<uint32_t, s_ee_instruction>& insns,
    s_gpr_constant_state& state,
    const elf_loader* memory_image = nullptr,
    size_t instruction_count = static_cast<size_t>(-1)
)
{
    const size_t count = std::min(instruction_count, block.insn_indices.size());
    for (size_t i = 0; i < count; ++i)
    {
        const size_t pc_index = block.insn_indices[i];
        auto it = insns.find(static_cast<uint32_t>(pc_index));
        if (it == insns.end())
        {
            clear_all_gpr_constants(state);
            continue;
        }

        transfer_gpr_constants(it->second, state, memory_image);
    }

    state.m_known[0] = true;
    state.m_value[0] = 0;
}

std::set<uint32_t> collect_jalr_targets_in_block(
    const s_ee_basic_block& block,
    const std::map<uint32_t, s_ee_instruction>& insns,
    s_gpr_constant_state state,
    uint32_t caller_entry,
    std::vector<s_static_jalr_site>* site_audit,
    const elf_loader* memory_image
)
{
    std::set<uint32_t> targets;
    for (size_t pc_index : block.insn_indices)
    {
        auto it = insns.find(static_cast<uint32_t>(pc_index));
        if (it == insns.end())
        {
            clear_all_gpr_constants(state);
            continue;
        }

        const s_ee_instruction& ins = it->second;
        if (ins.valid == e_ee_valid::VALID &&
            ins.opcode == e_ee_opcode::OP_SPECIAL &&
            ins.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_JALR))
        {
            uint64_t target_value = 0;
            const bool source_known =
                read_gpr_constant(state, ins.rs, &target_value);
            const uint32_t target = static_cast<uint32_t>(target_value);
            const bool target_aligned =
                source_known && target != 0 && (target & 3u) == 0;

            if (site_audit != nullptr)
            {
                s_static_jalr_site site{};
                site.m_caller_entry = caller_entry;
                site.m_pc = ins.pc;
                site.m_source_register = ins.rs;
                site.m_source_known = source_known;
                site.m_source_value = target_value;
                site.m_target = target;
                site.m_target_aligned = target_aligned;
                site_audit->push_back(site);
            }

            if (target_aligned)
            {
                targets.insert(target);
            }
        }

        transfer_gpr_constants(ins, state, memory_image);
    }
    return targets;
}

bool is_control_transfer(const s_ee_instruction& ins)
{
    return ins.flow != e_ee_flow::FLOW_NONE;
}

bool is_unconditional(const s_ee_instruction& ins)
{
    return ins.flow == e_ee_flow::FLOW_JUMP ||
           ins.flow == e_ee_flow::FLOW_CALL ||
           ins.flow == e_ee_flow::FLOW_RETURN ||
           ins.flow == e_ee_flow::FLOW_EXCEPTION;
}

bool is_conditional(const s_ee_instruction& ins)
{
    return ins.flow == e_ee_flow::FLOW_BRANCH ||
           ins.flow == e_ee_flow::FLOW_BRANCH_LIKELY;
}

bool is_return(const s_ee_instruction& ins)
{
    return ins.flow == e_ee_flow::FLOW_RETURN;
}
} // namespace

std::set<uint32_t> collect_function_block_starts(
    const std::map<uint32_t, s_ee_instruction>& insns,
    const std::vector<uint32_t>& all_entries)
{
    std::set<uint32_t> block_starts(all_entries.begin(), all_entries.end());
    for (const auto& [instruction_pc, instruction] : insns)
    {
        (void)instruction_pc;
        if (instruction.flow == e_ee_flow::FLOW_BRANCH ||
            instruction.flow == e_ee_flow::FLOW_BRANCH_LIKELY)
        {
            block_starts.insert(static_cast<uint32_t>(instruction.branch_target));
        }
        else if (instruction.flow == e_ee_flow::FLOW_JUMP &&
                 instruction.opcode == e_ee_opcode::OP_J)
        {
            block_starts.insert(instruction.jump_target);
        }
    }
    return block_starts;
}

std::vector<s_ee_function> function_analyzer::analyze(
    const std::map<uint32_t, s_ee_instruction>& insns,
    const std::vector<uint32_t>& entry_points,
    const std::vector<uint32_t>& all_entries,
    const std::set<uint32_t>* block_starts,
    const std::set<uint32_t>* stop_entries)
{
    // Thousands of function analyses may share the same stop-at-entry
    // boundary. Avoid allocating and filling an identical set for every
    // single-function CFG in the full-image coverage and IR passes.
    const std::set<uint32_t> local_stop_entries = stop_entries
        ? std::set<uint32_t>{}
        : std::set<uint32_t>(all_entries.begin(), all_entries.end());
    const std::set<uint32_t>& indexed_stop_entries = stop_entries
        ? *stop_entries : local_stop_entries;
    const std::set<uint32_t> local_block_starts = block_starts
        ? std::set<uint32_t>{}
        : collect_function_block_starts(insns, all_entries);
    const std::set<uint32_t>& indexed_block_starts = block_starts
        ? *block_starts : local_block_starts;

    std::vector<s_ee_function> functions;
    for (uint32_t ep : entry_points)
    {
        s_ee_function fn;
        fn.entry_pc = ep;
        split_into_blocks(insns, ep, fn, indexed_stop_entries, indexed_block_starts);
        link_cfg(fn);
        functions.push_back(std::move(fn));
    }
    return functions;
}

std::set<uint32_t> collect_static_jalr_targets(
    const std::map<uint32_t, s_ee_instruction>& insns,
    const std::vector<uint32_t>& entry_points,
    const std::vector<uint32_t>& all_entries,
    const std::set<uint32_t>* block_starts,
    std::vector<s_static_jalr_site>* site_audit,
    const elf_loader* memory_image
)
{
    std::set<uint32_t> targets;
    const std::set<uint32_t> local_block_starts = block_starts
        ? std::set<uint32_t>{}
        : collect_function_block_starts(insns, all_entries);
    const std::set<uint32_t>& indexed_block_starts = block_starts
        ? *block_starts : local_block_starts;
    function_analyzer analyzer;
    // JALR fixed-point discovery analyzes every known entry. All CFGs in
    // this invocation use identical function boundaries.
    const std::set<uint32_t> stop_entries(
        all_entries.begin(), all_entries.end()
    );

    auto states_equal = [](const s_gpr_constant_state& a,
                           const s_gpr_constant_state& b) -> bool
    {
        return a.m_reachable == b.m_reachable &&
               a.m_known == b.m_known &&
               a.m_value == b.m_value;
    };

    for (uint32_t entry : entry_points)
    {
        const std::vector<s_ee_function> functions =
            analyzer.analyze(
                insns, {entry}, all_entries, &indexed_block_starts,
                &stop_entries
            );
        if (functions.empty() || functions[0].blocks.empty())
        {
            continue;
        }

        const s_ee_function& fn = functions[0];
        std::map<uint32_t, const s_ee_basic_block*> blocks;
        for (const auto& block : fn.blocks)
        {
            blocks[block.start_pc] = &block;
        }

        if (blocks.find(entry) == blocks.end())
        {
            continue;
        }

        std::map<uint32_t, s_gpr_constant_state> incoming;
        std::map<uint32_t, s_gpr_constant_state> outgoing;
        std::map<uint32_t, s_gpr_constant_state> outgoing_fallthrough;
        std::deque<uint32_t> worklist;
        std::set<uint32_t> queued;

        s_gpr_constant_state entry_state{};
        entry_state.m_reachable = true;
        entry_state.m_known[0] = true;
        entry_state.m_value[0] = 0;
        incoming[entry] = entry_state;
        worklist.push_back(entry);
        queued.insert(entry);

        // Forward data-flow over the function CFG. The lattice only moves
        // from a known constant to unknown at a join, so this converges even
        // in the presence of loops and recursive-looking local control flow.
        while (!worklist.empty())
        {
            const uint32_t block_pc = worklist.front();
            worklist.pop_front();
            queued.erase(block_pc);

            const auto block_it = blocks.find(block_pc);
            const auto in_it = incoming.find(block_pc);
            if (block_it == blocks.end() ||
                in_it == incoming.end() ||
                !in_it->second.m_reachable)
            {
                continue;
            }

            const s_ee_basic_block& block = *block_it->second;
            s_gpr_constant_state out_state = in_it->second;
            transfer_block_constants(block, insns, out_state, memory_image);
            out_state.m_reachable = true;

            const bool annul_delay_on_fallthrough =
                block.branch_likely && block.has_delay_slot &&
                block.insn_indices.size() > 1;
            s_gpr_constant_state out_fallthrough = out_state;
            if (annul_delay_on_fallthrough)
            {
                out_fallthrough = in_it->second;
                // The final listed instruction is the delay slot. It executes
                // only on the taken path for a branch-likely instruction.
                transfer_block_constants(
                    block, insns, out_fallthrough, memory_image,
                    block.insn_indices.size() - 1u
                );
                out_fallthrough.m_reachable = true;
            }

            const auto old_out = outgoing.find(block_pc);
            const bool out_changed =
                old_out == outgoing.end() ||
                !states_equal(old_out->second, out_state);
            const auto old_fallthrough = outgoing_fallthrough.find(block_pc);
            const bool fallthrough_changed =
                annul_delay_on_fallthrough &&
                (old_fallthrough == outgoing_fallthrough.end() ||
                 !states_equal(old_fallthrough->second, out_fallthrough));
            if (!out_changed && !fallthrough_changed)
            {
                continue;
            }
            outgoing[block_pc] = out_state;
            if (annul_delay_on_fallthrough)
            {
                outgoing_fallthrough[block_pc] = out_fallthrough;
            }

            for (uint32_t successor : block.successors)
            {
                if (blocks.find(successor) == blocks.end())
                {
                    continue;
                }

                bool successor_changed = false;
                const bool is_fallthrough =
                    annul_delay_on_fallthrough &&
                    successor == block.fallthrough_pc;
                const bool is_taken_target =
                    annul_delay_on_fallthrough &&
                    successor == block.target_pc;

                if (is_fallthrough)
                {
                    successor_changed |= merge_gpr_state(
                        incoming[successor], out_fallthrough
                    );
                }

                if (!is_fallthrough || is_taken_target)
                {
                    successor_changed |= merge_gpr_state(
                        incoming[successor], out_state
                    );
                }

                if (successor_changed && queued.insert(successor).second)
                {
                    worklist.push_back(successor);
                }
            }
        }

        // Collect only after the data-flow fixpoint. This avoids reporting a
        // target seen on one path before a later predecessor disproves it.
        for (const auto& block : fn.blocks)
        {
            const auto in_it = incoming.find(block.start_pc);
            if (in_it == incoming.end() || !in_it->second.m_reachable)
            {
                continue;
            }

            const std::set<uint32_t> block_targets =
                collect_jalr_targets_in_block(
                    block, insns, in_it->second, entry, site_audit,
                    memory_image
                );
            targets.insert(block_targets.begin(), block_targets.end());
        }
    }

    return targets;
}

void function_analyzer::split_into_blocks(
    const std::map<uint32_t, s_ee_instruction>& insns,
    uint32_t entry_pc,
    s_ee_function& fn,
    const std::set<uint32_t>& stop_entries,
    const std::set<uint32_t>& block_starts)
{
    // Another function's entry: do not absorb its body into this CFG.
    auto is_foreign_entry = [&](uint32_t pc) -> bool {
        return pc != entry_pc && stop_entries.count(pc) != 0;
    };

    // All decoded branch/jump leaders are supplied by the shared index built
    // once for this instruction-map revision. Keeping that work out of this
    // per-function routine avoids scanning the whole image for every entry.
    std::set<uint32_t> visited;
    std::vector<uint32_t> worklist;
    worklist.push_back(entry_pc);

    while (!worklist.empty())
    {
        uint32_t cur = worklist.back();
        worklist.pop_back();
        if (!visited.insert(cur).second) continue;
        // Never start a block at another function's entry.
        if (is_foreign_entry(cur)) continue;

        // A control-flow target may be discovered after a linear scan has
        // already absorbed it into an existing block. Split that block now,
        // so the target has a unique entry and every predecessor is linked to
        // the same data-flow node.
        auto containing_block = std::find_if(
            fn.blocks.begin(), fn.blocks.end(),
            [&](const s_ee_basic_block& block) {
                return cur > block.start_pc && cur < block.end_pc;
            }
        );
        if (containing_block != fn.blocks.end())
        {
            const auto split_instruction = std::find(
                containing_block->insn_indices.begin(),
                containing_block->insn_indices.end(),
                static_cast<size_t>(cur)
            );
            if (split_instruction != containing_block->insn_indices.end())
            {
                const size_t split_offset = static_cast<size_t>(
                    split_instruction - containing_block->insn_indices.begin()
                );
                s_ee_basic_block tail = *containing_block;
                tail.start_pc = cur;
                tail.insn_indices.erase(
                    tail.insn_indices.begin(),
                    tail.insn_indices.begin() + split_offset
                );
                tail.predecessors.clear();

                containing_block->insn_indices.erase(
                    containing_block->insn_indices.begin() + split_offset,
                    containing_block->insn_indices.end()
                );
                containing_block->end_pc = cur;
                containing_block->fallthrough_pc = cur;
                containing_block->target_pc = 0;
                containing_block->has_delay_slot = false;
                containing_block->branch_likely = false;
                containing_block->is_terminator = false;
                containing_block->successors.clear();
                containing_block->successors.insert(cur);

                fn.blocks.push_back(std::move(tail));
                continue;
            }
        }

        auto it = insns.find(cur);
        if (it == insns.end()) continue;

        s_ee_basic_block bb;
        bb.start_pc = cur;
        uint32_t pc = cur;

        while (true)
        {
            auto iit = insns.find(pc);
            if (iit == insns.end()) break;

            // Stop *before* consuming a foreign function entry mid-block.
            if (pc != bb.start_pc && is_foreign_entry(pc))
            {
                bb.end_pc = pc;
                bb.fallthrough_pc = pc;
                break;
            }

            // Do not absorb a known branch/jump target into this linear
            // scan, even when its block has not yet been visited. It may be
            // discovered later in worklist order; all such leaders were
            // collected from the decoded image before block construction.
            if (pc != bb.start_pc && block_starts.count(pc) != 0)
            {
                bb.end_pc = pc;
                bb.fallthrough_pc = pc;
                bb.successors.insert(pc);
                worklist.push_back(pc);
                break;
            }

            const s_ee_instruction& ins = iit->second;
            bb.insn_indices.push_back(pc); // store pc as index key for now

            bb.end_pc = pc + 4;
            bb.fallthrough_pc = pc + 4;

            if (is_control_transfer(ins))
            {
                bb.has_delay_slot = ins.has_delay_slot;
                bb.branch_likely = ins.branch_likely;

                uint32_t delay_pc = pc + 4;

                if (ins.has_delay_slot)
                {
                    // The delay-slot instruction belongs to this block and
                    // executes before the control transfer takes effect.
                    // Do not pull a foreign entry into the delay slot.
                    auto dit = insns.find(delay_pc);
                    if (dit != insns.end() && !is_foreign_entry(delay_pc))
                    {
                        bb.insn_indices.push_back(delay_pc);
                        bb.end_pc = delay_pc + 4;
                        bb.fallthrough_pc = delay_pc + 4;
                    }
                    else
                    {
                        bb.end_pc = delay_pc + 4;
                        bb.fallthrough_pc = delay_pc + 4;
                    }
                }
                else
                {
                    bb.end_pc = pc + 4;
                    bb.fallthrough_pc = pc + 4;
                }

                if (is_conditional(ins))
                {
                    // A conditional branch occupying another conditional
                    // branch's slot can select its own target. Keep that
                    // guest address discoverable, including for COP1.
                    const auto nested = insns.find(delay_pc);
                    if (ins.has_delay_slot && nested != insns.end() &&
                        nested->second.has_delay_slot &&
                        is_conditional(nested->second))
                    {
                        const uint32_t nested_target = nested->second.branch_target;
                        if (!is_foreign_entry(nested_target))
                        {
                            bb.successors.insert(nested_target);
                            worklist.push_back(nested_target);
                        }
                    }
                    bb.target_pc = ins.branch_target;
                    // Fallthrough: only if it is not another function entry.
                    if (!is_foreign_entry(bb.fallthrough_pc))
                    {
                        bb.successors.insert(bb.fallthrough_pc);
                        worklist.push_back(bb.fallthrough_pc);
                    }
                    // Taken target: follow only if still inside this function.
                    if (!is_foreign_entry(ins.branch_target))
                    {
                        bb.successors.insert(ins.branch_target);
                        worklist.push_back(ins.branch_target);
                    }
                }
                else if (is_unconditional(ins))
                {
                    if (ins.flow == e_ee_flow::FLOW_EXCEPTION)
                    {
                        // The exception handler chooses the vector and the
                        // current translated block exits to the host. It may
                        // later resume at EPC+4, so retain that PC as a
                        // separately dispatchable continuation block.
                        bb.is_terminator = true;
                        if (!is_foreign_entry(bb.fallthrough_pc))
                        {
                            bb.successors.insert(bb.fallthrough_pc);
                            worklist.push_back(bb.fallthrough_pc);
                        }
                    }
                    else if (ins.flow == e_ee_flow::FLOW_CALL)
                    {
                        // STOP-AT-CALL: callee body is not part of this CFG.
                        // Only the return path (fallthrough after delay) continues.
                        bb.target_pc = ins.jump_target;
                        if (!is_foreign_entry(bb.fallthrough_pc))
                        {
                            bb.successors.insert(bb.fallthrough_pc);
                            worklist.push_back(bb.fallthrough_pc);
                        }
                        // Intentionally do NOT push jump_target.
                    }
                    else if (is_return(ins))
                    {
                        bb.is_terminator = true;
                    }
                    else
                    {
                        // Pure jump (J / JR-as-jump): follow target if in-function.
                        bb.target_pc = ins.jump_target;
                        if (ins.opcode == e_ee_opcode::OP_J &&
                            !is_foreign_entry(ins.jump_target))
                        {
                            bb.successors.insert(ins.jump_target);
                            worklist.push_back(ins.jump_target);
                        }
                        else
                        {
                            // Tail-call / jump into another function → end.
                            bb.is_terminator = true;
                        }
                    }
                }
                break; // block ends at control transfer (after delay slot)
            }

            bb.end_pc = pc + 4;
            bb.fallthrough_pc = pc + 4;
            pc += 4;

            // About to walk into another function via linear fallthrough.
            if (is_foreign_entry(pc))
                break;
        }

        fn.blocks.push_back(std::move(bb));
    }

    // Order blocks by start_pc for deterministic output.
    std::sort(fn.blocks.begin(), fn.blocks.end(),
        [](const s_ee_basic_block& a, const s_ee_basic_block& b) {
            return a.start_pc < b.start_pc;
        });
}

void function_analyzer::link_cfg(s_ee_function& fn)
{
    // Map pc -> block index for edge linking.
    std::map<uint32_t, size_t> pc_to_block;
    for (size_t i = 0; i < fn.blocks.size(); ++i)
        pc_to_block[fn.blocks[i].start_pc] = i;

    for (auto& bb : fn.blocks)
    {
        for (uint32_t succ : bb.successors)
        {
            auto it = pc_to_block.find(succ);
            if (it != pc_to_block.end())
            {
                fn.blocks[it->second].predecessors.insert(bb.start_pc);
            }
        }
    }
}

} // namespace ee
