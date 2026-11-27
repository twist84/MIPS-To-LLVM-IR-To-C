#pragma once

#include <cstdint>

namespace ee
{
// A bounded register-programming contract for the two pinned firmware
// CRT profiles. This recognizes actual GS register values, not guest
// code addresses or syscall numbers. No analog PLL lock is simulated.
struct s_ee_strict_gs_timing
{
    uint64_t m_words[5]{};
    uint32_t m_stage = 0u;
    uint32_t m_mode = 0u;
    uint32_t m_smode2 = 0u;

    static constexpr uint64_t profile_word(uint32_t mode, uint32_t smode2, uint32_t index)
    {
        const bool pal = mode == 3u;
        switch (index)
        {
        case 0u: return 8u;
        case 1u: return pal ? 0x7f5c21fc83030ull : 0x7f5b61f06f040ull;
        case 2u: return pal ? 0x3484bcull : 0x33a4d8ull;
        case 3u: return pal
            ? ((smode2 & 1u) != 0u ? 0xa9000502101401ull : 0xa9000502101404ull)
            : ((smode2 & 1u) != 0u ? 0xc7800601a01801ull : 0xc7800601a01802ull);
        case 4u: return (pal ? 0x0740816504ull : 0x0740814504ull) | (3ull << 16u);
        default: return 0u;
        }
    }

    constexpr bool write(uint32_t address, uint64_t value, uint64_t smode2)
    {
        if (smode2 > 3u || m_stage >= 6u ||
            (m_stage != 0u && smode2 != m_smode2))
        {
            return false;
        }
        if (m_stage == 5u)
        {
            if (address != 0x12000010u || value != (m_words[4] & ~(1ull << 17u)))
            {
                return false;
            }
            m_words[4] = value;
            ++m_stage;
            return true;
        }
        const uint32_t expected = m_stage < 4u
            ? 0x12000030u + 0x10u * m_stage : 0x12000010u;
        if (address != expected)
        {
            return false;
        }
        uint32_t matching = 0u;
        for (uint32_t mode = 2u; mode <= 3u; ++mode)
        {
            bool match = value == profile_word(mode, static_cast<uint32_t>(smode2), m_stage);
            for (uint32_t i = 0u; i < m_stage; ++i)
            {
                match = match && m_words[i] == profile_word(mode, static_cast<uint32_t>(smode2), i);
            }
            if (match)
            {
                matching = mode;
            }
        }
        if (matching == 0u)
        {
            return false;
        }
        m_smode2 = static_cast<uint32_t>(smode2);
        m_words[m_stage++] = value;
        if (m_stage == 5u)
        {
            m_mode = matching;
        }
        return true;
    }
};

static_assert([]()
{
    s_ee_strict_gs_timing timing{};
    return !timing.write(0x12000010u, 0u, 3u) &&
        timing.write(0x12000030u, 8u, 3u) &&
        !timing.write(0x12000040u, 0u, 3u) &&
        timing.write(0x12000040u, 0x7f5b61f06f040ull, 3u) &&
        !timing.write(0x12000050u, 0x3484bcull, 3u) &&
        timing.write(0x12000050u, 0x33a4d8ull, 3u) &&
        timing.write(0x12000060u, 0xc7800601a01801ull, 3u) &&
        timing.write(0x12000010u, 0x0740834504ull, 3u) &&
        timing.m_mode == 2u && timing.m_stage == 5u &&
        !timing.write(0x12000010u, 0x0740814504ull, 2u) &&
        timing.write(0x12000010u, 0x0740814504ull, 3u) &&
        timing.m_stage == 6u;
}());
} // namespace ee
