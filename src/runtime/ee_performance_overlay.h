#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cmath>

namespace ee
{
// Fixed-size host-only text. Each 5x7 glyph occupies two words (six
// 5-bit rows in word 0, last row in word 1). No guest memory or font I/O.
inline constexpr uint32_t k_ee_overlay_columns = 32u;
inline constexpr uint32_t k_ee_overlay_rows = 8u;

inline constexpr std::array<uint32_t, 2> ee_overlay_glyph(char ch)
{
    switch (ch)
    {
    case '0': return {0x239ace2eu, 14u};
    case '1': return {0x8421184u, 14u};
    case '2': return {0x1041062eu, 31u};
    case '3': return {0x217043eu, 30u};
    case '4': return {0x5f928c2u, 2u};
    case '5': return {0x21f421fu, 30u};
    case '6': return {0x231f420eu, 14u};
    case '7': return {0x1082083fu, 8u};
    case '8': return {0x2317462eu, 14u};
    case '9': return {0x217c62eu, 14u};
    case 'A': return {0x231fc62eu, 17u};
    case 'B': return {0x231f463eu, 30u};
    case 'C': return {0x2308422eu, 14u};
    case 'D': return {0x2318c63eu, 30u};
    case 'E': return {0x210f421fu, 31u};
    case 'F': return {0x210f421fu, 16u};
    case 'G': return {0x231bc22eu, 14u};
    case 'H': return {0x231fc631u, 17u};
    case 'I': return {0x842108eu, 14u};
    case 'J': return {0x25210847u, 12u};
    case 'K': return {0x254c5251u, 17u};
    case 'L': return {0x21084210u, 31u};
    case 'M': return {0x231ad771u, 17u};
    case 'N': return {0x2319d731u, 17u};
    case 'O': return {0x2318c62eu, 14u};
    case 'P': return {0x210f463eu, 16u};
    case 'Q': return {0x2558c62eu, 13u};
    case 'R': return {0x254f463eu, 17u};
    case 'S': return {0x217420fu, 30u};
    case 'T': return {0x842109fu, 4u};
    case 'U': return {0x2318c631u, 14u};
    case 'V': return {0x1518c631u, 4u};
    case 'W': return {0x2b5ac631u, 10u};
    case 'X': return {0x22a22a31u, 17u};
    case 'Y': return {0x8422a31u, 4u};
    case 'Z': return {0x2082083fu, 31u};
    case '.': return {0x18000000u, 12u};
    case ':': return {0x18c03180u, 0u};
    case '/': return {0x10820841u, 16u};
    case '%': return {0x20821051u, 17u};
    case '-': return {0xf8000u, 0u};
    case '+': return {0x84f9080u, 0u};
    default: return {};
    }
}

struct s_ee_overlay_text
{
    std::array<uint32_t, k_ee_overlay_columns * k_ee_overlay_rows * 2u> m_words{};

    void set_line(uint32_t row, const char* text)
    {
        if (row >= k_ee_overlay_rows || text == nullptr)
        {
            return;
        }
        bool ended = false;
        for (uint32_t column = 0u; column < k_ee_overlay_columns; ++column)
        {
            const char ch = ended ? '\0' : text[column];
            ended = ended || ch == '\0';
            const auto glyph = ee_overlay_glyph(ch);
            const uint32_t i = (row * k_ee_overlay_columns + column) * 2u;
            m_words[i] = glyph[0];
            m_words[i + 1u] = glyph[1];
        }
    }
};

struct s_ee_performance_counters
{
    uint64_t m_cycles = 0u;
    uint64_t m_dispatches = 0u;
    uint64_t m_instructions = 0u;
    uint64_t m_fields = 0u;
    uint64_t m_presentations = 0u;
    uint64_t m_draws = 0u;
    uint64_t m_ee_host_ns = 0u;
    uint64_t m_iop_host_ns = 0u;
    uint32_t m_target_speed = 0u;
    uint32_t m_crt_mode = 0u;
    bool m_clock_available = false;
    bool m_profile_enabled = false;
};

class c_ee_performance_overlay
{
public:
    bool visible() const { return m_visible; }
    const s_ee_overlay_text& text() const { return m_text; }

    void toggle()
    {
        m_visible = !m_visible;
        m_have_baseline = false;
        m_text = {};
        if (m_visible)
        {
            m_text.set_line(0u, "PERFORMANCE  F5  CPU PROFILE F6");
            m_text.set_line(1u, "SAMPLING...");
        }
    }

