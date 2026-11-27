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
