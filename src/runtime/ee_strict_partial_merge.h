#pragma once

#include <cstdint>

namespace ee
{
// All callers supply a validated lane range within one 64-bit half.
inline constexpr uint64_t ee_strict_low_byte_mask(uint32_t count)
{
    return count == 8u ? ~uint64_t{0} : (uint64_t{1} << (count * 8u)) - 1u;
}

inline constexpr uint64_t ee_strict_merge_load(uint64_t original, uint64_t incoming,
    uint32_t first_lane, uint32_t count, bool sign_extend_word)
{
    const uint32_t shift = first_lane * 8u;
    const uint64_t mask = ee_strict_low_byte_mask(count);
    uint64_t value = (original & ~(mask << shift)) | ((incoming & mask) << shift);
    if (sign_extend_word)
    {
        value &= 0xffffffffull;
        if ((value & 0x80000000ull) != 0u)
        {
            value |= 0xffffffff00000000ull;
        }
    }
    return value;
}

static_assert(ee_strict_low_byte_mask(8u) == 0xffffffffffffffffull);
static_assert(ee_strict_merge_load(0x1122334455667788ull, 0x80u, 3u, 1u, true) ==
              0xffffffff80667788ull);
static_assert(ee_strict_merge_load(0x1122334455667788ull, 0xffu, 0u, 1u, false) ==
              0x11223344556677ffull);
static_assert(ee_strict_merge_load(0u, 0x8877665544332211ull, 0u, 8u, false) ==
              0x8877665544332211ull);

} // namespace ee
