#pragma once
#include <cstdint>

// Integer VU FMAC reference for the generated LLVM arithmetic.
// Derived from the existing R5900 adder and truncated Booth model.
namespace ee::vu_fmac_fp
{
struct s_fmac_result
{
    uint32_t m_bits;
    bool m_overflow;
    bool m_underflow;
};

// The VU0 adder retains one alignment guard bit and, on cancellation
// underflow, the normalized fraction in an exponent-zero result word.
// See the physical SCPH-900xx observations in PCSX2 PR #12001.
inline s_fmac_result add(uint32_t a, uint32_t b, bool subtract)
{
    if (subtract)
    {
        b ^= 0x80000000u;
    }
    const int ea = static_cast<int>((a >> 23u) & 255u);
    const int eb = static_cast<int>((b >> 23u) & 255u);
    int exponent = ea > eb ? ea : eb;
    const int da = exponent - ea;
    const int db = exponent - eb;
    const uint32_t ma = ea == 0 || da >= 25 ? 0u :
        (((a & 0x7fffffu) | 0x800000u) << 1u) >> da;
    const uint32_t mb = eb == 0 || db >= 25 ? 0u :
        (((b & 0x7fffffu) | 0x800000u) << 1u) >> db;
    const int32_t sa = (a & 0x80000000u) != 0u ?
        -static_cast<int32_t>(ma) : static_cast<int32_t>(ma);
    const int32_t sb = (b & 0x80000000u) != 0u ?
        -static_cast<int32_t>(mb) : static_cast<int32_t>(mb);
    const int32_t sum = sa + sb;
    if (sum == 0)
    {
        return {(a & b) & 0x80000000u, false, false};
    }
    const uint32_t sign = sum < 0 ? 0x80000000u : 0u;
    uint32_t magnitude = static_cast<uint32_t>(sum < 0 ? -sum : sum);
    if (magnitude >= 0x2000000u)
    {
        magnitude >>= 1u;
        ++exponent;
    }
    else
    {
        while (magnitude < 0x1000000u)
        {
            magnitude <<= 1u;
            --exponent;
        }
    }
    const uint32_t fraction = (magnitude >> 1u) & 0x7fffffu;
    if (exponent > 255)
    {
        return {sign | 0x7fffffffu, true, false};
    }
    if (exponent <= 0)
    {
        return {sign | fraction, false, true};
    }
    return {sign | (static_cast<uint32_t>(exponent) << 23u) | fraction,
            false, false};
}

// Same asymmetric truncated Booth multiplier model as ee_cop1_mul_ir.h.
// All low-row operations wrap at 32 bits; the full significand product
// fits within 48 bits. Preserve Fs/Ft order, including broadcast forms.
struct s_product_row
{
    uint32_t m_sum;
    uint32_t m_carry;
};

inline s_product_row booth_row(uint32_t a, uint32_t code, uint32_t shift)
{
    if (code == 0u || code == 7u)
    {
        return {0u, 0u};
    }
    uint32_t magnitude = a << shift;
    if (code == 3u || code == 4u)
    {
        magnitude <<= 1u;
    }
    if (code >= 4u)
    {
        const uint32_t position = 1u << shift;
        return {magnitude ^ (0u - position), position};
    }
    return {magnitude, 0u};
}

inline s_product_row product_csa(uint32_t a, uint32_t b, uint32_t c)
{
    return {a ^ b ^ c, ((a & b) | ((a ^ b) & c)) << 1u};
}

inline s_fmac_result mul(uint32_t a, uint32_t b)
{
    const int ea = static_cast<int>((a >> 23u) & 255u);
    const int eb = static_cast<int>((b >> 23u) & 255u);
    const uint32_t sign = (a ^ b) & 0x80000000u;
    if (ea == 0 || eb == 0)
    {
        return {sign, false, false};
    }
    const uint32_t ma = (a & 0x7fffffu) | 0x800000u;
    const uint32_t mb = (b & 0x7fffffu) | 0x800000u;
    s_product_row rows[8]{};
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        const uint32_t code = i == 0u ? (mb << 1u) : (mb >> (2u * i - 1u));
        rows[i] = booth_row(ma, code & 7u, i * 2u);
    }
    const auto p0 = product_csa(rows[1].m_sum, rows[2].m_sum, rows[3].m_sum);
    auto p1 = product_csa(rows[4].m_sum & ~2047u,
        rows[5].m_sum & ~4095u, rows[6].m_sum);
    p1.m_carry |= rows[6].m_carry | (rows[5].m_sum & 2048u);
    const uint32_t row7 = rows[7].m_sum |
        ((rows[5].m_sum & 1024u) + rows[5].m_carry);
    const auto p2 = product_csa(rows[0].m_sum, p0.m_sum, p0.m_carry);
    const auto p3 = product_csa(row7, p1.m_sum, p1.m_carry);
    const auto p4 = product_csa(p2.m_carry, p3.m_sum, p3.m_carry);
    const auto p5 = product_csa(p2.m_sum, p4.m_sum, p4.m_carry);
    const uint32_t low = (p5.m_sum & ~32767u) +
        ((p5.m_carry + rows[7].m_carry) & ~32767u);
    const uint64_t full = static_cast<uint64_t>(ma) * mb;
    const uint64_t product = full - ((low ^ static_cast<uint32_t>(full)) & 32768u);
    const bool wide = (product >> 47u) != 0u;
    const int exponent = ea + eb - 127 + static_cast<int>(wide);
    if (exponent > 255)
    {
        return {sign | 0x7fffffffu, true, false};
    }
    if (exponent <= 0)
    {
        return {sign, false, true};
    }
    const uint32_t fraction = static_cast<uint32_t>(product >> (wide ? 24u : 23u)) & 0x7fffffu;
    return {sign | (static_cast<uint32_t>(exponent) << 23u) | fraction, false, false};
}

struct s_mac_result
{
    s_fmac_result m_final;
    uint32_t m_sticky;
};

inline s_mac_result mac(uint32_t acc, uint32_t a, uint32_t b,
                        bool subtract, bool acc_overflow)
{
    const auto product = mul(a, b);
    s_fmac_result result{};
    if (product.m_overflow)
    {
        result = {product.m_bits ^ (subtract ? 0x80000000u : 0u), true, false};
    }
    else if (acc_overflow)
    {
        result = {(acc & 0x80000000u) | 0x7fffffffu, true, false};
    }
    else
    {
        result = add(acc, product.m_bits, subtract);
    }
    uint32_t sticky = product.m_underflow ? 0x100u : 0u;
    if (!product.m_overflow && (acc_overflow || result.m_overflow || result.m_underflow))
    {
        if ((product.m_bits & 0x7f800000u) == 0u)
        {
            sticky |= 0x40u;
        }
        if ((product.m_bits & 0x80000000u) != 0u)
        {
            sticky |= 0x80u;
        }
    }
    return {result, sticky};
}
}
