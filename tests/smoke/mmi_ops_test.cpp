#include "mmi_ops.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace
{
constexpr void expect_u128(const char* name, const ee::s_ee_u128& actual,
                           const ee::s_ee_u128& expected)
{
    if (actual.m_lo != expected.m_lo || actual.m_hi != expected.m_hi)
    {
        // A failing constant expression reports the offending call site.
        throw name;
    }
}
constexpr uint64_t get_lane(const ee::s_ee_u128& value, unsigned width, unsigned index)
{
    const unsigned bit = width * index;
    const uint64_t word = bit < 64 ? value.m_lo : value.m_hi;
    const unsigned shift = bit & 63u;
    return (word >> shift) & ((1ull << width) - 1ull);
}
constexpr void put_lane(ee::s_ee_u128& value, unsigned width, unsigned index, uint64_t lane)
{
    const unsigned bit = width * index;
    uint64_t& word = bit < 64 ? value.m_lo : value.m_hi;
    const unsigned shift = bit & 63u;
    const uint64_t mask = ((1ull << width) - 1ull) << shift;
    word = (word & ~mask) | ((lane << shift) & mask);
}
constexpr int64_t signed_lane(uint64_t raw, unsigned width)
{
    const uint64_t sign = 1ull << (width - 1);
    return (raw & sign) ? (int64_t)raw - (1ll << width) : (int64_t)raw;
}
enum class e_lane_op { abs_sat, equal, min_signed, add_unsigned_sat, sub_unsigned_sat };
constexpr ee::s_ee_u128 expected_lanes(const ee::s_ee_u128& a,
                             const ee::s_ee_u128& b,
                             unsigned width,
                             e_lane_op operation)
{
    ee::s_ee_u128 result{0, 0};
    const unsigned count = 128 / width;
    const uint64_t mask = (1ull << width) - 1ull;
    for (unsigned i = 0; i < count; ++i)
    {
        const uint64_t lhs = get_lane(a, width, i);
        const uint64_t rhs = get_lane(b, width, i);
        uint64_t value = 0;
        switch (operation)
        {
        case e_lane_op::abs_sat:
        {
            const int64_t signed_lhs = signed_lane(lhs, width);
            const int64_t min_value = -(1ll << (width - 1));
            const int64_t max_value = (1ll << (width - 1)) - 1;
            value = signed_lhs == min_value ? (uint64_t)max_value :
                (uint64_t)(signed_lhs < 0 ? -signed_lhs : signed_lhs);
            break;
        }
        case e_lane_op::equal: value = lhs == rhs ? mask : 0; break;
        case e_lane_op::min_signed:
            value = (uint64_t)(signed_lane(lhs, width) < signed_lane(rhs, width)
                ? signed_lane(lhs, width) : signed_lane(rhs, width));
            break;
        case e_lane_op::add_unsigned_sat: value = lhs > mask - rhs ? mask : lhs + rhs; break;
        case e_lane_op::sub_unsigned_sat: value = lhs > rhs ? lhs - rhs : 0; break;
        }
        put_lane(result, width, i, value & mask);
    }
    return result;
}
constexpr ee::s_ee_u128 expected_padsbh(const ee::s_ee_u128& a, const ee::s_ee_u128& b)
{
    ee::s_ee_u128 result{0, 0};
    for (unsigned i = 0; i < 8; ++i)
    {
        const uint16_t lhs = (uint16_t)get_lane(a, 16, i);
        const uint16_t rhs = (uint16_t)get_lane(b, 16, i);
        put_lane(result, 16, i, i < 4 ? (uint16_t)(lhs - rhs) : (uint16_t)(lhs + rhs));
    }
    return result;
}
constexpr ee::s_ee_u128 expected_pext(const ee::s_ee_u128& a,const ee::s_ee_u128& b,unsigned width)
{
    ee::s_ee_u128 result{0, 0};
    const unsigned half = (128 / width) / 2;
    for (unsigned i = 0; i < half; ++i)
    {
        put_lane(result, width, i * 2, get_lane(b, width, half + i));
        put_lane(result, width, i * 2 + 1, get_lane(a, width, half + i));
    }
    return result;
}
constexpr ee::s_ee_u128 expected_qfsrv(const ee::s_ee_u128& a,const ee::s_ee_u128& b,unsigned shift)
{
    shift &= 0x7fu;
    if (shift == 0) return b;
    ee::s_ee_u128 result{};
    if (shift < 64)
    {
        result.m_lo = (b.m_lo >> shift) | (b.m_hi << (64 - shift));
        result.m_hi = (b.m_hi >> shift) | (a.m_lo << (64 - shift));
        return result;
    }
    const unsigned upper_shift = shift - 64;
    if (upper_shift == 0) { result.m_lo = b.m_hi; result.m_hi = a.m_lo; return result; }
    result.m_lo = (b.m_hi >> upper_shift) | (a.m_lo << (64 - upper_shift));
    result.m_hi = (a.m_lo >> upper_shift) | (a.m_hi << (64 - upper_shift));
    return result;
}

consteval bool test_mmi_ops()
{
    const ee::s_ee_u128 a{0x800000007fffffffull, 0x12345678ffffffffull};
    const ee::s_ee_u128 b{0x800000007fffffffull, 0x8765432100000002ull};
    expect_u128("PABSW", ee::mmi_apply(ee::LOP_MMI_ABS_W, a, b, 0), expected_lanes(b,b,32,e_lane_op::abs_sat));
    expect_u128("PABSH", ee::mmi_apply(ee::LOP_MMI_ABS_H, a, b, 0), expected_lanes(b,b,16,e_lane_op::abs_sat));
    expect_u128("PCEQW", ee::mmi_apply(ee::LOP_MMI_CEQ_W, a, b, 0), expected_lanes(a,b,32,e_lane_op::equal));
    expect_u128("PCEQH", ee::mmi_apply(ee::LOP_MMI_CEQ_H, a, b, 0), expected_lanes(a,b,16,e_lane_op::equal));
    expect_u128("PCEQB", ee::mmi_apply(ee::LOP_MMI_CEQ_B, a, b, 0), expected_lanes(a,b,8,e_lane_op::equal));
    expect_u128("PADSBH", ee::mmi_apply(ee::LOP_MMI_ADDSUB_H, a, b, 0), expected_padsbh(a,b));
    expect_u128("PEXTUW", ee::mmi_apply(ee::LOP_MMI_EXTU_W, a, b, 0), expected_pext(a,b,32));
    expect_u128("PEXTUH", ee::mmi_apply(ee::LOP_MMI_EXTU_H, a, b, 0), expected_pext(a,b,16));
    expect_u128("PEXTUB", ee::mmi_apply(ee::LOP_MMI_EXTU_B, a, b, 0), expected_pext(a,b,8));
    expect_u128("PADDUW", ee::mmi_apply(ee::LOP_MMI_ADDU_SAT_W, a, b, 0), expected_lanes(a,b,32,e_lane_op::add_unsigned_sat));
    expect_u128("PADDUH", ee::mmi_apply(ee::LOP_MMI_ADDU_SAT_H, a, b, 0), expected_lanes(a,b,16,e_lane_op::add_unsigned_sat));
    expect_u128("PADDUB", ee::mmi_apply(ee::LOP_MMI_ADDU_SAT_B, a, b, 0), expected_lanes(a,b,8,e_lane_op::add_unsigned_sat));
    expect_u128("PSUBUW", ee::mmi_apply(ee::LOP_MMI_SUBU_SAT_W, a, b, 0), expected_lanes(a,b,32,e_lane_op::sub_unsigned_sat));
    expect_u128("PSUBUH", ee::mmi_apply(ee::LOP_MMI_SUBU_SAT_H, a, b, 0), expected_lanes(a,b,16,e_lane_op::sub_unsigned_sat));
    expect_u128("PSUBUB", ee::mmi_apply(ee::LOP_MMI_SUBU_SAT_B, a, b, 0), expected_lanes(a,b,8,e_lane_op::sub_unsigned_sat));
    for (unsigned shift : {0u,8u,40u,120u})
        expect_u128("QFSRV", ee::mmi_apply(ee::LOP_MMI_QFSRV,a,b,(uint8_t)shift), expected_qfsrv(a,b,shift));
    const ee::s_ee_u128 copy_a{
        0x1122334455667788ull, 0x99aabbccddeeff01ull
    };
    const ee::s_ee_u128 copy_b{
        0xfedcba9876543210ull, 0x0123456789abcdefull
    };
    expect_u128(
        "PCPYLD ordered halves",
        ee::mmi_apply(ee::LOP_MMI_COPY_LOWER_D, copy_a, copy_b, 0),
        { copy_b.m_lo, copy_a.m_lo }
    );
    expect_u128(
        "PCPYLD same operands",
        ee::mmi_apply(ee::LOP_MMI_COPY_LOWER_D, copy_a, copy_a, 0),
        { copy_a.m_lo, copy_a.m_lo }
    );
    expect_u128(
        "PCPYH independent halves",
        ee::mmi_apply(ee::LOP_MMI_COPY_H, copy_a, copy_b, 0),
        { 0x3210321032103210ull, 0xcdefcdefcdefcdefull }
    );
    expect_u128(
        "PCPYH all ones",
        ee::mmi_apply(
            ee::LOP_MMI_COPY_H, copy_a,
            { 0xffffffffffffffffull, 0xffffffffffffffffull }, 0
        ),
        { 0xffffffffffffffffull, 0xffffffffffffffffull }
    );
    expect_u128("SLL shift 0", ee::mmi_apply(ee::LOP_MMI_SLL,a,b,0), b);
    expect_u128("SRL shift 0", ee::mmi_apply(ee::LOP_MMI_SRL,a,b,0), b);
    return true;
}

static_assert(test_mmi_ops(), "packed MMI semantic regression");

// Cross-check every legal QFSRV shift, including the 64-bit boundary,
// against a per-bit reference rather than duplicating the implementation.
consteval bool test_qfsrv_all_shifts()
{
    constexpr ee::s_ee_u128 a{0x1122334455667788ull, 0x99aabbccddeeff01ull};
    constexpr ee::s_ee_u128 b{0xfedcba9876543210ull, 0x0123456789abcdefull};
    constexpr uint64_t words[] = {b.m_lo, b.m_hi, a.m_lo, a.m_hi};
    for (unsigned shift = 0u; shift < 128u; ++shift)
    {
        ee::s_ee_u128 expected{};
        for (unsigned bit = 0u; bit < 128u; ++bit)
        {
            const unsigned source = bit + shift;
            const uint64_t value = (words[source / 64u] >> (source % 64u)) & 1u;
            uint64_t& half = bit < 64u ? expected.m_lo : expected.m_hi;
            half |= value << (bit % 64u);
        }
        expect_u128("QFSRV bit reference",
            ee::mmi_apply(ee::LOP_MMI_QFSRV, a, b, static_cast<uint8_t>(shift)),
            expected);
    }
    return true;
}

static_assert(test_qfsrv_all_shifts(), "QFSRV full shift range regression");
}

int main()
{
    std::puts("mmi_ops_test: compile-time fixtures passed");
    return 0;
}
