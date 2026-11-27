#pragma once

#include "ee_strict_rom0.h"

namespace ee
{
// Minimal isolated reset-prefix experiment. These are only the COP0
// reset values relevant to the supported initial instruction (EE Core
// User's Manual v6.0, 4.2.1). Unspecified state is initialized to zero
// for reproducibility, not asserted as hardware power-on contents.
// No cache, device, counter clock or live runtime reset is implied.
inline constexpr s_ee_cpu_state ee_make_strict_reset_prefix_state()
{
    s_ee_cpu_state cpu{};
    cpu.m_pc = 0xbfc00000u;
    cpu.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);
    cpu.m_cop0[COP0_Status] = STATUS_ERL | STATUS_BEV;
    cpu.m_cop0[COP0_Random] = 47u;
    // Reference-compatible R5900 model/revision (PCSX2 cpuReset).
    // Not a claim that all physical EE revisions have this low byte.
    cpu.m_cop0[COP0_PRId] = 0x00002e20u;
    return cpu;
}

enum class e_ee_reset_prefix_result : uint8_t
{
    COMMITTED,
    UNSAFE_BOUNDARY,
    FETCH_UNAVAILABLE,
    UNSUPPORTED_OPCODE,
    BUDGET_EXHAUSTED,
    DATA_UNAVAILABLE
};

struct s_ee_reset_prefix_step
{
    e_ee_reset_prefix_result m_result = e_ee_reset_prefix_result::UNSAFE_BOUNDARY;
    uint32_t m_opcode = 0u;
    uint32_t m_retired = 0u;
    // On refusal, identify the actual unsupported instruction or slot,
    // not just the branch restart PC. Data address is meaningful only
    // for DATA_UNAVAILABLE.
    uint32_t m_detail_pc = 0u;
    uint32_t m_data_address = 0u;
};

// Only the reset-entry MTC0 zero,Status is supported. Its source is
// architectural zero, so no undefined power-on GPR is consumed.
// Other ERL instructions remain rejected by the ordinary executors.
// Rejection leaves the CPU unchanged. The caller owns isolated state;
// this function does not emulate COP0 pipeline hazards or elapsed cycles.
inline constexpr s_ee_reset_prefix_step ee_execute_strict_reset_prefix(
    s_ee_cpu_state& cpu, const s_ee_strict_rom0_view& rom)
{
    using result = e_ee_reset_prefix_result;
    if (cpu.m_pc != 0xbfc00000u ||
        cpu.m_exit_reason != static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH) ||
        cpu.m_cop0[COP0_Status] != (STATUS_ERL | STATUS_BEV) ||
        cpu.m_cop0[COP0_Config] != 0u ||
        cpu.m_cop0[COP0_Random] != 47u ||
        cpu.m_cop0[COP0_Wired] != 0u)
    {
        return {result::UNSAFE_BOUNDARY};
    }
    const c_ee_tlb tlb{};
    const auto fetch = ee_plan_instruction_fetch(tlb, cpu.m_pc,
        cpu.m_cop0[COP0_Status], cpu.m_cop0[COP0_Config],
        cpu.m_cop0[COP0_Context], cpu.m_cop0[COP0_EntryHi]);
    const uint8_t* code = rom.fetch_span(fetch);
    if (code == nullptr)
    {
        return {result::FETCH_UNAVAILABLE};
    }
    const uint32_t opcode = ee_read_instruction_word(code);
    if (opcode != 0x40806000u)
    {
        return {result::UNSUPPORTED_OPCODE, opcode};
    }
    // Architectural effect of the observed MTC0 zero,Status.
    cpu.m_cop0[COP0_Status] = 0u;
    cpu.m_pc += 4u;
    return {result::COMMITTED, opcode, 1u};
}

