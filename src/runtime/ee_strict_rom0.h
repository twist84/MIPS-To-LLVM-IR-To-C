#pragma once

#include "ee_memory_bus_plan.h"
#include <cstddef>

namespace ee
{
// Caller-owned immutable instruction backing, distinct from IOP ROM0.
struct s_ee_strict_rom0_view
{
    static constexpr size_t k_size = 0x00400000u;
    static constexpr uint32_t k_base = 0x1fc00000u;
    const uint8_t* m_bytes = nullptr;
    size_t m_size = 0u;

    // Read-only data access for a previously translated, validated span.
    // Only the isolated reset executor opts into this path. Neither
    // cached/accelerated reads nor any mutable ROM backing are exposed.
    constexpr const uint8_t* read_span(const s_ee_bus_span_plan& bus) const
    {
        if (m_bytes == nullptr || m_size != k_size ||
            bus.m_region != e_ee_bus_region::ROM0 ||
            bus.m_cache_policy != e_ee_data_cache_policy::UNCACHED ||
            bus.m_width > 16u ||
            !ee_bus_span_inside(bus.m_address, bus.m_width,
                                k_base, uint64_t{k_base} + k_size))
        {
            return nullptr;
        }
        return m_bytes + (bus.m_address - k_base);
    }

    constexpr const uint8_t* fetch_span(const s_ee_fetch_plan& fetch) const
    {
        if (m_bytes == nullptr || m_size != k_size ||
            fetch.m_route != e_ee_fetch_route::CACHE_BYPASS_PHYSICAL ||
            fetch.m_access.m_status != e_ee_access_status::PHYSICAL ||
            fetch.m_access.m_cache_mode != 2u || (fetch.m_pc & 3u) != 0u ||
            !ee_bus_span_inside(fetch.m_access.m_physical_address, 4u,
                                k_base, uint64_t{k_base} + k_size))
        {
            return nullptr;
        }
        return m_bytes + (fetch.m_access.m_physical_address - k_base);
    }
};

inline constexpr uint32_t ee_read_instruction_word(const uint8_t* code)
{
    return static_cast<uint32_t>(code[0]) |
        (static_cast<uint32_t>(code[1]) << 8u) |
        (static_cast<uint32_t>(code[2]) << 16u) |
        (static_cast<uint32_t>(code[3]) << 24u);
}
static_assert(s_ee_strict_rom0_view{}.fetch_span({}) == nullptr);

} // namespace ee
