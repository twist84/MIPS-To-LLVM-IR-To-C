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