    // Update no more than four times per second. On a counter reset or
    // nonmonotonic host time, establish a new baseline rather than wrap.
    bool sample(double now, const s_ee_performance_counters& current)
    {
        if (!m_visible || !std::isfinite(now))
        {
            return false;
        }
        const auto& old = m_previous;
        if (!m_have_baseline || now < m_time ||
            current.m_cycles < old.m_cycles || current.m_dispatches < old.m_dispatches ||
            current.m_instructions < old.m_instructions || current.m_fields < old.m_fields ||
            current.m_presentations < old.m_presentations || current.m_draws < old.m_draws ||
            current.m_ee_host_ns < old.m_ee_host_ns || current.m_iop_host_ns < old.m_iop_host_ns ||
            current.m_clock_available != old.m_clock_available ||
            current.m_profile_enabled != old.m_profile_enabled ||
            current.m_crt_mode != old.m_crt_mode)
        {
            m_previous = current;
            m_time = now;
            m_have_baseline = true;
            m_text = {};
            m_text.set_line(0u, "PERFORMANCE  F5  CPU PROFILE F6");
            m_text.set_line(1u, "SAMPLING...");
            return true;
        }
        const double dt = now - m_time;
        if (dt < 0.25)
        {
            return false;
        }

        char line[64]{};
        if (current.m_clock_available)
        {
            const double speed = (current.m_cycles - old.m_cycles) * 100.0 /
                (294912000.0 * dt);
            if (current.m_target_speed == 0u)
            {
                std::snprintf(line, sizeof(line), "SPEED    %8.1f%% / UNLIMITED", speed);
            }
            else
            {
                std::snprintf(line, sizeof(line), "SPEED    %8.1f%% / %u%%", speed,
                    current.m_target_speed);
            }
        }
        else
        {
            std::snprintf(line, sizeof(line), "SPEED    UNAVAILABLE");
        }
        m_text.set_line(1u, line);
        const double nominal_hz = current.m_crt_mode == 2u ? 60000.0 / 1001.0 :
            (current.m_crt_mode == 3u ? 50.0 : 0.0);
        const double presents = (current.m_presentations - old.m_presentations) / dt;
        const double fields = (current.m_fields - old.m_fields) / dt;
        if (nominal_hz > 0.0)
        {
            std::snprintf(line, sizeof(line), "PRESENT  %8.1f / %5.2f FPS", presents, nominal_hz);
            m_text.set_line(2u, line);
            std::snprintf(line, sizeof(line), "GUEST    %8.1f / %5.2f F/S", fields, nominal_hz);
        }
        else
        {
            std::snprintf(line, sizeof(line), "PRESENT  %8.1f FPS", presents);
            m_text.set_line(2u, line);
            std::snprintf(line, sizeof(line), "GUEST    %8.1f F/S", fields);
        }
        m_text.set_line(3u, line);
        std::snprintf(line, sizeof(line), "EE       %8.1f M INST/S",
            (current.m_instructions - old.m_instructions) / (dt * 1e6));
        m_text.set_line(4u, line);
        std::snprintf(line, sizeof(line), "DISPATCH %8.1f K/S",
            (current.m_dispatches - old.m_dispatches) / (dt * 1e3));
        m_text.set_line(5u, line);
        std::snprintf(line, sizeof(line), "GS       %8.1f K DRAWS/S",
            (current.m_draws - old.m_draws) / (dt * 1e3));
        m_text.set_line(6u, line);
        if (current.m_profile_enabled)
        {
            // Completed exclusive runner-thread scopes, not total CPU/GPU usage.
            std::snprintf(line, sizeof(line), "EE HOST  %8.1f%% IOP %5.1f%%",
                (current.m_ee_host_ns - old.m_ee_host_ns) / (dt * 1e7),
                (current.m_iop_host_ns - old.m_iop_host_ns) / (dt * 1e7));
        }
        else
        {
            std::snprintf(line, sizeof(line), "CPU PROFILE OFF");
        }
        m_text.set_line(7u, line);
        m_previous = current;
        m_time = now;
        return true;
    }

private:
    bool m_visible = false;
    bool m_have_baseline = false;
    double m_time = 0.0;
    s_ee_performance_counters m_previous{};
    s_ee_overlay_text m_text{};
};

} // namespace ee
