#pragma once

#include <cstdint>

namespace ee
{
// EE Core User's Manual v6.0, Fig. 5-1 and sections 5.1.4-5.1.6.
// Segment classification precedes TLB and physical bus decoding. A
// direct address is not necessarily backed by RAM. Cache modes are
// descriptive only; this is not a cache implementation.
enum class e_ee_address_route : uint8_t
{
    TLB_MAPPED,
    DIRECT_PHYSICAL,
    PRIVILEGE_ERROR,
    RESERVED_MODE,
    RESERVED_CACHE_MODE
};

struct s_ee_address_route
{
    e_ee_address_route m_route = e_ee_address_route::PRIVILEGE_ERROR;
    uint32_t m_physical_address = 0u;
    uint32_t m_cache_mode = 0u;
};

inline constexpr s_ee_address_route ee_classify_address(
    uint32_t address, uint32_t status, uint32_t config)
{
    using route = e_ee_address_route;
    const bool erl = (status & 4u) != 0u;
    const bool exl = (status & 2u) != 0u;
    const uint32_t mode = (erl || exl) ? 0u : (status >> 3u) & 3u;
    if (mode == 3u)
    {
        return {route::RESERVED_MODE};
    }
    if (address < 0x80000000u)
    {
        return erl ? s_ee_address_route{route::DIRECT_PHYSICAL, address, 2u}
                   : s_ee_address_route{route::TLB_MAPPED};
    }
    if (address < 0xc0000000u)
    {
        if (mode != 0u)
        {
            return {route::PRIVILEGE_ERROR};
        }
        const uint32_t cache = address < 0xa0000000u ? config & 7u : 2u;
        // Config.K0 permits mode 0; EntryLo.C does not (manual p. 78).
        if (cache != 0u && cache != 2u && cache != 3u && cache != 7u)
        {
            return {route::RESERVED_CACHE_MODE};
        }
        return {route::DIRECT_PHYSICAL, address & 0x1fffffffu, cache};
    }
    if (address < 0xe0000000u)
    {
        return mode == 2u ? s_ee_address_route{route::PRIVILEGE_ERROR}
                          : s_ee_address_route{route::TLB_MAPPED};
    }
    return mode == 0u ? s_ee_address_route{route::TLB_MAPPED}
                      : s_ee_address_route{route::PRIVILEGE_ERROR};
}

} // namespace ee
