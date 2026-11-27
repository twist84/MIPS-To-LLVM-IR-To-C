#include "ee_pcsx2_timing.h"
#include "ee_decoder.h"

#include <cstdio>

namespace
{
using ee::e_ee_valid;
constexpr uint32_t cost(uint32_t word)
{
    return ee::pcsx2_opcode_weight(word, e_ee_valid::VALID);
}
constexpr uint32_t mmi(uint32_t funct, uint32_t sa)
{
    return (0x1cu << 26) | (sa << 6) | funct;
}
constexpr uint32_t cop1(uint32_t fmt, uint32_t funct)
{
    return (0x11u << 26) | (fmt << 21) | funct;
}
static_assert(cost(0u) == 9u && cost(0x08000000u) == 9u);
static_assert(cost(0x00000008u) == 9u && cost(0x10000000u) == 11u);
static_assert(cost(0x04280000u) == 11u && cost(0x04380000u) == 9u);
static_assert(cost(0x00000030u) == 11u && cost(0x00000018u) == 16u);
static_assert(cost(0x0000001au) == 112u && cost(0x8c000000u) == 14u);
static_assert(cost(mmi(0x18, 0)) == 16u && cost(mmi(0x1a, 0)) == 112u);
static_assert(cost(mmi(8, 0)) == 14u && cost(mmi(9, 0)) == 24u);
static_assert(cost(mmi(0x28, 0)) == 0u && cost(mmi(0x29, 0)) == 24u);
static_assert(cost(mmi(9, 13)) == 176u && cost(mmi(0x29, 13)) == 176u);
static_assert(cost(mmi(9, 8)) == 24u && cost(mmi(0x29, 8)) == 14u);
static_assert(cost(mmi(0x30, 5)) == 0u && cost(mmi(0x31, 1)) == 0u);
static_assert(cost(0x40000000u) == 7u && cost(0x41000000u) == 11u);
static_assert(cost(0x42000018u) == 7u && cost(0x41040000u) == 0u);
static_assert(cost(cop1(16, 2)) == 32u && cost(cop1(16, 3)) == 48u);
static_assert(cost(cop1(16, 0x16)) == 64u && cost(cop1(20, 0x20)) == 7u);
static_assert(cost(cop1(20, 2)) == 0u && cost(0x48000000u) == 9u);
static_assert(cost(0xd4000000u) == 0u);
static_assert(ee::pcsx2_opcode_weight(0u, e_ee_valid::INVALID) == 0u);
static_assert(ee::pcsx2_opcode_weight(0u, e_ee_valid::UNSUPPORTED) == 0u);

template <uint32_t first>
consteval bool check_mmi_weights()
{
    for (uint32_t funct = first; funct < first + 8u; ++funct)
    {
        for (uint32_t sa = 0u; sa < 32u; ++sa)
        {
            // Other operand fields must not influence a selector weight.
            const uint32_t word = mmi(funct, sa) | 0x01253000u;
            if (cost(word) != ee::pcsx2_timing_detail::reference_opcode_weight(
                    word, e_ee_valid::VALID) ||
                ee::pcsx2_opcode_weight(word, e_ee_valid::INVALID) != 0u ||
                ee::pcsx2_opcode_weight(word, e_ee_valid::UNSUPPORTED) != 0u)
            {
                return false;
            }
        }
    }
    return true;
}
static_assert(check_mmi_weights<0u>(), "MMI timing selector group 0");
static_assert(check_mmi_weights<8u>(), "MMI timing selector group 1");
static_assert(check_mmi_weights<16u>(), "MMI timing selector group 2");
static_assert(check_mmi_weights<24u>(), "MMI timing selector group 3");
static_assert(check_mmi_weights<32u>(), "MMI timing selector group 4");
static_assert(check_mmi_weights<40u>(), "MMI timing selector group 5");
static_assert(check_mmi_weights<48u>(), "MMI timing selector group 6");
static_assert(check_mmi_weights<56u>(), "MMI timing selector group 7");

constexpr bool check_accumulator()
{
    ee::s_pcsx2_cycle_accumulator a;
    constexpr uint32_t die = 1u << 18;
    if (!a.add(7u, 1u, die) || a.publish_whole() != 0u ||
        a.m_eighth_cycles != 7u || !a.add(9u, 1u, die) ||
        a.publish_whole() != 2u || a.m_eighth_cycles != 0u)
    {
        return false;
    }
    if (!a.add(9u, 3u, 0u) || a.publish_whole() != 6u ||
        a.m_eighth_cycles != 6u || !a.add(7u, 1u, die) ||
        a.publish_whole() != 1u || a.m_eighth_cycles != 5u)
    {
        return false;
    }
    const uint64_t limit = (std::numeric_limits<uint64_t>::max)();
    a.m_eighth_cycles = limit - 9u;
    return !a.add(9u, 1u, 0u) && a.m_eighth_cycles == limit - 9u &&
        a.add(9u, 1u, die) && a.m_eighth_cycles == limit &&
        !a.add(1u, 1u, die) && !a.add(limit, 1u, 0u) &&
        !a.add(0u, 1u, die) && a.m_eighth_cycles == limit;
}
static_assert(check_accumulator());
}

int main()
{
    // Exercise the real decoder gate independently from raw selector tests.
    struct s_case
    {
        uint32_t m_word;
        e_ee_valid m_validity;
        uint32_t m_weight;
    };
    constexpr s_case cases[] =
    {
        {0x24020001u, e_ee_valid::VALID, 9u},        // ADDIU
        {0x00003028u, e_ee_valid::VALID, 9u},        // MFSA r6
        {0x00203028u, e_ee_valid::UNSUPPORTED, 0u},  // MFSA with reserved rs
        {0xe0850000u, e_ee_valid::INVALID, 0u},      // SC (not on EE)
    };
    for (const auto& item : cases)
    {
        const auto decoded = ee::decode(0x1000u, item.m_word);
        const auto weight = ee::pcsx2_opcode_weight(decoded);
        if (decoded.valid != item.m_validity || weight != item.m_weight)
        {
            std::fprintf(stderr,
                "PCSX2 timing decoder gate mismatch: word=%08x valid=%u/%u weight=%u/%u\n",
                item.m_word, static_cast<unsigned>(decoded.valid),
                static_cast<unsigned>(item.m_validity), weight, item.m_weight);
            return 1;
        }
    }
    return 0;
}
