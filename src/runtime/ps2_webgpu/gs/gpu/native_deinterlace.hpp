#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ps2::gs::gpu
{
// PCSX2-compatible option numbering. The reconstruction algorithms are
// implemented independently in the native WebGPU presentation shader.
enum class e_deinterlace_mode : uint32_t
{
    automatic, off, weave_tff, weave_bff, bob_tff, bob_bff,
    blend_tff, blend_bff, adaptive_tff, adaptive_bff
};

enum class e_field_method : uint32_t
{
    off, weave, bob, blend, adaptive, legacy_average
};

struct s_field_presentation
{
    e_field_method m_method = e_field_method::off;
    uint32_t m_parity = 0u;
    bool m_history = false;
};

inline e_deinterlace_mode parse_deinterlace_mode(const char* value)
{
    if (value != nullptr && value[0] >= '0' && value[0] <= '9' && value[1] == '\0')
    {
        return static_cast<e_deinterlace_mode>(value[0] - '0');
    }
    return e_deinterlace_mode::automatic;
}

// The GUI writes a single ASCII digit atomically. Ignore missing/invalid
// data and retain the last valid selection rather than changing modes.
inline e_deinterlace_mode read_deinterlace_mode(const char* path, e_deinterlace_mode fallback)
{
    if (path != nullptr && path[0] != '\0')
    {
        if (FILE* input = std::fopen(path, "rb"))
        {
            const int value = std::fgetc(input);
            const int end = std::fgetc(input);
            std::fclose(input);
            if (value >= '0' && value <= '9' && end == EOF)
            {
                fallback = static_cast<e_deinterlace_mode>(value - '0');
            }
        }
    }
    return fallback;
}

inline s_field_presentation select_field_presentation(
    e_deinterlace_mode mode, bool interlaced, bool halved,
    uint64_t field, uint64_t previous_field, bool compatible_history,
    bool legacy_force = false, bool legacy_off = false)
{
    s_field_presentation result{};
    result.m_history = compatible_history && field != 0u &&
        previous_field == field - 1u;
    result.m_parity = field != 0u ? static_cast<uint32_t>((field - 1u) & 1u) : 0u;
    if (mode == e_deinterlace_mode::automatic)
    {
        if (!legacy_off)
        {
            // Keep the original automatic blending policy for half-height
            // scanout; use adaptive reconstruction for interlaced full frames.
            if (legacy_force)
            {
                result.m_method = result.m_history ? e_field_method::legacy_average : e_field_method::off;
            }
            else if (interlaced)
            {
                result.m_method = halved ? e_field_method::blend : e_field_method::adaptive;
            }
        }
    }
    else if (interlaced && mode != e_deinterlace_mode::off)
    {
        const uint32_t index = static_cast<uint32_t>(mode) - 2u;
        result.m_method = static_cast<e_field_method>(1u + index / 2u);
        result.m_parity ^= index & 1u;
    }
    if (!result.m_history && result.m_method != e_field_method::off &&
        result.m_method != e_field_method::legacy_average)
    {
        result.m_method = e_field_method::bob;
    }
    return result;
}
}
