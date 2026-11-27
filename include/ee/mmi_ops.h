#pragma once
#include "ee_instruction.h"
#include "ee_state.h"
#include "instruction_lifter.h"

namespace ee
{

// MMI (multimedia/SIMD) semantics operating on the portable two-half 128-bit
// GPR representation (m_lo = bits[0:63], m_hi = bits[64:127], little-endian
// packed lanes). These are pure functions: given the source operands they
// return the destination 128-bit value. Both the reference interpreter and the
// native mirror use them so their MMI semantics stay consistent; correctness is
// validated against hand-computed expected values in the differential tests.

namespace
{
static inline constexpr uint8_t  mmi_get8 (const s_ee_u128& v, int i){ return (uint8_t)((i<8?v.m_lo:v.m_hi)>>((i&7)*8)); }
static inline constexpr uint16_t mmi_get16(const s_ee_u128& v, int i){ return (uint16_t)((i<4?v.m_lo:v.m_hi)>>((i&3)*16)); }
static inline constexpr uint32_t mmi_get32(const s_ee_u128& v, int i){ return (uint32_t)((i<2?v.m_lo:v.m_hi)>>((i&1)*32)); }
static inline constexpr uint64_t mmi_get64(const s_ee_u128& v, int i){ return i==0?v.m_lo:v.m_hi; }

static inline constexpr s_ee_u128 mmi_put8 (s_ee_u128 v, int i, uint8_t  x){ uint64_t& w = i<8?v.m_lo:v.m_hi; int s=(i&7)*8; w=(w&~(0xffULL<<s))|((uint64_t)x<<s); return v; }
static inline constexpr s_ee_u128 mmi_put16(s_ee_u128 v, int i, uint16_t x){ uint64_t& w = i<4?v.m_lo:v.m_hi; int s=(i&3)*16; w=(w&~(0xffffULL<<s))|((uint64_t)x<<s); return v; }
static inline constexpr s_ee_u128 mmi_put32(s_ee_u128 v, int i, uint32_t x){ uint64_t& w = i<2?v.m_lo:v.m_hi; int s=(i&1)*32; w=(w&~(0xffffffffULL<<s))|((uint64_t)x<<s); return v; }
static inline constexpr s_ee_u128 mmi_put64(s_ee_u128 v, int i, uint64_t x){ if(i==0)v.m_lo=x;else v.m_hi=x; return v; }

static inline constexpr uint8_t  mmi_sat8u (uint32_t v){ return v>0xff?0xff:(uint8_t)v; }
static inline constexpr uint16_t mmi_sat16u(uint32_t v){ return v>0xffff?0xffff:(uint16_t)v; }
static inline constexpr uint32_t mmi_sat32u(uint64_t v){ return v>0xffffffffULL?0xffffffffu:(uint32_t)v; }

static inline constexpr s_ee_u128 mmi_shr128(const s_ee_u128& v, int n)
{
    n &= 127;
    if (n == 0)
        return v;
    s_ee_u128 r;
    if (n < 64) { r.m_hi = v.m_hi >> n; r.m_lo = (v.m_lo >> n) | (v.m_hi << (64 - n)); }
    else { int m = n - 64; r.m_hi = 0; r.m_lo = (m < 64) ? (v.m_hi >> m) : 0; }
    return r;
}
static inline constexpr s_ee_u128 mmi_shl128(const s_ee_u128& v, int n)
{
    n &= 127;
    if (n == 0)
        return v;
    s_ee_u128 r;
    if (n < 64) { r.m_lo = v.m_lo << n; r.m_hi = (v.m_hi << n) | (v.m_lo >> (64 - n)); }
    else { int m = n - 64; r.m_lo = 0; r.m_hi = (m < 64) ? (v.m_lo << m) : 0; }
    return r;
}
static inline constexpr s_ee_u128 mmi_sar128(const s_ee_u128& v, int n)
{
    n &= 127;
    if (n == 0)
        return v;
    s_ee_u128 r;
    uint64_t sign = (uint64_t)(int64_t)v.m_hi >> 63;
    if (n < 64) { r.m_hi = (uint64_t)(int64_t)v.m_hi >> n; r.m_lo = (v.m_lo >> n) | ((uint64_t)(int64_t)v.m_hi << (64 - n)); }
    else { int m = n - 64; r.m_hi = sign ? ~0ULL : 0; r.m_lo = (m < 64) ? (uint64_t)(int64_t)v.m_hi >> m : (sign ? ~0ULL : 0); }
    return r;
} // close mmi_sar128
} // namespace

// Apply one MMI packed op. a = rs operand, b = rt operand, sa = shift amount.
inline constexpr s_ee_u128 mmi_apply(e_lift_op_kind kind, const s_ee_u128& a, const s_ee_u128& b, uint8_t sa)
{
    s_ee_u128 r{};
    switch (kind)
    {
    case LOP_MMI_ADD_B:
        for (int i = 0; i < 16; ++i) r = mmi_put8(r, i, (uint8_t)((uint32_t)mmi_get8(a,i) + mmi_get8(b,i)));
        return r;
    case LOP_MMI_ADD_H:
        for (int i = 0; i < 8; ++i) r = mmi_put16(r, i, (uint16_t)((uint32_t)mmi_get16(a,i) + mmi_get16(b,i)));
        return r;
    case LOP_MMI_ADD_W:
        for (int i = 0; i < 4; ++i) r = mmi_put32(r, i, mmi_get32(a,i) + mmi_get32(b,i));
        return r;
    case LOP_MMI_ADD_D:
        return mmi_put64(mmi_put64(r, 0, mmi_get64(a,0) + mmi_get64(b,0)), 1, mmi_get64(a,1) + mmi_get64(b,1));
    case LOP_MMI_SUB_B:
        for (int i = 0; i < 16; ++i) r = mmi_put8(r, i, (uint8_t)((uint32_t)mmi_get8(a,i) - mmi_get8(b,i)));
        return r;
    case LOP_MMI_SUB_H:
        for (int i = 0; i < 8; ++i) r = mmi_put16(r, i, (uint16_t)((uint32_t)mmi_get16(a,i) - mmi_get16(b,i)));
        return r;
    case LOP_MMI_SUB_W:
        for (int i = 0; i < 4; ++i) r = mmi_put32(r, i, mmi_get32(a,i) - mmi_get32(b,i));
        return r;
    case LOP_MMI_SUB_D:
        return mmi_put64(mmi_put64(r, 0, mmi_get64(a,0) - mmi_get64(b,0)), 1, mmi_get64(a,1) - mmi_get64(b,1));
    case LOP_MMI_ADDU_SAT_B:
        for (int i = 0; i < 16; ++i) r = mmi_put8(r, i, mmi_sat8u((uint32_t)mmi_get8(a,i) + mmi_get8(b,i)));
        return r;
    case LOP_MMI_ADDU_SAT_H:
        for (int i = 0; i < 8; ++i) r = mmi_put16(r, i, mmi_sat16u((uint32_t)mmi_get16(a,i) + mmi_get16(b,i)));
        return r;
    case LOP_MMI_ADDU_SAT_W:
        for (int i = 0; i < 4; ++i) r = mmi_put32(r, i, mmi_sat32u((uint64_t)mmi_get32(a,i) + mmi_get32(b,i)));
        return r;
    case LOP_MMI_AND:
        return { a.m_lo & b.m_lo, a.m_hi & b.m_hi };
    case LOP_MMI_OR:
        return { a.m_lo | b.m_lo, a.m_hi | b.m_hi };
    case LOP_MMI_XOR:
        return { a.m_lo ^ b.m_lo, a.m_hi ^ b.m_hi };
    case LOP_MMI_NOR:
        return { ~(a.m_lo | b.m_lo), ~(a.m_hi | b.m_hi) };
    case LOP_MMI_COPY_H:
        // PCPYH is unary: replicate the bottom halfword of each
        // source doubleword independently across that doubleword.
        return {
            (b.m_lo & 0xffffull) * 0x0001000100010001ull,
            (b.m_hi & 0xffffull) * 0x0001000100010001ull
        };
    case LOP_MMI_COPY_LOWER_D:
        // PCPYLD: low64 = rt.low64; high64 = rs.low64.
        return { b.m_lo, a.m_lo };
    case LOP_MMI_SLL:
        return mmi_shl128(b, (int)sa);
    case LOP_MMI_SRL:
        return mmi_shr128(b, (int)sa);
    case LOP_MMI_SRA:
        return mmi_sar128(b, (int)sa);
    case LOP_MMI_MAXU_B:
        for (int i = 0; i < 16; ++i) r = mmi_put8(r, i, mmi_get8(a,i) > mmi_get8(b,i) ? mmi_get8(a,i) : mmi_get8(b,i));
        return r;
    case LOP_MMI_MAXU_H:
        for (int i = 0; i < 8; ++i) r = mmi_put16(r, i, mmi_get16(a,i) > mmi_get16(b,i) ? mmi_get16(a,i) : mmi_get16(b,i));
        return r;
    case LOP_MMI_MAXU_W:
        for (int i = 0; i < 4; ++i) r = mmi_put32(r, i, mmi_get32(a,i) > mmi_get32(b,i) ? mmi_get32(a,i) : mmi_get32(b,i));
        return r;
    case LOP_MMI_MAXS_B:
        for (int i = 0; i < 16; ++i) r = mmi_put8(r, i, (int8_t)mmi_get8(a,i) > (int8_t)mmi_get8(b,i) ? mmi_get8(a,i) : mmi_get8(b,i));
        return r;
    case LOP_MMI_MAXS_H:
        for (int i = 0; i < 8; ++i) r = mmi_put16(r, i, (int16_t)mmi_get16(a,i) > (int16_t)mmi_get16(b,i) ? mmi_get16(a,i) : mmi_get16(b,i));
        return r;
    case LOP_MMI_MAXS_W:
        for (int i = 0; i < 4; ++i) r = mmi_put32(r, i, (int32_t)mmi_get32(a,i) > (int32_t)mmi_get32(b,i) ? mmi_get32(a,i) : mmi_get32(b,i));
        return r;
    case LOP_MMI_MINU_B:
        for (int i = 0; i < 16; ++i) r = mmi_put8(r, i, mmi_get8(a,i) < mmi_get8(b,i) ? mmi_get8(a,i) : mmi_get8(b,i));
        return r;
    case LOP_MMI_MINU_H:
        for (int i = 0; i < 8; ++i) r = mmi_put16(r, i, mmi_get16(a,i) < mmi_get16(b,i) ? mmi_get16(a,i) : mmi_get16(b,i));
        return r;
    case LOP_MMI_MINU_W:
        for (int i = 0; i < 4; ++i) r = mmi_put32(r, i, mmi_get32(a,i) < mmi_get32(b,i) ? mmi_get32(a,i) : mmi_get32(b,i));
        return r;
    case LOP_MMI_MINS_B:
        for (int i = 0; i < 16; ++i) r = mmi_put8(r, i, (int8_t)mmi_get8(a,i) < (int8_t)mmi_get8(b,i) ? mmi_get8(a,i) : mmi_get8(b,i));
        return r;
    case LOP_MMI_MINS_H:
        for (int i = 0; i < 8; ++i) r = mmi_put16(r, i, (int16_t)mmi_get16(a,i) < (int16_t)mmi_get16(b,i) ? mmi_get16(a,i) : mmi_get16(b,i));
        return r;
    case LOP_MMI_MINS_W:
        for (int i = 0; i < 4; ++i) r = mmi_put32(r, i, (int32_t)mmi_get32(a,i) < (int32_t)mmi_get32(b,i) ? mmi_get32(a,i) : mmi_get32(b,i));
        return r;
    case LOP_MMI_ABS_W:
        for (int i = 0; i < 4; ++i)
        {
            const int32_t value = (int32_t)mmi_get32(b, i);
            const int32_t result = value == (-2147483647 - 1)
                ? 2147483647 : (value < 0 ? -value : value);
            r = mmi_put32(r, i, (uint32_t)result);
        }
        return r;
    case LOP_MMI_ABS_H:
        for (int i = 0; i < 8; ++i)
        {
            const int16_t value = (int16_t)mmi_get16(b, i);
            const int16_t result = value == (-32767 - 1)
                ? 32767 : (value < 0 ? (int16_t)-value : value);
            r = mmi_put16(r, i, (uint16_t)result);
        }
        return r;
    case LOP_MMI_CEQ_W:
        for (int i = 0; i < 4; ++i)
            r = mmi_put32(r, i, mmi_get32(a, i) == mmi_get32(b, i) ? 0xffffffffu : 0u);
        return r;
    case LOP_MMI_CEQ_H:
        for (int i = 0; i < 8; ++i)
            r = mmi_put16(r, i, mmi_get16(a, i) == mmi_get16(b, i) ? 0xffffu : 0u);
        return r;
    case LOP_MMI_CEQ_B:
        for (int i = 0; i < 16; ++i)
            r = mmi_put8(r, i, mmi_get8(a, i) == mmi_get8(b, i) ? 0xffu : 0u);
        return r;
    case LOP_MMI_ADDSUB_H:
        for (int i = 0; i < 8; ++i)
        {
            const uint16_t lhs = mmi_get16(a, i);
            const uint16_t rhs = mmi_get16(b, i);
            r = mmi_put16(r, i, i < 4 ? (uint16_t)(lhs - rhs) : (uint16_t)(lhs + rhs));
        }
        return r;
    case LOP_MMI_EXTU_W:
        for (int i = 0; i < 2; ++i)
        {
            r = mmi_put32(r, i * 2, mmi_get32(b, i + 2));
            r = mmi_put32(r, i * 2 + 1, mmi_get32(a, i + 2));
        }
        return r;
    case LOP_MMI_EXTU_H:
        for (int i = 0; i < 4; ++i)
        {
            r = mmi_put16(r, i * 2, mmi_get16(b, i + 4));
            r = mmi_put16(r, i * 2 + 1, mmi_get16(a, i + 4));
        }
        return r;
    case LOP_MMI_EXTU_B:
        for (int i = 0; i < 8; ++i)
        {
            r = mmi_put8(r, i * 2, mmi_get8(b, i + 8));
            r = mmi_put8(r, i * 2 + 1, mmi_get8(a, i + 8));
        }
        return r;
    case LOP_MMI_SUBU_SAT_W:
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t lhs = mmi_get32(a, i);
            const uint32_t rhs = mmi_get32(b, i);
            r = mmi_put32(r, i, lhs > rhs ? lhs - rhs : 0u);
        }
        return r;
    case LOP_MMI_SUBU_SAT_H:
        for (int i = 0; i < 8; ++i)
        {
            const uint16_t lhs = mmi_get16(a, i);
            const uint16_t rhs = mmi_get16(b, i);
            r = mmi_put16(r, i, lhs > rhs ? (uint16_t)(lhs - rhs) : 0u);
        }
        return r;
    case LOP_MMI_SUBU_SAT_B:
        for (int i = 0; i < 16; ++i)
        {
            const uint8_t lhs = mmi_get8(a, i);
            const uint8_t rhs = mmi_get8(b, i);
            r = mmi_put8(r, i, lhs > rhs ? (uint8_t)(lhs - rhs) : 0u);
        }
        return r;
    case LOP_MMI_QFSRV:
    {
        // SA is a bit count produced by MTSAB/MTSAH.
        const unsigned shift = (unsigned)sa & 0x7fu;
        if (shift == 0)
            return b;
        if (shift < 64)
        {
            r.m_lo = (b.m_lo >> shift) | (b.m_hi << (64 - shift));
            r.m_hi = (b.m_hi >> shift) | (a.m_lo << (64 - shift));
            return r;
        }
        const unsigned upper_shift = shift - 64;
        if (upper_shift == 0)
        {
            r.m_lo = b.m_hi;
            r.m_hi = a.m_lo;
            return r;
        }
        r.m_lo = (b.m_hi >> upper_shift) | (a.m_lo << (64 - upper_shift));
        r.m_hi = (a.m_lo >> upper_shift) | (a.m_hi << (64 - upper_shift));
        return r;
    }
    case LOP_MMI_SHUFFLE:
        for (int i = 0; i < 16; ++i) r = mmi_put8(r, i, mmi_get8(b, mmi_get8(a,i) & 0xF));
        return r;
    case LOP_MMI_PACK:
        for (int i = 0; i < 8; ++i) {
            r = mmi_put8(r, i, mmi_sat8u(mmi_get16(b,i)));
            r = mmi_put8(r, 8 + i, mmi_sat8u(mmi_get16(a,i)));
        }
        return r;
    default:
        return { 0, 0 };
    }
}

} // namespace ee
