#pragma once

#include "ee_memory_fault.h"

namespace ee
{
// A data cache policy is descriptive, not permission to access the
// backing directly. Even an uncached access may need store-buffer
// ordering, and accelerated accesses have separate UCAB/WBB state.
enum class e_ee_data_cache_policy : uint8_t
{
    UNMODELED,
    SCRATCHPAD,
    UNCACHED,
    CACHE_DISABLED,
    CACHED_NO_WRITEBACK_ALLOCATE,
    CACHED_WRITEBACK_ALLOCATE,
    UNCACHED_ACCELERATED
};

inline constexpr e_ee_data_cache_policy ee_classify_data_cache(
    const s_ee_access_decision& access, uint32_t config)
{
    using policy = e_ee_data_cache_policy;
    using result = e_ee_access_status;
    policy value = policy::UNMODELED;
    if (access.m_status == result::SCRATCHPAD)
    {
        value = policy::SCRATCHPAD;
    }
    else if (access.m_status == result::PHYSICAL)
    {
        constexpr uint32_t dce = 1u << 16u;
        switch (access.m_cache_mode)
        {
        case 0u:
            value = (config & dce) != 0u
                ? policy::CACHED_NO_WRITEBACK_ALLOCATE : policy::CACHE_DISABLED;
            break;
        case 2u:
            value = policy::UNCACHED;
            break;
        case 3u:
            value = (config & dce) != 0u
                ? policy::CACHED_WRITEBACK_ALLOCATE : policy::CACHE_DISABLED;
            break;
        case 7u:
            // Preserve the accelerated attribute separately; DCE is
            // not evidence that UCAB/WBB state may be discarded.
            value = policy::UNCACHED_ACCELERATED;
            break;
        default:
            break;
        }
    }
    return value;
}

// Deliberately bounded retail map, not an exhaustive EE bus decoder.
// RAM mirrors/expansion configurations and unspecified holes require
// independent evidence. DEVICE is a window requiring further decoding,
// not proof of an implemented register or legal transaction width.
enum class e_ee_bus_region : uint8_t
{
    UNMODELED,
    INVALID_SPAN,
    RAM,
    RAM_BACKING_UNAVAILABLE,
    SCRATCHPAD,
    VU0_CODE,
    VU0_DATA,
    DEVICE,
    ROM0
};

struct s_ee_bus_span_plan
{
    e_ee_bus_region m_region = e_ee_bus_region::UNMODELED;
    e_ee_data_cache_policy m_cache_policy = e_ee_data_cache_policy::UNMODELED;
    uint32_t m_address = 0u;
    uint32_t m_width = 0u;
};

// Address is already translated: never apply a guest virtual alias
// transform or the HLE scratchpad address here. Use 64-bit arithmetic
// for the exclusive end so the last byte at UINT32_MAX is representable.
inline constexpr bool ee_bus_span_inside(uint32_t address, uint32_t width,
    uint64_t first, uint64_t end)
{
    return width != 0u && address >= first &&
        static_cast<uint64_t>(address) + width <= end;
}

inline constexpr e_ee_bus_region ee_classify_physical_span(
    uint32_t address, uint32_t width, uint64_t ram_backing_size)
{
    using region = e_ee_bus_region;
    constexpr uint64_t end32 = uint64_t{1} << 32u;
    constexpr uint64_t ram_end = 0x02000000u;
    constexpr uint64_t device_first = 0x10000000u;
    constexpr uint64_t device_end = 0x13000000u;
    constexpr uint64_t vu_code = 0x11000000u;
    constexpr uint64_t vu_data = 0x11004000u;
    constexpr uint64_t vu_size = 0x1000u;
    constexpr uint64_t rom_first = 0x1fc00000u;
    constexpr uint64_t rom_end = 0x20000000u;
    const uint64_t end = static_cast<uint64_t>(address) + width;
    region value = region::UNMODELED;
    if (width == 0u || width > 16u || end > end32)
    {
        value = region::INVALID_SPAN;
    }
    else if (address < ram_end)
    {
        value = end > ram_end ? region::INVALID_SPAN :
            end > ram_backing_size ? region::RAM_BACKING_UNAVAILABLE : region::RAM;
    }
    else if (ee_bus_span_inside(address, width, vu_code, vu_code + vu_size))
    {
        value = region::VU0_CODE;
    }
    else if (ee_bus_span_inside(address, width, vu_data, vu_data + vu_size))
    {
        value = region::VU0_DATA;
    }
    else if ((address < vu_code + vu_size && end > vu_code) ||
             (address < vu_data + vu_size && end > vu_data))
    {
        value = region::INVALID_SPAN;
    }
    else if (ee_bus_span_inside(address, width, device_first, device_end))
    {
        value = region::DEVICE;
    }
    else if (ee_bus_span_inside(address, width, rom_first, rom_end))
    {
        value = region::ROM0;
    }
    else if ((address < device_end && end > device_first) ||
             (address < rom_end && end > rom_first))
    {
        value = region::INVALID_SPAN;
    }
    return value;
}

// Compose a translated data access with its contiguous byte span.
// The caller supplies actual available main-RAM backing size (zero
// if unavailable). For partial transfers, pass the selected first
// byte and lane count, not an extra guest read of the container.
// No pointer is returned, no bus operation is attempted, and no
// architectural DBE is invented for an unimplemented host region.
inline constexpr s_ee_bus_span_plan ee_plan_data_bus_span(
    const s_ee_access_decision& access, uint32_t width,
    uint32_t config, uint64_t ram_backing_size)
{
    using region = e_ee_bus_region;
    using result = e_ee_access_status;
    s_ee_bus_span_plan plan{};
    plan.m_width = width;
    plan.m_cache_policy = ee_classify_data_cache(access, config);
    if (access.m_status == result::PHYSICAL)
    {
        plan.m_address = access.m_physical_address;
        plan.m_region = ee_classify_physical_span(plan.m_address, width,
                                                ram_backing_size);
    }
    else if (access.m_status == result::SCRATCHPAD)
    {
        plan.m_address = access.m_scratchpad_offset;
        plan.m_region = width <= 16u &&
            ee_bus_span_inside(plan.m_address, width, 0u, 0x4000u)
            ? region::SCRATCHPAD : region::INVALID_SPAN;
    }
    return plan;
}

// The partial plan's translated EA is not always the first enabled
// byte (LEFT). Derive the physical span from the aligned container.
inline constexpr s_ee_bus_span_plan ee_plan_partial_bus_span(
    const s_ee_partial_access_plan& partial, uint32_t config,
    uint64_t ram_backing_size)
{
    auto access = partial.m_access;
    if (partial.m_width != 4u && partial.m_width != 8u)
    {
        return {};
    }
    if (partial.m_lane_count == 0u ||
        partial.m_memory_first_lane >= partial.m_width ||
        partial.m_lane_count > partial.m_width - partial.m_memory_first_lane)
    {
        return {};
    }
    if (access.m_status == e_ee_access_status::PHYSICAL)
    {
        access.m_physical_address = partial.m_aligned_physical_address +
            partial.m_memory_first_lane;
    }
    else if (access.m_status == e_ee_access_status::SCRATCHPAD)
    {
        access.m_scratchpad_offset = partial.m_aligned_scratchpad_offset +
            partial.m_memory_first_lane;
    }
    return ee_plan_data_bus_span(access, partial.m_lane_count, config,
                                 ram_backing_size);
}

} // namespace ee
