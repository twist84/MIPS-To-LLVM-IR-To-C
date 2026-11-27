#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <limits>

namespace iop
{
using t_cdvd_rtc = std::array<uint8_t, 8u>;

// A fixed UTC Unix timestamp is an explicit diagnostic input, not
// guest time advancement. Restrict it to the two-digit PS2 RTC era
// (2000..2099, including the JST offset).
inline bool parse_fixed_cdvd_rtc(const char* text, std::time_t& result)
{
    constexpr uint64_t first = 946684800ull;
    constexpr uint64_t last = 4102444799ull - 9u * 60u * 60u;
    if (text == nullptr || *text == '\0')
    {
        return false;
    }
    uint64_t value = 0u;
    for (const char* digit = text; *digit != '\0'; ++digit)
    {
        if (*digit < '0' || *digit > '9' || value > last / 10u)
        {
            return false;
        }
        value = value * 10u + static_cast<uint64_t>(*digit - '0');
    }
    if (value < first || value > last ||
        value + 9u * 60u * 60u >
            static_cast<uint64_t>(std::numeric_limits<std::time_t>::max()))
    {
        return false;
    }
    result = static_cast<std::time_t>(value);
    return true;
}

// The RTC stores Japan Standard Time, including the correct calendar
// date when adding nine hours crosses midnight or the end of a year.
inline bool encode_cdvd_rtc(std::time_t utc, t_cdvd_rtc& result)
{
    constexpr std::time_t offset = 9 * 60 * 60;
    if (utc == static_cast<std::time_t>(-1) ||
        utc > std::numeric_limits<std::time_t>::max() - offset)
    {
        return false;
    }
    const std::time_t jst = utc + offset;
    std::tm calendar{};
#if defined(_WIN32)
    if (::gmtime_s(&calendar, &jst) != 0)
#else
    if (::gmtime_r(&jst, &calendar) == nullptr)
#endif
    {
        return false;
    }
    const auto bcd = [](int value) -> uint8_t
    {
        return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
    };
    result = {0u, bcd(calendar.tm_sec), bcd(calendar.tm_min),
        bcd(calendar.tm_hour), 0u, bcd(calendar.tm_mday),
        bcd(calendar.tm_mon + 1), bcd((calendar.tm_year + 1900) % 100)};
    return true;
}

inline bool read_cdvd_rtc(t_cdvd_rtc& result)
{
    const char* fixed = std::getenv("SDBZ_RTC_FIXED_UTC");
    std::time_t utc{};
    if (fixed != nullptr)
    {
        if (!parse_fixed_cdvd_rtc(fixed, utc))
        {
            return false;
        }
    }
    else
    {
        utc = std::time(nullptr);
    }
    return encode_cdvd_rtc(utc, result);
}
} // namespace iop
