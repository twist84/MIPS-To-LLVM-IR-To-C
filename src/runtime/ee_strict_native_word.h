#pragma once

#include "ee_ram_interpreter.h"

namespace ee
{
// Separate experimental ABI: not s_ee_runtime or ee_entry. The caller
// owns the objects and the isolated execution boundary for the entire
// native call, including the transfer after successful preparation.
struct s_ee_strict_native_word_context
{
    ee_memory* m_memory = nullptr;
    bool* m_actual_delay_slot = nullptr;
    s_ee_strict_ram_evidence m_evidence{};
};

inline constexpr bool ee_strict_word_supported(uint32_t opcode)
{
    return (opcode >> 26u) == 0x23u || (opcode >> 26u) == 0x2bu;
}

struct s_ee_strict_transfer
{
    uint32_t m_width = 0u;
    bool m_store = false;
    bool m_signed_load = false;
    bool m_partial = false;
    bool m_left = false;
};

// Only ordinary integer GPR memory instructions, not COP1/COP2 or
// cache operations. Shared by preparation and compile-time lowering.
inline constexpr s_ee_strict_transfer ee_decode_strict_transfer(uint32_t opcode)
{
    switch (opcode >> 26u)
    {
    case 0x20u: return {1u, false, true};
    case 0x24u: return {1u};
    case 0x21u: return {2u, false, true};
    case 0x25u: return {2u};
    case 0x23u: return {4u, false, true};
    case 0x27u: return {4u};
    case 0x37u: return {8u};
    case 0x28u: return {1u, true};
    case 0x29u: return {2u, true};
    case 0x2bu: return {4u, true};
    case 0x3fu: return {8u, true};
    case 0x1eu: return {16u};
    case 0x1fu: return {16u, true};
    case 0x22u: return {4u, false, false, true, true};
    case 0x26u: return {4u, false, false, true, false};
    case 0x1au: return {8u, false, false, true, true};
    case 0x1bu: return {8u, false, false, true, false};
    case 0x2au: return {4u, true, false, true, true};
    case 0x2eu: return {4u, true, false, true, false};
    case 0x2cu: return {8u, true, false, true, true};
    case 0x2du: return {8u, true, false, true, false};
    default: return {};
    }
}

static_assert(ee_decode_strict_transfer(0x1eu << 26u).m_width == 16u);
static_assert(ee_decode_strict_transfer(0x2du << 26u).m_store &&
              ee_decode_strict_transfer(0x2du << 26u).m_partial);
static_assert(ee_decode_strict_transfer(0u).m_width == 0u);

// Integer equality branches and restartable direct/indirect jumps.
// Sony EE instruction manual pp.65–68: JALR rs == rd is undefined.
// Reserved fields in JR/JALR are not silently accepted.
inline constexpr bool ee_strict_branch_supported(uint32_t opcode)
{
    const uint32_t primary = opcode >> 26u;
    if (primary == 2u || primary == 3u || primary == 4u || primary == 5u ||
        primary == 0x14u || primary == 0x15u)
    {
        return true;
    }
    if (primary == 0u)
    {
        const uint32_t function = opcode & 63u;
        const uint32_t rs = (opcode >> 21u) & 31u;
        const uint32_t rd = (opcode >> 11u) & 31u;
        return (function == 8u && (opcode & 0x001fffc0u) == 0u) ||
            (function == 9u && (opcode & 0x001f07c0u) == 0u && rs != rd);
    }
    return false;
}

static_assert(ee_strict_branch_supported(0x10000000u));
static_assert(ee_strict_branch_supported(0x54000000u));
static_assert(ee_strict_branch_supported(0x0c000000u));
static_assert(ee_strict_branch_supported(0x00a03809u));
static_assert(!ee_strict_branch_supported(0x00a02809u));

// Check the live branch fetch without executing it. A generated branch
// pair must call this first, evaluate the condition, and call the delay
// helper only if the slot is executed. The branch leaves cpu.m_pc at
// its own PC until the pair succeeds or commits a fault. A completed,
// restartable link is visible to the slot and preserved on a guest
// exception; generated code restores it on host refusal.
// These are internal experimental entry contracts, not guest callbacks.
extern "C" uint32_t ee_strict_native_check_branch(
    s_ee_cpu_state* cpu, s_ee_strict_native_word_context* context,
    uint32_t expected_pc, uint32_t expected_opcode);
extern "C" uint32_t ee_strict_native_prepare_delay_transfer(
    s_ee_cpu_state* cpu, s_ee_strict_native_word_context* context,
    uint32_t expected_pc, uint32_t expected_opcode, uint8_t** out,
    uint32_t* lane_offset);

// General transfer preparation. For partial operations the returned
// pointer addresses the first enabled byte, and lane_offset is the
// original effective address modulo container width. Both outputs
// must be consumed only on success within the same isolated call.
extern "C" uint32_t ee_strict_native_prepare_transfer(
    s_ee_cpu_state* cpu, s_ee_strict_native_word_context* context,
    uint32_t expected_pc, uint32_t expected_opcode, uint8_t** out,
    uint32_t* lane_offset);

// Returns the strict step result, not e_ee_exit_reason. COMMITTED (0)
// means preparation succeeded; the generated success edge must finish
// the transfer and advance PC synchronously. No pointer may escape.
// On fault/rejection the out pointer is null; no transfer is allowed.
extern "C" uint32_t ee_strict_native_prepare_word(
    s_ee_cpu_state* cpu, s_ee_strict_native_word_context* context,
    uint32_t expected_pc, uint32_t expected_opcode, uint8_t** out);

} // namespace ee
