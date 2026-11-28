#pragma once

#include <cstdint>

namespace ee
{
// JR has no rt, rd or shamt fields. Keep the canonical encoding
// predicate shared between native lowering and isolated reset execution.
inline constexpr bool ee_strict_jr_supported(uint32_t opcode)
{
    return (opcode & 0xfc1fffffu) == 8u;
}

// Sony EE instruction manual pp.65–68: JALR rs == rd is undefined.
inline constexpr bool ee_strict_jalr_supported(uint32_t opcode)
{
    return (opcode & 0xfc1f07ffu) == 9u &&
        ((opcode >> 21u) & 31u) != ((opcode >> 11u) & 31u);
}

static_assert(ee_strict_jalr_supported(0x0320f809u));
static_assert(ee_strict_jalr_supported(0x00a03809u));
static_assert(ee_strict_jalr_supported(0x00a00009u));
static_assert(!ee_strict_jalr_supported(0x00a02809u));
static_assert(!ee_strict_jalr_supported(0x0321f809u));
static_assert(!ee_strict_jalr_supported(0x0320f849u));
static_assert(!ee_strict_jalr_supported(0x0720f809u));

inline constexpr bool ee_strict_conditional_branch_likely(uint32_t opcode)
{
    const uint32_t primary = opcode >> 26u;
    const uint32_t rt = (opcode >> 16u) & 31u;
    return primary == 0x14u || primary == 0x15u ||
        ((primary == 0x16u || primary == 0x17u) && rt == 0u) ||
        (primary == 1u && (rt == 2u || rt == 3u));
}

// REGIMM link forms are outside this bounded contract.
inline constexpr bool ee_strict_conditional_branch_supported(uint32_t opcode)
{
    const uint32_t primary = opcode >> 26u;
    const uint32_t rt = (opcode >> 16u) & 31u;
    return primary == 4u || primary == 5u ||
        ((primary == 6u || primary == 7u) && rt == 0u) ||
        (primary == 1u && rt <= 1u) || ee_strict_conditional_branch_likely(opcode);
}

// Only called for the validated encodings above. Operands are the
// architectural low 64-bit patterns, not the 32-bit address halves.
inline constexpr bool ee_strict_conditional_branch_taken(uint32_t opcode,
    uint64_t lhs, uint64_t rhs)
{
    const bool negative = (lhs >> 63u) != 0u;
    switch (opcode >> 26u)
    {
    case 4u: case 0x14u: return lhs == rhs;
    case 5u: case 0x15u: return lhs != rhs;
    case 6u: case 0x16u: return negative || lhs == 0u;
    case 7u: case 0x17u: return !negative && lhs != 0u;
    case 1u: return (opcode & 0x10000u) == 0u ? negative : !negative;
    default: return false;
    }
}

inline constexpr uint32_t ee_strict_conditional_branch_target(uint32_t pc,
    uint32_t opcode, uint64_t lhs, uint64_t rhs)
{
    const uint32_t imm = opcode & 0xffffu;
    const uint32_t disp = (imm | ((imm & 0x8000u) != 0u ? 0xffff0000u : 0u)) << 2u;
    return ee_strict_conditional_branch_taken(opcode, lhs, rhs)
        ? pc + 4u + disp : pc + 8u;
}

static_assert(ee_strict_conditional_branch_supported(0x1bc00063u));
static_assert(ee_strict_conditional_branch_supported(0x04810002u));
static_assert(!ee_strict_conditional_branch_supported(0x1bc10063u));
static_assert(ee_strict_conditional_branch_likely(0x04820002u));
static_assert(!ee_strict_conditional_branch_supported(0x04900002u));
static_assert(ee_strict_conditional_branch_taken(0x18800002u, 0x8000000000000000ull, 0u));
static_assert(!ee_strict_conditional_branch_taken(0x18800002u, 0x80000000u, 0u));
static_assert(ee_strict_conditional_branch_target(0u, 0x0480fffeu, ~0ull, 0u) == 0xfffffffcu);

// Direct J/JAL use the high nibble of the delay-slot PC, not the
// branch PC. R5900 links zero-extend the 32-bit PC+8 into GPR[63:0].
inline constexpr uint32_t ee_strict_direct_jump_target(uint32_t pc, uint32_t opcode)
{
    return ((pc + 4u) & 0xf0000000u) | ((opcode & 0x03ffffffu) << 2u);
}

inline constexpr uint64_t ee_strict_link_address(uint32_t pc)
{
    return static_cast<uint64_t>(pc + 8u);
}

static_assert(ee_strict_direct_jump_target(0xa001003cu, 0x0c005877u) == 0xa00161dcu);
static_assert(ee_strict_direct_jump_target(0x0ffffffcu, 0x08000001u) == 0x10000004u);
static_assert(ee_strict_direct_jump_target(0xfffffffcu, 0x08000001u) == 4u);
static_assert(ee_strict_link_address(0xa001003cu) == 0x00000000a0010044ull);
static_assert(ee_strict_link_address(0xfffffffcu) == 4u);

static_assert(ee_strict_jr_supported(0x03000008u));
static_assert(ee_strict_jr_supported(0x00000008u));
static_assert(!ee_strict_jr_supported(0x03010008u));
static_assert(!ee_strict_jr_supported(0x03000808u));
static_assert(!ee_strict_jr_supported(0x03000048u));
static_assert(!ee_strict_jr_supported(0x07000008u));
} // namespace ee
