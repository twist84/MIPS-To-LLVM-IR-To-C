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

static_assert(ee_strict_jr_supported(0x03000008u));
static_assert(ee_strict_jr_supported(0x00000008u));
static_assert(!ee_strict_jr_supported(0x03010008u));
static_assert(!ee_strict_jr_supported(0x03000808u));
static_assert(!ee_strict_jr_supported(0x03000048u));
static_assert(!ee_strict_jr_supported(0x07000008u));
} // namespace ee
