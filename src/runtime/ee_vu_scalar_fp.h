#pragma once

#include <cstdint>

// R5900 signed-digit root and quotient model shared by the EE and VU
// divide units. This C++ implementation follows the existing module-local
// LLVM recurrence in ee_cop1_rsqrt_ir.h, not host IEEE rounding.
namespace ee::vu_scalar_fp
{
struct s_step
{
    uint32_t m_sum;
    uint32_t m_carry;
    int32_t m_digit;
};

inline s_step step(uint32_t sum, uint32_t carry, uint32_t magnitude, int32_t digit)
{
    const uint32_t term = digit > 0 ? ~magnitude : digit < 0 ? magnitude : 0u;
    const uint32_t adjusted = carry + static_cast<uint32_t>(digit > 0);
    const uint32_t parity = sum ^ adjusted;
    const uint32_t next_sum = parity ^ term;
    const uint32_t next_carry = ((sum & adjusted) | (parity & term)) << 1u;
    const uint32_t test_sum = digit == 0 ? sum : next_sum;
    const uint32_t test_carry = digit == 0 ? carry : next_carry;
    const uint32_t estimate = ((test_sum & 0xff000000u) + test_carry) |
        (test_sum & 0xffffffu);
    const bool positive = estimate >= 0x00800000u && estimate < 0x80000000u;
    const bool negative = estimate >= 0x80000000u && estimate < 0xff000000u;
    return {next_sum << 1u, next_carry << 1u, positive ? 1 : negative ? -1 : 0};
}

inline uint32_t exponent(uint32_t bits)
{
    return (bits >> 23u) & 255u;
}

inline uint32_t root(uint32_t bits)
{
    const uint32_t e = exponent(bits);
    if (e == 0u)
    {
        return 0u;
    }
    s_step state{((bits & 0x7fffffu) | 0x800000u) << (2u - (e & 1u)), 0u, 1};
    uint32_t result = 0u;
    for (int32_t i = 24; i >= 0; --i)
    {
        const uint32_t increment = static_cast<uint32_t>(state.m_digit) << i;
        const uint32_t trial = result + increment;
        result += increment << 1u;
        state = step(state.m_sum, state.m_carry, trial, state.m_digit);
    }
    return (((e + 127u) >> 1u) << 23u) | ((result >> 2u) & 0x7fffffu);
}

inline uint32_t quotient(uint32_t a, uint32_t b)
{
    const int ea = static_cast<int>(exponent(a));
    const int eb = static_cast<int>(exponent(b));
    const uint32_t sign = (a ^ b) & 0x80000000u;
    if (eb == 0)
    {
        return sign | 0x7fffffffu;
    }
    if (ea == 0)
    {
        return sign;
    }
    s_step state{((a & 0x7fffffu) | 0x800000u) << 2u, 0u, 1};
    const uint32_t divisor = ((b & 0x7fffffu) | 0x800000u) << 2u;
    uint32_t result = 0u;
    for (int32_t i = 25; i >= 0; --i)
    {
        result = (result << 1u) + static_cast<uint32_t>(state.m_digit);
        state = step(state.m_sum, state.m_carry, divisor, state.m_digit);
    }
    const bool wide = result >= 0x1000000u;
    const int e = ea - eb + 126 + static_cast<int>(wide);
    if (e > 255)
    {
        return sign | 0x7fffffffu;
    }
    if (e <= 0)
    {
        return sign;
    }
    return sign | (static_cast<uint32_t>(e) << 23u) |
        ((result >> static_cast<uint32_t>(wide)) & 0x7fffffu);
}

inline uint32_t current_flags(uint32_t a, uint32_t b, bool is_sqrt, bool is_rsqrt)
{
    uint32_t flags = (is_sqrt || is_rsqrt) && (b & 0x80000000u) ? 0x10u : 0u;
    if (!is_sqrt && exponent(b) == 0u)
    {
        flags |= exponent(a) == 0u ? 0x10u : 0x20u;
    }
    return flags;
}

inline uint32_t update_status(uint32_t status, uint32_t current)
{
    return (status & ~0x30u) | current | (current << 6u);
}
}