namespace strict_reset_detail
{
inline constexpr uint64_t read_gpr(const s_ee_cpu_state& cpu, uint32_t reg)
{
    return reg == 0u ? 0u : cpu.m_gpr[reg].m_lo;
}

inline constexpr uint64_t sign_extend_word(uint32_t word)
{
    return (word & 0x80000000u) != 0u
        ? uint64_t{word} | 0xffffffff00000000ull : uint64_t{word};
}

// No memory, branches, clocks or side-effectful COP0 registers.
inline constexpr bool execute_register_word(s_ee_cpu_state& cpu, uint32_t opcode)
{
    const uint32_t primary = opcode >> 26u;
    const uint32_t rs = (opcode >> 21u) & 31u;
    const uint32_t rt = (opcode >> 16u) & 31u;
    uint32_t dest = rt;
    const uint32_t imm = opcode & 0xffffu;
    uint64_t value = 0u;
    switch (primary)
    {
    case 0u:
        if (opcode == 0u)
        {
            return true;
        }
        dest = (opcode >> 11u) & 31u;
        switch (opcode & 0x7ffu) // includes shamt: reject reserved encodings
        {
        case 0x21u: // ADDU: low 32-bit wrapping sum
            value = sign_extend_word(static_cast<uint32_t>(read_gpr(cpu, rs)) +
                                     static_cast<uint32_t>(read_gpr(cpu, rt)));
            break;
        case 0x24u: // AND: both low 64-bit operands
            value = read_gpr(cpu, rs) & read_gpr(cpu, rt);
            break;
        case 0x27u: // NOR: both low 64-bit operands
            value = ~(read_gpr(cpu, rs) | read_gpr(cpu, rt));
            break;
        default:
            return false;
        }
        break;
    case 0x10u:
        if ((opcode & 0xffe0ffffu) != 0x40007800u)
        {
            return false;
        }
        value = sign_extend_word(cpu.m_cop0[COP0_PRId]);
        break;
    case 0x0cu: // ANDI
        value = read_gpr(cpu, rs) & imm;
        break;
    case 0x0du: // ORI
        value = read_gpr(cpu, rs) | imm;
        break;
    case 0x0fu: // LUI
        if (rs != 0u)
        {
            return false;
        }
        value = sign_extend_word(imm << 16u);
        break;
    case 0x09u: // ADDIU (32-bit wrapping, sign-extended result)
        value = sign_extend_word(static_cast<uint32_t>(read_gpr(cpu, rs)) +
            (imm | ((imm & 0x8000u) != 0u ? 0xffff0000u : 0u)));
        break;
    default:
        return false;
    }
    if (dest != 0u)
    {
        cpu.m_gpr[dest].m_lo = value;
    }
    return true;
}

// LBU/LW must validate even when the destination is zero. A refusal
// is diagnostic only: this ROM probe does not deliver guest faults.
inline constexpr s_ee_reset_prefix_step execute_rom_word(
    s_ee_cpu_state& cpu, const s_ee_strict_rom0_view& rom,
    const c_ee_tlb& tlb, uint32_t pc, uint32_t opcode)
{
    using result = e_ee_reset_prefix_result;
    const uint32_t primary = opcode >> 26u;
    if (primary != 0x24u && primary != 0x23u)
    {
        return {execute_register_word(cpu, opcode) ? result::COMMITTED :
            result::UNSUPPORTED_OPCODE, opcode, 0u, pc};
    }
    const uint32_t rs = (opcode >> 21u) & 31u;
    const uint32_t rt = (opcode >> 16u) & 31u;
    const uint32_t imm = opcode & 0xffffu;
    const uint32_t address = static_cast<uint32_t>(read_gpr(cpu, rs)) +
        (imm | ((imm & 0x8000u) != 0u ? 0xffff0000u : 0u));
    const auto kind = primary == 0x23u
        ? e_ee_data_access_kind::WORD : e_ee_data_access_kind::BYTE;
    const auto plan = ee_plan_data_access(tlb, kind,
        false, address, cpu.m_cop0[COP0_Status], cpu.m_cop0[COP0_Config],
        cpu.m_cop0[COP0_Context], cpu.m_cop0[COP0_EntryHi]);
    const auto bus = ee_plan_data_bus_span(plan.m_access, plan.m_width,
        cpu.m_cop0[COP0_Config], 0u);
    const uint8_t* bytes = plan.m_fault.m_valid ? nullptr : rom.read_span(bus);
    if (bytes == nullptr)
    {
        return {result::DATA_UNAVAILABLE, opcode, 0u, pc, address};
    }
    if (rt != 0u)
    {
        cpu.m_gpr[rt].m_lo = primary == 0x23u
            ? sign_extend_word(ee_read_instruction_word(bytes))
            : static_cast<uint64_t>(*bytes);
    }
    return {result::COMMITTED, opcode, 0u, pc};
}
} // namespace strict_reset_detail

