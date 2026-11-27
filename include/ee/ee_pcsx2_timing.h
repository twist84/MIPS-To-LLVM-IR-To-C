#pragma once

#include "ee_instruction.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ee
{
// A PCSX2-style heuristic in eighths of an EE cycle, not measured R5900
// latency. Only apply to an instruction accepted by the exact EE decoder.
// Zero means that no reference cost is established.
namespace pcsx2_timing_detail
{
constexpr uint32_t reference_opcode_weight(uint32_t word, e_ee_valid validity)
{
    if (validity != e_ee_valid::VALID)
    {
        return 0u;
    }

    const uint32_t primary = word >> 26;
    const uint32_t selector = (word >> 21) & 31u;
    const uint32_t rt = (word >> 16) & 31u;
    const uint32_t sa = (word >> 6) & 31u;
    const uint32_t funct = word & 63u;

    switch (primary)
    {
    case 0x00:
        switch (funct)
        {
        case 0x18: case 0x19: return 16u;
        case 0x1a: case 0x1b: return 112u;
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x36: return 11u;
        case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:
        case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
        case 0x0f: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
        case 0x16: case 0x17: case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29:
        case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        case 0x38: case 0x3a: case 0x3b: case 0x3c: case 0x3e: case 0x3f:
            return 9u;
        default: return 0u;
        }
    case 0x01:
        switch (rt)
        {
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0e:
        case 0x10: case 0x11: case 0x12: case 0x13: return 11u;
        case 0x18: case 0x19: return 9u;
        default: return 0u;
        }
    case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    case 0x18: case 0x19: case 0x2f: case 0x33:
        return 9u;
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x14: case 0x15: case 0x16: case 0x17:
        return 11u;
    case 0x1a: case 0x1b: case 0x1e: case 0x1f:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x31:
    case 0x36: case 0x37: case 0x39: case 0x3e: case 0x3f:
        return 14u;
    case 0x10:
        if (selector == 8u)
        {
            return rt < 4u ? 11u : 0u;
        }
        if (selector == 0u || selector == 4u)
        {
            return 7u;
        }
        if (selector == 16u)
        {
            switch (funct)
            {
            case 1: case 2: case 6: case 8: case 0x18: case 0x38: case 0x39:
                return 7u;
            default: return 0u;
            }
        }
        return 0u;
    case 0x11:
        switch (selector)
        {
        case 0: case 2: case 4: case 6: return 7u;
        case 8: return rt < 4u ? 11u : 0u;
        case 20: return funct == 0x20u ? 7u : 0u;
        case 16:
            switch (funct)
            {
            case 2: case 0x1a: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
                return 32u;
            case 3: case 4: return 48u;
            case 0x16: return 64u;
            case 0: case 1: case 5: case 6: case 7:
            case 0x18: case 0x19: case 0x24: case 0x28: case 0x29:
            case 0x30: case 0x32: case 0x34: case 0x36: return 7u;
            default: return 0u;
            }
        default: return 0u;
        }
    case 0x12:
        // The reference charges the primary COP2 entry; the decoder
        // validates the exact VU operation. No extra VU pipeline cost here.
        return 9u;
    case 0x1c:
        switch (funct)
        {
        case 0x00: case 0x01: case 0x18: case 0x19: case 0x20: case 0x21:
            return 16u;
        case 0x1a: case 0x1b: return 112u;
        case 0x10: case 0x11: case 0x12: case 0x13: return 9u;
        case 0x04: case 0x34: case 0x36: case 0x37:
        case 0x3c: case 0x3e: case 0x3f: return 14u;
        case 0x30: return sa <= 4u ? 14u : 0u;
        case 0x31: return sa == 0u ? 14u : 0u;
        case 0x08: // MMI0
            return (sa <= 10u || (sa >= 16u && sa <= 27u) || sa >= 30u) ? 14u : 0u;
        case 0x28: // MMI1
            switch (sa)
            {
            case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 10:
            case 16: case 17: case 18: case 20: case 21: case 22:
            case 24: case 25: case 26: case 27: return 14u;
            default: return 0u;
            }
        case 0x09: // MMI2
            switch (sa)
            {
            case 0: case 4: case 8: case 9: case 12:
            case 16: case 17: case 20: case 21: case 28: return 24u;
            case 13: case 29: return 176u;
            case 2: case 3: case 10: case 14: case 18: case 19:
            case 26: case 27: case 30: case 31: return 14u;
            default: return 0u;
            }
        case 0x29: // MMI3
            switch (sa)
            {
            case 0: case 12: return 24u;
            case 13: return 176u;
            case 3: case 8: case 9: case 10: case 14:
            case 18: case 19: case 26: case 27: case 30: return 14u;
            default: return 0u;
            }
        default: return 0u;
        }
    default: return 0u;
    }
}

// Only the four packed sub-op groups need selector lookup. The
// existing reference classifier is the single source of their costs.
inline constexpr std::array<uint32_t, 4> k_mmi_groups{0x08u, 0x28u, 0x09u, 0x29u};

consteval auto make_mmi_weights()
{
    std::array<std::array<uint8_t, 32>, k_mmi_groups.size()> weights{};
    for (std::size_t group = 0u; group < k_mmi_groups.size(); ++group)
    {
        for (uint32_t sa = 0u; sa < 32u; ++sa)
        {
            const uint32_t word = (0x1cu << 26u) | (sa << 6u) | k_mmi_groups[group];
            const auto weight = reference_opcode_weight(word, e_ee_valid::VALID);
            weights[group][sa] = static_cast<uint8_t>(weight);
        }
    }
    return weights;
}

inline constexpr auto k_mmi_weights = make_mmi_weights();
static_assert([]() consteval
{
    for (std::size_t group = 0u; group < k_mmi_groups.size(); ++group)
    {
        for (uint32_t sa = 0u; sa < 32u; ++sa)
        {
            const uint32_t word = (0x1cu << 26u) | (sa << 6u) | k_mmi_groups[group];
            if (k_mmi_weights[group][sa] != reference_opcode_weight(word, e_ee_valid::VALID))
            {
                return false;
            }
        }
    }
    return true;
}(), "MMI timing weights must fit the generated table");
} // namespace pcsx2_timing_detail

constexpr uint32_t pcsx2_opcode_weight(uint32_t word, e_ee_valid validity)
{
    if (validity != e_ee_valid::VALID)
    {
        return 0u;
    }
    if ((word >> 26u) == 0x1cu)
    {
        const uint32_t sa = (word >> 6u) & 31u;
        switch (word & 63u)
        {
        case 0x08u: return pcsx2_timing_detail::k_mmi_weights[0][sa];
        case 0x28u: return pcsx2_timing_detail::k_mmi_weights[1][sa];
        case 0x09u: return pcsx2_timing_detail::k_mmi_weights[2][sa];
        case 0x29u: return pcsx2_timing_detail::k_mmi_weights[3][sa];
        default: break;
        }
    }
    return pcsx2_timing_detail::reference_opcode_weight(word, validity);
}

constexpr uint32_t pcsx2_opcode_weight(const s_ee_instruction& insn)
{
    return pcsx2_opcode_weight(insn.opcode_word, insn.valid);
}

// Accumulate an explicitly chosen DIE epoch in eighth-cycles. This is
// arithmetic only: the caller must establish guest publication boundaries,
// Config write ordering and idle/HLE policy. Never force a minimum tick.
struct s_pcsx2_cycle_accumulator
{
    uint64_t m_eighth_cycles = 0u;

    constexpr bool add(uint64_t weight, uint64_t count, uint32_t config)
    {
        if (weight == 0u)
        {
            return false;
        }
        const uint64_t scale = (config & (1u << 18)) != 0u ? 1u : 2u;
        const uint64_t limit = (std::numeric_limits<uint64_t>::max)();
        if (weight > limit / scale)
        {
            return false;
        }
        const uint64_t scaled = weight * scale;
        if (count > (limit - m_eighth_cycles) / scaled)
        {
            return false;
        }
        m_eighth_cycles += scaled * count;
        return true;
    }

    constexpr uint64_t publish_whole()
    {
        const uint64_t whole = m_eighth_cycles / 8u;
        m_eighth_cycles %= 8u;
        return whole;
    }
};
} // namespace ee
