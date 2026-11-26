#pragma once
#include "ee_scalar_constant.h"

namespace ee::scalar_constant
{
// Immutable facts about one i64 SSA value, not a guest register across
// dispatcher entries. Disjoint masks; word means sext(trunc32(value)).
struct s_known_bits
{
    uint64_t m_zero = 0;
    uint64_t m_one = 0;
    bool m_word = false;

    constexpr std::optional<uint64_t> constant() const
    {
        return (m_zero | m_one) == UINT64_MAX
            ? std::optional<uint64_t>{m_one} : std::nullopt;
    }

    constexpr bool empty() const
    {
        return m_zero == 0 && m_one == 0 && !m_word;
    }

    constexpr std::optional<uint64_t> masked_constant(uint64_t mask) const
    {
        return ((m_zero | m_one) & mask) == mask
            ? std::optional<uint64_t>{m_one & mask} : std::nullopt;
    }
};

constexpr s_known_bits exact_bits(uint64_t value)
{
    return {~value, value, sign_extend<32>(value) == value};
}

constexpr s_known_bits word_bits(s_known_bits value)
{
    constexpr uint64_t low = low_mask<32>();
    constexpr uint64_t sign = uint64_t{1} << 31;
    return {(value.m_zero & low) | ((value.m_zero & sign) ? ~low : 0),
            (value.m_one & low) | ((value.m_one & sign) ? ~low : 0), true};
}

constexpr s_known_bits unsigned_bits(unsigned width)
{
    return {width == 64 ? 0 : UINT64_MAX << width, 0, width < 32};
}

constexpr s_known_bits common_bits(s_known_bits a, s_known_bits b)
{
    return {a.m_zero & b.m_zero, a.m_one & b.m_one, a.m_word && b.m_word};
}

constexpr s_known_bits not_bits(s_known_bits a)
{
    return {a.m_one, a.m_zero, a.m_word};
}

constexpr s_known_bits logical_bits(e_op op, s_known_bits a, s_known_bits b)
{
    switch (op)
    {
    case e_op::bit_and: case e_op::andi:
        return {a.m_zero | b.m_zero, a.m_one & b.m_one, a.m_word && b.m_word};
    case e_op::bit_or: case e_op::ori:
        return {a.m_zero & b.m_zero, a.m_one | b.m_one, a.m_word && b.m_word};
    case e_op::bit_xor: case e_op::xori:
        return {(a.m_zero & b.m_zero) | (a.m_one & b.m_one),
                (a.m_zero & b.m_one) | (a.m_one & b.m_zero), a.m_word && b.m_word};
    case e_op::bit_nor:
        return not_bits(logical_bits(e_op::bit_or, a, b));
    default:
        return {};
    }
}

// Signed ordering is unsigned ordering after toggling the sign bit.
// Intervals are conservative even with sparse masks or word correlation.
constexpr std::optional<uint64_t> less_bits(s_known_bits a, s_known_bits b,
                                            bool signed_compare)
{
    if (signed_compare)
    {
        constexpr uint64_t sign = uint64_t{1} << 63;
        a = logical_bits(e_op::bit_xor, a, exact_bits(sign));
        b = logical_bits(e_op::bit_xor, b, exact_bits(sign));
    }
    if (~a.m_zero < b.m_one)
    {
        return uint64_t{1};
    }
    if (a.m_one >= ~b.m_zero)
    {
        return uint64_t{0};
    }
    return std::nullopt;
}

constexpr s_known_bits shift_bits(s_known_bits a, unsigned count,
                                  bool word, bool left, bool arithmetic)
{
    const unsigned width = word ? 32u : 64u;
    const uint64_t mask = word ? low_mask<32>() : UINT64_MAX;
    count &= width - 1u;
    a.m_zero &= mask;
    a.m_one &= mask;
    if (count != 0)
    {
        if (left)
        {
            a.m_zero = ((a.m_zero << count) | ((uint64_t{1} << count) - 1)) & mask;
            a.m_one = (a.m_one << count) & mask;
        }
        else
        {
            const uint64_t fill = mask ^ (mask >> count);
            const uint64_t sign = uint64_t{1} << (width - 1u);
            a.m_zero = (a.m_zero >> count) |
                ((!arithmetic || (a.m_zero & sign)) ? fill : 0);
            a.m_one = (a.m_one >> count) |
                ((arithmetic && (a.m_one & sign)) ? fill : 0);
        }
    }
    // A full-width shift need not preserve sign-extension correlation.
    a.m_word = false;
    return word ? word_bits(a) : a;
}
}