// Isolated post-Status-clear ROM-only execution. The caller owns all
// state and establishes the absence of pending clock/device effects.
// BEQ/BNE and their supported register/ROM-read slots are a single
// bounded transaction: an unavailable/unsupported slot leaves the
// branch PC and GPRs unchanged. No guest fault is invented by refusal.
inline constexpr s_ee_reset_prefix_step ee_execute_strict_rom_register_step(
    s_ee_cpu_state& cpu, const s_ee_strict_rom0_view& rom, uint32_t budget)
{
    using result = e_ee_reset_prefix_result;
    if (cpu.m_exit_reason != static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH) ||
        cpu.m_cop0[COP0_Status] != 0u || cpu.m_pc < 0xbfc00000u ||
        cpu.m_pc >= 0xc0000000u)
    {
        return {result::UNSAFE_BOUNDARY};
    }
    if (budget == 0u)
    {
        return {result::BUDGET_EXHAUSTED};
    }
    const c_ee_tlb tlb{};
    const auto fetch = [&](uint32_t pc)
    {
        return rom.fetch_span(ee_plan_instruction_fetch(tlb, pc,
            cpu.m_cop0[COP0_Status], cpu.m_cop0[COP0_Config],
            cpu.m_cop0[COP0_Context], cpu.m_cop0[COP0_EntryHi]));
    };
    const uint8_t* code = fetch(cpu.m_pc);
    if (code == nullptr)
    {
        return {result::FETCH_UNAVAILABLE, 0u, 0u, cpu.m_pc};
    }
    const uint32_t opcode = ee_read_instruction_word(code);
    const uint32_t primary = opcode >> 26u;
    if (primary == 4u || primary == 5u)
    {
        if (budget < 2u)
        {
            return {result::BUDGET_EXHAUSTED, opcode};
        }
        const uint32_t pc = cpu.m_pc;
        const bool equal = strict_reset_detail::read_gpr(cpu, (opcode >> 21u) & 31u) ==
            strict_reset_detail::read_gpr(cpu, (opcode >> 16u) & 31u);
        const bool taken = primary == 4u ? equal : !equal;
        const uint32_t imm = opcode & 0xffffu;
        const uint32_t disp = (imm | ((imm & 0x8000u) != 0u ? 0xffff0000u : 0u)) << 2u;
        const uint8_t* slot = fetch(pc + 4u);
        if (slot == nullptr)
        {
            return {result::FETCH_UNAVAILABLE, 0u, 0u, pc + 4u};
        }
        const auto action = strict_reset_detail::execute_rom_word(cpu, rom, tlb,
            pc + 4u, ee_read_instruction_word(slot));
        if (action.m_result != result::COMMITTED)
        {
            return action;
        }
        cpu.m_pc = taken ? pc + 4u + disp : pc + 8u;
        return {result::COMMITTED, opcode, 2u};
    }
    const auto action = strict_reset_detail::execute_rom_word(cpu, rom, tlb,
        cpu.m_pc, opcode);
    if (action.m_result != result::COMMITTED)
    {
        return action;
    }
    cpu.m_pc += 4u;
    return {result::COMMITTED, opcode, 1u};
}

static_assert(strict_reset_detail::sign_extend_word(0xbfc0012cu) == 0xffffffffbfc0012cull);
static_assert(ee_make_strict_reset_prefix_state().m_cop0[COP0_PRId] == 0x2e20u);
static_assert(ee_make_strict_reset_prefix_state().m_pc == 0xbfc00000u);
static_assert(ee_make_strict_reset_prefix_state().m_cop0[COP0_Status] ==
              (STATUS_ERL | STATUS_BEV));
static_assert(ee_make_strict_reset_prefix_state().m_cop0[COP0_Random] == 47u);

} // namespace ee
