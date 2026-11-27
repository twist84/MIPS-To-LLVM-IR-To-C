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
