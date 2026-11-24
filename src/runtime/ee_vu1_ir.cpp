#include "ee_vu1_ir.h"
#include "ee_vu1_dependency.h"
#include "ee_vu1_aot.h"

#include <utility>
#include <bitset>
#include <set>
#include <tuple>

namespace ee
{
namespace
{
uint32_t read_word(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
}

// Integer-only VU arithmetic matching ee_vu1_fp.h. No host FP,
// fast-math, or out-of-range shifts. These helpers are module-local.
constexpr const char* k_fp_ir = R"IR(
declare i32 @llvm.ctlz.i32(i32, i1 immarg)

define internal i32 @vu_flush(i32 %x) alwaysinline {
  %exp = and i32 %x, 2139095040
  %zero = icmp eq i32 %exp, 0
  %sign = and i32 %x, -2147483648
  %result = select i1 %zero, i32 %sign, i32 %x
  ret i32 %result
}

define internal i32 @vu_pack(i32 %sign, i32 %e, i32 %m) alwaysinline {
  %overflow = icmp sgt i32 %e, 255
  %underflow = icmp sle i32 %e, 0
  %exp = shl i32 %e, 23
  %mantissa = and i32 %m, 8388607
  %bits = or i32 %exp, %mantissa
  %normal = select i1 %underflow, i32 0, i32 %bits
  %mag = select i1 %overflow, i32 2147483647, i32 %normal
  %result = or i32 %sign, %mag
  ret i32 %result
}

define internal i32 @vu_add(i32 %a0, i32 %b0) alwaysinline {
  %a = call i32 @vu_flush(i32 %a0)
  %b = call i32 @vu_flush(i32 %b0)
  %am = and i32 %a, 2147483647
  %bm = and i32 %b, 2147483647
  %az = icmp eq i32 %am, 0
  %bz = icmp eq i32 %bm, 0
  br i1 %bz, label %zero_b, label %check_a
zero_b:
  %both_sign = and i32 %a, %b
  %zero_result = select i1 %az, i32 %both_sign, i32 %a
  ret i32 %zero_result
check_a:
  br i1 %az, label %zero_a, label %calc
zero_a:
  ret i32 %b
calc:
  %swap = icmp ugt i32 %bm, %am
  %large = select i1 %swap, i32 %b, i32 %a
  %small = select i1 %swap, i32 %a, i32 %b
  %large_mag = and i32 %large, 2147483647
  %small_mag = and i32 %small, 2147483647
  %e = lshr i32 %large_mag, 23
  %se = lshr i32 %small_mag, 23
  %diff = sub i32 %e, %se
  %far = icmp ugt i32 %diff, 25
  %safe_diff = select i1 %far, i32 0, i32 %diff
  %ma0 = and i32 %large_mag, 8388607
  %mb0 = and i32 %small_mag, 8388607
  %ma1 = or i32 %ma0, 8388608
  %mb1 = or i32 %mb0, 8388608
  %ma = shl i32 %ma1, 1
  %mb2 = shl i32 %mb1, 1
  %mb3 = lshr i32 %mb2, %safe_diff
  %mb = select i1 %far, i32 0, i32 %mb3
  %sign = and i32 %large, -2147483648
  %xor = xor i32 %large, %small
  %opposite = icmp slt i32 %xor, 0
  br i1 %opposite, label %subtract, label %same
subtract:
  %delta = sub i32 %ma, %mb
  %cancel = icmp eq i32 %delta, 0
  br i1 %cancel, label %cancelled, label %normalize
cancelled:
  ret i32 0
normalize:
  %lz = call i32 @llvm.ctlz.i32(i32 %delta, i1 false)
  %shift = sub i32 %lz, 7
  %norm = shl i32 %delta, %shift
  %ne = sub i32 %e, %shift
  %nm = lshr i32 %norm, 1
  %nr = call i32 @vu_pack(i32 %sign, i32 %ne, i32 %nm)
  ret i32 %nr
same:
  %sum = add i32 %ma, %mb
  %carry_bit = and i32 %sum, 33554432
  %carry = icmp ne i32 %carry_bit, 0
  %half = lshr i32 %sum, 1
  %adjust = select i1 %carry, i32 %half, i32 %sum
  %inc = zext i1 %carry to i32
  %ee = add i32 %e, %inc
  %mm = lshr i32 %adjust, 1
  %rr = call i32 @vu_pack(i32 %sign, i32 %ee, i32 %mm)
  ret i32 %rr
}

define internal i32 @vu_sub(i32 %a, i32 %b) alwaysinline {
  %neg = xor i32 %b, -2147483648
  %r = call i32 @vu_add(i32 %a, i32 %neg)
  ret i32 %r
}

define internal i32 @vu_mul(i32 %a, i32 %b) alwaysinline {
  %xor = xor i32 %a, %b
  %sign = and i32 %xor, -2147483648
  %ae = lshr i32 %a, 23
  %be = lshr i32 %b, 23
  %ea = and i32 %ae, 255
  %eb = and i32 %be, 255
  %az = icmp eq i32 %ea, 0
  %bz = icmp eq i32 %eb, 0
  %zero = or i1 %az, %bz
  br i1 %zero, label %signed_zero, label %calc
signed_zero:
  ret i32 %sign
calc:
  %a0 = and i32 %a, 8388607
  %b0 = and i32 %b, 8388607
  %a1 = or i32 %a0, 8388608
  %b1 = or i32 %b0, 8388608
  %a2 = zext i32 %a1 to i64
  %b2 = zext i32 %b1 to i64
  %product = mul i64 %a2, %b2
  %high = lshr i64 %product, 47
  %carry = icmp ne i64 %high, 0
  %shift = select i1 %carry, i64 24, i64 23
  %mantissa = lshr i64 %product, %shift
  %m = trunc i64 %mantissa to i32
  %e0 = add i32 %ea, %eb
  %e1 = sub i32 %e0, 127
  %inc = zext i1 %carry to i32
  %e = add i32 %e1, %inc
  %r = call i32 @vu_pack(i32 %sign, i32 %e, i32 %m)
  ret i32 %r
}

define internal i32 @vu_key(i32 %raw) alwaysinline {
  %x = call i32 @vu_flush(i32 %raw)
  %mag = and i32 %x, 2147483647
  %negative = icmp slt i32 %x, 0
  %neg = sub i32 0, %mag
  %r = select i1 %negative, i32 %neg, i32 %mag
  ret i32 %r
}

define internal i32 @vu_max(i32 %a, i32 %b) alwaysinline {
  %ak = call i32 @vu_key(i32 %a)
  %bk = call i32 @vu_key(i32 %b)
  %pick_a = icmp sge i32 %ak, %bk
  %r = select i1 %pick_a, i32 %a, i32 %b
  ret i32 %r
}

define internal i32 @vu_min(i32 %a, i32 %b) alwaysinline {
  %ak = call i32 @vu_key(i32 %a)
  %bk = call i32 @vu_key(i32 %b)
  %pick_a = icmp sle i32 %ak, %bk
  %r = select i1 %pick_a, i32 %a, i32 %b
  ret i32 %r
}

; Integer fixed-point conversion, matching vu1_fp::to_fixed.
; Clamp the exponent before shifting; LLVM overshifts are poison.
define internal i32 @vu_to_fixed(i32 %x, i32 %fraction) alwaysinline {
entry:
  %exp0 = lshr i32 %x, 23
  %exp = and i32 %exp0, 255
  %e = add i32 %exp, %fraction
  %denormal = icmp eq i32 %exp, 0
  %small = icmp sle i32 %e, 126
  %zero = or i1 %denormal, %small
  br i1 %zero, label %return_zero, label %check_large
return_zero:
  ret i32 0
check_large:
  %negative = icmp slt i32 %x, 0
  %large = icmp sge i32 %e, 158
  br i1 %large, label %saturate, label %convert
saturate:
  %limit = select i1 %negative, i32 -2147483648, i32 2147483647
  ret i32 %limit
convert:
  ; e is in [127,157], so shift is in [-23,7].
  %shift = sub i32 %e, 150
  %left = icmp sge i32 %shift, 0
  %negshift = sub i32 0, %shift
  %ls = select i1 %left, i32 %shift, i32 0
  %rs = select i1 %left, i32 0, i32 %negshift
  %m0 = and i32 %x, 8388607
  %m = or i32 %m0, 8388608
  %l = shl i32 %m, %ls
  %r = lshr i32 %l, %rs
  %neg = sub i32 0, %r
  %result = select i1 %negative, i32 %neg, i32 %r
  ret i32 %result
}

; Every signed i32 divided by a power of two is exactly representable
; as double. The reference from_double then truncates its mantissa.
; Normalize the unsigned magnitude directly, including INT32_MIN.
define internal i32 @vu_from_fixed(i32 %x, i32 %fraction) alwaysinline {
entry:
  %zero = icmp eq i32 %x, 0
  br i1 %zero, label %return_zero, label %convert
return_zero:
  ret i32 0
convert:
  %negative = icmp slt i32 %x, 0
  %neg = sub i32 0, %x
  %mag = select i1 %negative, i32 %neg, i32 %x
  %sign = and i32 %x, -2147483648
  %lz = call i32 @llvm.ctlz.i32(i32 %mag, i1 false)
  %high = sub i32 31, %lz
  %normalized = shl i32 %mag, %lz
  %mantissa = lshr i32 %normalized, 8
  %biased = add i32 %high, 127
  %e = sub i32 %biased, %fraction
  %result = call i32 @vu_pack(i32 %sign, i32 %e, i32 %mantissa)
  ret i32 %result
}

; Each ring stores the last eight writes in issue order, not timestamp
; order (FCSET and FSSET can publish retroactively visible values).
define internal void @vu_flag_push(ptr %head, ptr %times, ptr %values,
    i32 %time, i32 %value) alwaysinline {
  %h = load i32, ptr %head, align 4
  %index = and i32 %h, 7
  %tp = getelementptr i32, ptr %times, i32 %index
  %vp = getelementptr i32, ptr %values, i32 %index
  store i32 %time, ptr %tp, align 4
  store i32 %value, ptr %vp, align 4
  %next = add i32 %h, 1
  store i32 %next, ptr %head, align 4
  ret void
}

define internal i32 @vu_flag_read(ptr %head, ptr %times, ptr %values,
    i32 %now) alwaysinline {
entry:
  %h = load i32, ptr %head, align 4
  br label %scan
scan:
  %k = phi i32 [1, %entry], [%next, %scan]
  %offset = sub i32 %h, %k
  %index = and i32 %offset, 7
  %tp = getelementptr i32, ptr %times, i32 %index
  %ts = load i32, ptr %tp, align 4
  %ready = add i32 %ts, 4
  %visible = icmp ule i32 %ready, %now
  %oldest = icmp eq i32 %k, 8
  %done = or i1 %visible, %oldest
  %next = add i32 %k, 1
  br i1 %done, label %result, label %scan
result:
  %vp = getelementptr i32, ptr %values, i32 %index
  %value = load i32, ptr %vp, align 4
  ret i32 %value
}

; PS2 extended exponent values are finite in binary64. Match the
; interpreter's conversion and truncation rather than using binary32.
declare double @llvm.sqrt.f64(double)

define internal double @vu_to_double(i32 %x) alwaysinline {
  %e0 = lshr i32 %x, 23
  %e = and i32 %e0, 255
  %zero = icmp eq i32 %e, 0
  %sign0 = and i32 %x, -2147483648
  %sign1 = zext i32 %sign0 to i64
  %sign = shl i64 %sign1, 32
  %biased = add i32 %e, 896
  %e64 = zext i32 %biased to i64
  %exponent = shl i64 %e64, 52
  %m0 = and i32 %x, 8388607
  %m1 = zext i32 %m0 to i64
  %m = shl i64 %m1, 29
  %mag0 = or i64 %exponent, %m
  %mag = select i1 %zero, i64 0, i64 %mag0
  %bits = or i64 %sign, %mag
  %r = bitcast i64 %bits to double
  ret double %r
}

define internal i32 @vu_from_double(double %x) alwaysinline {
  %bits = bitcast double %x to i64
  %s64 = lshr i64 %bits, 32
  %s32 = trunc i64 %s64 to i32
  %sign = and i32 %s32, -2147483648
  %e64 = lshr i64 %bits, 52
  %e32 = trunc i64 %e64 to i32
  %e0 = and i32 %e32, 2047
  %e = sub i32 %e0, 896
  %m64 = lshr i64 %bits, 29
  %m = trunc i64 %m64 to i32
  %r = call i32 @vu_pack(i32 %sign, i32 %e, i32 %m)
  ret i32 %r
}

define internal i32 @vu_div(i32 %a0, i32 %b0) alwaysinline {
entry:
  %a = call i32 @vu_flush(i32 %a0)
  %b = call i32 @vu_flush(i32 %b0)
  %xor = xor i32 %a, %b
  %sign = and i32 %xor, -2147483648
  %bmag = and i32 %b, 2147483647
  %bz = icmp eq i32 %bmag, 0
  br i1 %bz, label %invalid, label %check_a
invalid:
  %limit = or i32 %sign, 2147483647
  ret i32 %limit
check_a:
  %amag = and i32 %a, 2147483647
  %az = icmp eq i32 %amag, 0
  br i1 %az, label %zero, label %calc
zero:
  ret i32 %sign
calc:
  %ad = call double @vu_to_double(i32 %a)
  %bd = call double @vu_to_double(i32 %b)
  %d = fdiv double %ad, %bd
  %r = call i32 @vu_from_double(double %d)
  ret i32 %r
}

define internal i32 @vu_sqrt(i32 %a0) alwaysinline {
  %a = and i32 %a0, 2147483647
  %d = call double @vu_to_double(i32 %a)
  %root = call double @llvm.sqrt.f64(double %d)
  %r = call i32 @vu_from_double(double %root)
  ret i32 %r
}

define internal i32 @vu_rsqrt(i32 %a0, i32 %b0) alwaysinline {
entry:
  %a = call i32 @vu_flush(i32 %a0)
  %b1 = call i32 @vu_flush(i32 %b0)
  %b = and i32 %b1, 2147483647
  %sign = and i32 %a, -2147483648
  %bz = icmp eq i32 %b, 0
  br i1 %bz, label %invalid, label %check_a
invalid:
  %limit = or i32 %sign, 2147483647
  ret i32 %limit
check_a:
  %amag = and i32 %a, 2147483647
  %az = icmp eq i32 %amag, 0
  br i1 %az, label %zero, label %calc
zero:
  ret i32 %sign
calc:
  %ad = call double @vu_to_double(i32 %a)
  %bd = call double @vu_to_double(i32 %b)
  %root = call double @llvm.sqrt.f64(double %bd)
  %d = fdiv double %ad, %root
  %r = call i32 @vu_from_double(double %d)
  ret i32 %r
}

; The same scalar pipeline machinery can be used for Q and P. Ready=0
; means no pending value; wait advances the total cycle counter only.
define internal void @vu_scalar_sync(ptr %current, ptr %ready, ptr %value,
    i32 %now) alwaysinline {
entry:
  %r = load i32, ptr %ready, align 4
  %pending = icmp ne i32 %r, 0
  %due = icmp uge i32 %now, %r
  %commit = and i1 %pending, %due
  br i1 %commit, label %publish, label %done
publish:
  %v = load i32, ptr %value, align 4
  store i32 %v, ptr %current, align 4
  store i32 0, ptr %ready, align 4
  br label %done
done:
  ret void
}

define internal void @vu_scalar_wait(ptr %current, ptr %ready, ptr %value,
    ptr %cycle) alwaysinline {
  %now = load i32, ptr %cycle, align 4
  %r = load i32, ptr %ready, align 4
  %stall = icmp ugt i32 %r, %now
  %elapsed = select i1 %stall, i32 %r, i32 %now
  store i32 %elapsed, ptr %cycle, align 4
  call void @vu_scalar_sync(ptr %current, ptr %ready, ptr %value, i32 %elapsed)
  ret void
}

define internal void @vu_scalar_schedule(ptr %current, ptr %ready, ptr %value,
    ptr %cycle, i32 %new_value, i32 %latency) alwaysinline {
  call void @vu_scalar_wait(ptr %current, ptr %ready, ptr %value, ptr %cycle)
  %now = load i32, ptr %cycle, align 4
  %at = add i32 %now, %latency
  store i32 %new_value, ptr %value, align 4
  store i32 %at, ptr %ready, align 4
  ret void
}
)IR";

constexpr uint32_t k_unknown_pc = 0xffffffffu;

bool is_branch(uint32_t lo, uint32_t hi)
{
    if ((hi & 0x80000000u) != 0u)
    {
        return false;
    }
    switch (lo >> 25u)
    {
    case 0x20u: case 0x21u: case 0x24u: case 0x25u:
    case 0x28u: case 0x29u: case 0x2cu: case 0x2du:
    case 0x2eu: case 0x2fu:
        return true;
    default:
        return false;
    }
}

bool is_flag_pair(uint32_t lo, uint32_t hi)
{
    const uint32_t funct = hi & 63u;
    const uint32_t index = (((hi >> 6u) & 31u) << 2u) | (funct & 3u);
    if (funct >= 0x3cu && index == 0x1fu)
    {
        return true;
    }
    if ((hi & 0x80000000u) != 0u)
    {
        return false;
    }
    const uint32_t major = lo >> 25u;
    return major >= 0x10u && major <= 0x1cu && major != 0x19u;
}

bool is_q_pair(uint32_t lo, uint32_t hi)
{
    if ((hi & 0x80000000u) != 0u || lo >> 25u != 0x40u ||
        (lo & 0x3cu) != 0x3cu)
    {
        return false;
    }
    const uint32_t index = (((lo >> 6u) & 31u) << 2u) | (lo & 3u);
    return index >= 0x38u && index <= 0x3bu;
}

bool is_efu_index(uint32_t index)
{
    return (index >= 0x70u && index <= 0x76u) ||
        (index >= 0x78u && index <= 0x7au) ||
        (index >= 0x7cu && index <= 0x7eu);
}

bool is_p_pair(uint32_t lo, uint32_t hi)
{
    if ((hi & 0x80000000u) != 0u || lo >> 25u != 0x40u ||
        (lo & 0x3cu) != 0x3cu)
    {
        return false;
    }
    const uint32_t index = (((lo >> 6u) & 31u) << 2u) | (lo & 3u);
    return is_efu_index(index) || index == 0x7bu || index == 0x64u;
}

bool is_kick_pair(uint32_t lo, uint32_t hi)
{
    return (hi & 0x80000000u) == 0u && lo >> 25u == 0x40u &&
        (lo & 0x3cu) == 0x3cu &&
        ((((lo >> 6u) & 31u) << 2u) | (lo & 3u)) == 0x6cu;
}

bool branch_delay_exempt(uint32_t lo)
{
    switch (lo >> 25u)
    {
    case 0x10u: case 0x12u: case 0x13u: case 0x14u:
    case 0x16u: case 0x17u: case 0x18u: case 0x1au:
    case 0x1bu: case 0x1cu: case 0x21u: case 0x25u:
        return true;
    default: return false;
    }
}

uint32_t relative_target(uint32_t pc, uint32_t lo)
{
    const uint32_t raw = lo & 0x7ffu;
    const int32_t offset = raw >= 1024u ?
        static_cast<int32_t>(raw) - 2048 : static_cast<int32_t>(raw);
    return (pc + 8u + static_cast<uint32_t>(offset) * 8u) & 0x3fffu;
}

class c_straight_line_emitter
{
public:
    c_straight_line_emitter()
    {
        m_ir = k_fp_ir;
        m_ir += "define dso_local i32 @ee_vu1_program(ptr %vi, ptr %i, "
            "ptr %tpc, i32 %top, i32 %itop, ptr %vf, ptr %q, "
            "ptr %mac, ptr %status, ptr %data, ptr %cycles_out, "
            "ptr %last_pc_out, ptr %acc, ptr %clip, ptr %kick, "
            "ptr %kick_context, ptr %p, ptr %r, ptr %efu) {\nentry:\n";
    }

    // The dynamic path uses the same per-pair arithmetic emitter as the
    // straight-line path. PC and delayed NPC are independent values, so
    // branches in delay slots and E termination obey interpreter order.
    void begin_dynamic(uint32_t start, const std::bitset<2048>& reached,
        bool flags, bool q, bool p)
    {
        m_dynamic = true;
        m_flag_pipeline = flags;
        m_q_pipeline = q;
        m_p_pipeline = p;
        if (p)
        {
            init_scalar("p");
        }
        if (q)
        {
            init_scalar("q");
        }
        if (flags)
        {
            init_ring("mac");
            init_ring("status");
            init_ring("clip");
        }
        line("%pc = alloca i32, align 4");
        line("%npc = alloca i32, align 4");
        line("%step_count = alloca i32, align 4");
        line("%cycle = alloca i32, align 4");
        line("%ending = alloca i1, align 1");
        line("%previous_lower = alloca i1, align 1");
        line("%previous_vi = alloca <16 x i16>, align 2");
        line("%ready = alloca [144 x i32], align 4");
        line("store [144 x i32] zeroinitializer, ptr %ready, align 4");
        line("store <16 x i16> zeroinitializer, ptr %previous_vi, align 2");
        line("store i32 " + std::to_string(start) + ", ptr %pc, align 4");
        line("store i32 " + std::to_string((start + 8u) & 0x3fffu) + ", ptr %npc, align 4");
        line("store i32 0, ptr %step_count, align 4");
        line("store i32 16, ptr %cycle, align 4");
        line("store i1 false, ptr %ending, align 1");
        line("store i1 false, ptr %previous_lower, align 1");
        line("br label %dispatch");
        m_ir += "dispatch:\n";
        line("%old_steps = load i32, ptr %step_count, align 4");
        line("%within_budget = icmp ult i32 %old_steps, " +
            std::to_string(c_ee_vu1::k_max_steps));
        line("br i1 %within_budget, label %select_pc, label %fail");
        m_ir += "select_pc:\n";
        line("%steps = add i32 %old_steps, 1");
        line("store i32 %steps, ptr %step_count, align 4");
        line("%current_pc = load i32, ptr %pc, align 4");
        line("%npc_now = load i32, ptr %npc, align 4");
        line("%ending_now = load i1, ptr %ending, align 1");
        line("%previous_lower_now = load i1, ptr %previous_lower, align 1");
        line("%previous_vi_now = load <16 x i16>, ptr %previous_vi, align 2");
        line("%before_vi = load <16 x i16>, ptr %vi, align 2");
        line("%sequential_raw = add i32 %npc_now, 8");
        line("%sequential = and i32 %sequential_raw, 16383");
        line("switch i32 %current_pc, label %fail [");
        for (uint32_t index = 0u; index < reached.size(); ++index)
        {
            if (reached.test(index))
            {
                line("  i32 " + std::to_string(index * 8u) +
                    ", label %pair_" + std::to_string(index * 8u));
            }
        }
        line("]");
    }

    bool emit_dynamic_pair(uint32_t pc, uint32_t lo, uint32_t hi)
    {
        m_ir += "pair_" + std::to_string(pc) + ":\n";
        m_current_pc = pc;
        m_next_target = "%sequential";

        // Identical dependency lists and four-cycle retire semantics to
        // s_ee_vu1_dependency. Readiness is local to this invocation.
        const auto use = s_ee_vu1_dependency::decode(lo, hi);
        const auto old = operation("load i32, ptr %cycle, align 4");
        auto cycle = operation("add i32 " + old + ", 1");
        for (uint32_t index = 0u; index < use.m_read_lane_count; ++index)
        {
            cycle = max_ready(cycle, use.m_read_lanes[index]);
        }
        for (uint32_t index = 0u; index < use.m_vi_index_count; ++index)
        {
            cycle = max_ready(cycle, 128u + use.m_vi_indices[index]);
        }
        line("store i32 " + cycle + ", ptr %cycle, align 4");
        if (m_q_pipeline)
        {
            sync_scalar("q", cycle);
        }
        if (m_p_pipeline)
        {
            sync_scalar("p", cycle);
        }
        if (m_flag_pipeline)
        {
            m_issue_cycle = cycle;
            m_flag_time = operation("add i32 %steps, 16");
        }
        if (!emit(lo, hi))
        {
            return false;
        }
        if (use.m_write_lane_count != 0u || use.m_vi_load != 0u)
        {
            const auto ready = operation("add i32 " + cycle + ", 4");
            for (uint32_t index = 0u; index < use.m_write_lane_count; ++index)
            {
                line("store i32 " + ready + ", ptr " +
                    ready_pointer(use.m_write_lanes[index]) + ", align 4");
            }
            if (use.m_vi_load != 0u)
            {
                line("store i32 " + ready + ", ptr " +
                    ready_pointer(128u + use.m_vi_load) + ", align 4");
            }
        }
        const bool previous_lower = (hi & 0x80000000u) == 0u &&
            !branch_delay_exempt(lo);
        line("store <16 x i16> %before_vi, ptr %previous_vi, align 2");
        line(std::string("store i1 ") + (previous_lower ? "true" : "false") +
            ", ptr %previous_lower, align 1");
        line("store i32 %npc_now, ptr %pc, align 4");
        line("store i32 " + m_next_target + ", ptr %npc, align 4");
        line(std::string("store i1 ") + ((hi & 0x40000000u) ? "true" : "false") +
            ", ptr %ending, align 1");
        line("br i1 %ending_now, label %done, label %dispatch");
        return true;
    }

    std::string finish_dynamic()
    {
        m_ir += "done:\n";
        if (m_q_pipeline)
        {
            wait_scalar("q");
        }
        if (m_p_pipeline)
        {
            wait_scalar("p");
        }
        const auto cycle = operation("load i32, ptr %cycle, align 4");
        const auto elapsed = operation("sub i32 " + cycle + ", 16");
        line("store i32 " + elapsed + ", ptr %cycles_out, align 4");
        line("store i32 %current_pc, ptr %last_pc_out, align 4");
        line("store i32 %npc_now, ptr %tpc, align 4");
        line("ret i32 %steps");
        m_ir += "fail:\n";
        line("ret i32 0");
        m_ir += "}\n";
        return m_ir;
    }

    bool emit(uint32_t lo, uint32_t hi)
    {
        // Compute upper operands before the lower executes, then
        // retire upper writes after lower writeback, as in the interpreter.
        m_upper_mask = 0u;
        m_upper_flags = false;
        m_upper_acc = false;
        m_pending_clip.clear();
        m_fsset = -1;
        if ((hi & 0x38000000u) != 0u || !emit_upper(hi) ||
            !emit_lower(lo, hi))
        {
            return false;
        }
        commit_upper();
        if (m_fsset >= 0)
        {
            const auto old = operation("load i32, ptr %status, align 4");
            const auto current = operation("and i32 " + old + ", 63");
            const auto status = operation("or i32 " + current + ", " +
                std::to_string(m_fsset));
            line("store i32 " + status + ", ptr %status, align 4");
            const auto time = operation("sub i32 " + m_flag_time + ", 4");
            push_ring("status", time, status);
            push_ring("mac", time, operation("load i32, ptr %mac, align 4"));
        }
        if (!m_pending_clip.empty())
        {
            line("store i32 " + m_pending_clip + ", ptr %clip, align 4");
            const auto now = (m_q_pipeline || m_p_pipeline) ?
                operation("load i32, ptr %cycle, align 4") : m_issue_cycle;
            push_ring("clip", now, m_pending_clip);
        }
        return true;
    }

    bool emit_lower(uint32_t lo, uint32_t hi)
    {
        if ((hi & 0x80000000u) != 0u)
        {
            line("store i32 " + std::to_string(lo) + ", ptr %i, align 4");
            return true;
        }
        if (lo == 0x8000033cu)
        {
            return true;
        }

        const uint32_t major = lo >> 25u;
        const uint32_t it = (lo >> 16u) & 15u;
        const uint32_t is = (lo >> 11u) & 15u;
        const uint32_t id = (lo >> 6u) & 15u;
        const uint32_t funct = lo & 63u;
        const uint32_t ft = (lo >> 16u) & 31u;
        const uint32_t fs = (lo >> 11u) & 31u;
        const uint32_t mask = (lo >> 21u) & 15u;
        const int32_t raw = static_cast<int32_t>(lo & 0x7ffu);
        const int32_t offset = raw >= 1024 ? raw - 2048 : raw;
        if (major == 0x00u || major == 0x01u ||
            major == 0x04u || major == 0x05u)
        {
            const auto address = data_address(major == 0x01u ? it : is, offset);
            switch (major)
            {
            case 0x00u: load_vector(ft, mask, address); break;
            case 0x01u: store_vector(fs, mask, address); break;
            case 0x04u: load_integer(it, mask, address, false); break;
            case 0x05u: store_integer(it, mask, address); break;
            }
            return true;
        }
        if (m_flag_pipeline && major >= 0x10u && major <= 0x1cu)
        {
            return emit_flag_lower(lo, major, is, it);
        }
        if (m_dynamic && is_branch(lo, hi))
        {
            return emit_branch(lo, major, is, it);
        }
        if (major == 0x08u || major == 0x09u)
        {
            const uint32_t imm = ((lo >> 10u) & 0x7800u) | (lo & 0x7ffu);
            binary(major == 0x08u ? "add" : "sub", it, is,
                std::to_string(imm));
            return true;
        }
        if (major != 0x40u)
        {
            return false;
        }
        switch (funct)
        {
        case 0x30u: binary("add", id, is, reg(it)); return true;
        case 0x31u: binary("sub", id, is, reg(it)); return true;
        case 0x34u: binary("and", id, is, reg(it)); return true;
        case 0x35u: binary("or", id, is, reg(it)); return true;
        case 0x32u:
        {
            const int32_t raw = static_cast<int32_t>((lo >> 6u) & 31u);
            binary("add", it, is, std::to_string(raw >= 16 ? raw - 32 : raw));
            return true;
        }
        default:
            break;
        }
        if (funct >= 0x3cu)
        {
            const uint32_t index = (((lo >> 6u) & 31u) << 2u) | (funct & 3u);
            if (index == 0x34u || index == 0x36u)
            {
                // LQD pre-decrements; LQI post-increments, including
                // suppressed VI0 updates and 16-bit integer wrapping.
                if (index == 0x36u)
                {
                    binary("sub", is, is, "1");
                }
                load_vector(ft, mask, data_address(is, 0));
                if (index == 0x34u)
                {
                    binary("add", is, is, "1");
                }
                return true;
            }
            if (index == 0x35u || index == 0x37u)
            {
                if (index == 0x37u)
                {
                    binary("sub", it, it, "1");
                }
                store_vector(fs, mask, data_address(it, 0));
                if (index == 0x35u)
                {
                    binary("add", it, it, "1");
                }
                return true;
            }
            if (m_q_pipeline && index >= 0x38u && index <= 0x3bu)
            {
                return emit_q_lower(lo, index, fs, ft);
            }
            if (index == 0x3eu)
            {
                load_integer(it, mask, data_address(is, 0), true);
                return true;
            }
            if (index == 0x3fu)
            {
                store_integer(it, mask, data_address(is, 0));
                return true;
            }
            if (index == 0x30u || index == 0x31u)
            {
                std::array<std::string, 4> values{};
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((mask & (8u >> lane)) != 0u)
                    {
                        values[lane] = vf(fs, index == 0x30u ? lane : (lane + 1u) & 3u);
                    }
                }
                write_vector(ft, mask, values);
                return true;
            }
            if (index == 0x3cu) // MTIR
            {
                store(it, operation("trunc i32 " + vf(fs, (lo >> 21u) & 3u) + " to i16"));
                return true;
            }
            if (index == 0x3du) // MFIR
            {
                const auto value = operation("sext i16 " + reg(is) + " to i32");
                const std::array<std::string, 4> values{value, value, value, value};
                write_vector(ft, mask, values);
                return true;
            }
            if (index >= 0x40u && index <= 0x43u)
            {
                return emit_random_lower(lo, index, fs, ft, mask);
            }
            if (m_p_pipeline && (is_efu_index(index) || index == 0x64u || index == 0x7bu))
            {
                return emit_p_lower(lo, index, fs, ft, mask);
            }
            if (m_dynamic && index == 0x6cu)
            {
                const auto address = operation("zext i16 " + reg(is) + " to i32");
                const auto result = operation("call i32 %kick(ptr %kick_context, i32 " +
                    address + ")");
                const auto ok = operation("icmp ne i32 " + result + ", 0");
                const auto label = "kick_ok_" + std::to_string(m_counter++);
                line("br i1 " + ok + ", label %" + label + ", label %fail");
                m_ir += label + ":\n";
                return true;
            }
            if (index == 0x68u || index == 0x69u)
            {
                const auto value = tmp();
                line(value + " = trunc i32 " +
                    (index == 0x68u ? "%top" : "%itop") + " to i16");
                store(it, value);
                return true;
            }
        }
        return false;
    }

    bool writes_data() const { return m_writes_data; }

    std::string finish(uint32_t steps, uint32_t cycles, uint32_t next_pc)
    {
        line("store i32 " + std::to_string(cycles) + ", ptr %cycles_out, align 4");
        line("store i32 " + std::to_string((next_pc - 8u) & 0x3fffu) +
            ", ptr %last_pc_out, align 4");
        line("store i32 " + std::to_string(next_pc) + ", ptr %tpc, align 4");
        line("ret i32 " + std::to_string(steps));
        m_ir += "}\n";
        return m_ir;
    }

private:
    void init_scalar(const std::string& name)
    {
        line("%" + name + "_ready = alloca i32, align 4");
        line("%" + name + "_pending = alloca i32, align 4");
        line("store i32 0, ptr %" + name + "_ready, align 4");
        line("store i32 0, ptr %" + name + "_pending, align 4");
    }

    std::string scalar_args(const std::string& name)
    {
        return "ptr %" + name + ", ptr %" + name + "_ready, ptr %" + name + "_pending";
    }

    void sync_scalar(const std::string& name, const std::string& cycle)
    {
        line("call void @vu_scalar_sync(" + scalar_args(name) + ", i32 " + cycle + ")");
    }

    void wait_scalar(const std::string& name)
    {
        line("call void @vu_scalar_wait(" + scalar_args(name) + ", ptr %cycle)");
    }

    void schedule_scalar(const std::string& name, const std::string& value, uint32_t latency)
    {
        line("call void @vu_scalar_schedule(" + scalar_args(name) + ", ptr %cycle, i32 " +
            value + ", i32 " + std::to_string(latency) + ")");
    }

    bool emit_random_lower(uint32_t lo, uint32_t index, uint32_t fs,
        uint32_t ft, uint32_t mask)
    {
        auto current = operation("load i32, ptr %r, align 4");
        if (index == 0x42u || index == 0x43u)
        {
            const auto source = vf(fs, (lo >> 21u) & 3u);
            const auto value = index == 0x42u ? source :
                operation("xor i32 " + current + ", " + source);
            const auto bits = operation("and i32 " + value + ", 8388607");
            line("store i32 " + bits + ", ptr %r, align 4");
            return true;
        }
        if (index == 0x40u)
        {
            const auto a = operation("lshr i32 " + current + ", 4");
            const auto b = operation("lshr i32 " + current + ", 22");
            const auto mix = operation("xor i32 " + a + ", " + b);
            const auto bit = operation("and i32 " + mix + ", 1");
            const auto shifted = operation("shl i32 " + current + ", 1");
            const auto combined = operation("or i32 " + shifted + ", " + bit);
            current = operation("and i32 " + combined + ", 8388607");
            line("store i32 " + current + ", ptr %r, align 4");
        }
        const auto bits = operation("and i32 " + current + ", 8388607");
        const auto value = operation("or i32 " + bits + ", 1065353216");
        const std::array<std::string, 4> values{value, value, value, value};
        write_vector(ft, mask, values);
        return true;
    }

    bool emit_p_lower(uint32_t lo, uint32_t index, uint32_t fs,
        uint32_t ft, uint32_t mask)
    {
        if (index == 0x64u)
        {
            const auto value = operation("load i32, ptr %p, align 4");
            const std::array<std::string, 4> values{value, value, value, value};
            write_vector(ft, mask, values);
            return true;
        }
        if (index == 0x7bu)
        {
            wait_scalar("p");
            return true;
        }
        // EFU math is a shared host callback, preserving the interpreter's
        // float normalization and platform math implementation. The P
        // dependency pipeline and destination writeback remain native IR.
        std::string call = "call i64 %efu(i32 " + std::to_string(index) +
            ", i32 " + std::to_string((lo >> 21u) & 3u);
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            call += ", i32 " + vf(fs, lane);
        }
        const auto result = operation(call + ")");
        const auto value = operation("trunc i64 " + result + " to i32");
        const auto latency64 = operation("lshr i64 " + result + ", 32");
        const auto latency = operation("trunc i64 " + latency64 + " to i32");
        const auto valid = operation("icmp ne i32 " + latency + ", 0");
        const auto label = "efu_ok_" + std::to_string(m_counter++);
        line("br i1 " + valid + ", label %" + label + ", label %fail");
        m_ir += label + ":\n";
        line("call void @vu_scalar_schedule(" + scalar_args("p") +
            ", ptr %cycle, i32 " + value + ", i32 " + latency + ")");
        return true;
    }

    bool emit_q_lower(uint32_t lo, uint32_t index, uint32_t fs, uint32_t ft)
    {
        if (index == 0x3bu)
        {
            wait_scalar("q");
            return true;
        }
        const auto rhs = vf(ft, (lo >> 23u) & 3u);
        if (index == 0x39u)
        {
            schedule_scalar("q", operation("call i32 @vu_sqrt(i32 " + rhs + ")"), 7u);
            return true;
        }
        const auto lhs = vf(fs, (lo >> 21u) & 3u);
        const auto value = fp_binary(index == 0x38u ? "vu_div" : "vu_rsqrt", lhs, rhs);
        schedule_scalar("q", value, index == 0x38u ? 7u : 13u);
        const auto exponent = operation("and i32 " + rhs + ", 2139095040");
        const auto invalid = operation("icmp eq i32 " + exponent + ", 0");
        const auto bit = operation("select i1 " + invalid + ", i32 32, i32 0");
        const auto old = operation("load i32, ptr %status, align 4");
        const auto cleared = operation("and i32 " + old + ", -49");
        const auto status = operation("or i32 " + cleared + ", " + bit);
        line("store i32 " + status + ", ptr %status, align 4");
        return true;
    }

    void init_ring(const std::string& name)
    {
        const auto prefix = "%" + name + "_ring_";
        line(prefix + "head = alloca i32, align 4");
        line(prefix + "times = alloca [8 x i32], align 4");
        line(prefix + "values = alloca [8 x i32], align 4");
        line("store i32 0, ptr " + prefix + "head, align 4");
        line("store [8 x i32] zeroinitializer, ptr " + prefix + "times, align 4");
        const auto initial = operation("load i32, ptr %" + name + ", align 4");
        const auto lane = operation("insertelement <8 x i32> poison, i32 " + initial + ", i32 0");
        const auto all = operation("shufflevector <8 x i32> " + lane +
            ", <8 x i32> poison, <8 x i32> zeroinitializer");
        line("store <8 x i32> " + all + ", ptr " + prefix + "values, align 4");
    }

    void push_ring(const std::string& name, const std::string& time,
        const std::string& value)
    {
        const auto prefix = "%" + name + "_ring_";
        line("call void @vu_flag_push(ptr " + prefix + "head, ptr " +
            prefix + "times, ptr " + prefix + "values, i32 " + time +
            ", i32 " + value + ")");
    }

    std::string read_ring(const std::string& name)
    {
        const auto prefix = "%" + name + "_ring_";
        return operation("call i32 @vu_flag_read(ptr " + prefix +
            "head, ptr " + prefix + "times, ptr " + prefix +
            "values, i32 " + (name == "clip" ? m_issue_cycle : m_flag_time) + ")");
    }

    void store_flag_result(uint32_t target, const std::string& value)
    {
        store(target, operation("trunc i32 " + value + " to i16"));
    }

    void store_flag_predicate(uint32_t target, const std::string& predicate)
    {
        store(target, operation("zext i1 " + predicate + " to i16"));
    }

    bool emit_flag_lower(uint32_t lo, uint32_t major, uint32_t is, uint32_t it)
    {
        const auto imm24 = std::to_string(lo & 0xffffffu);
        const uint32_t imm12 = ((lo >> 10u) & 0x800u) | (lo & 0x7ffu);
        const auto immediate = std::to_string(imm12);
        if (major == 0x11u)
        {
            line("store i32 " + imm24 + ", ptr %clip, align 4");
            push_ring("clip", operation("sub i32 " + m_flag_time + ", 4"), imm24);
            return true;
        }
        if (major == 0x15u)
        {
            m_fsset = static_cast<int32_t>(imm12 & 0xfc0u);
            return true;
        }
        if (major == 0x10u || major == 0x12u || major == 0x13u || major == 0x1cu)
        {
            const auto clip = operation("and i32 " + read_ring("clip") + ", 16777215");
            if (major == 0x10u)
            {
                store_flag_predicate(1u, operation("icmp eq i32 " + clip + ", " + imm24));
            }
            else if (major == 0x12u)
            {
                const auto bits = operation("and i32 " + clip + ", " + imm24);
                store_flag_predicate(1u, operation("icmp ne i32 " + bits + ", 0"));
            }
            else if (major == 0x13u)
            {
                const auto bits = operation("or i32 " + clip + ", " + imm24);
                store_flag_predicate(1u, operation("icmp eq i32 " + bits + ", 16777215"));
            }
            else
            {
                store_flag_result(it, operation("and i32 " + clip + ", 4095"));
            }
            return true;
        }
        if (major == 0x14u || major == 0x16u || major == 0x17u)
        {
            const auto status = operation("and i32 " + read_ring("status") + ", 4095");
            if (major == 0x14u)
            {
                store_flag_predicate(it, operation("icmp eq i32 " + status + ", " + immediate));
            }
            else
            {
                store_flag_result(it, operation(std::string(major == 0x16u ? "and" : "or") +
                    " i32 " + status + ", " + immediate));
            }
            return true;
        }
        if (major == 0x18u || major == 0x1au || major == 0x1bu)
        {
            const auto mac = operation("and i32 " + read_ring("mac") + ", 65535");
            const auto vi = operation("zext i16 " + reg(is) + " to i32");
            if (major == 0x18u)
            {
                store_flag_predicate(it, operation("icmp eq i32 " + mac + ", " + vi));
            }
            else
            {
                store_flag_result(it, operation(std::string(major == 0x1au ? "and" : "or") +
                    " i32 " + mac + ", " + vi));
            }
            return true;
        }
        return false;
    }

    std::string ready_pointer(uint32_t index)
    {
        return operation("getelementptr i32, ptr %ready, i32 " +
            std::to_string(index));
    }

    std::string max_ready(const std::string& cycle, uint32_t index)
    {
        const auto ready = operation("load i32, ptr " + ready_pointer(index) +
            ", align 4");
        const auto wait = operation("icmp ugt i32 " + ready + ", " + cycle);
        return operation("select i1 " + wait + ", i32 " + ready +
            ", i32 " + cycle);
    }

    std::string branch_vi(uint32_t index)
    {
        if (index == 0u)
        {
            return "0";
        }
        const auto previous = operation("extractelement <16 x i16> %previous_vi_now, i32 " +
            std::to_string(index));
        const auto current = operation("extractelement <16 x i16> %before_vi, i32 " +
            std::to_string(index));
        return operation("select i1 %previous_lower_now, i16 " + previous +
            ", i16 " + current);
    }

    bool emit_branch(uint32_t lo, uint32_t major, uint32_t is, uint32_t it)
    {
        if (major == 0x24u || major == 0x25u)
        {
            // JR/JALR read the current VI, not the delayed conditional
            // branch value. The switch guards unknown indirect targets.
            const auto target = operation("zext i16 " + reg(is) + " to i32");
            const auto bytes = operation("shl i32 " + target + ", 3");
            m_next_target = operation("and i32 " + bytes + ", 16383");
        }
        else
        {
            const auto target = std::to_string(relative_target(m_current_pc, lo));
            if (major == 0x20u || major == 0x21u)
            {
                m_next_target = target;
            }
            else
            {
                const char* predicate = nullptr;
                switch (major)
                {
                case 0x28u: predicate = "eq"; break;
                case 0x29u: predicate = "ne"; break;
                case 0x2cu: predicate = "slt"; break;
                case 0x2du: predicate = "sgt"; break;
                case 0x2eu: predicate = "sle"; break;
                case 0x2fu: predicate = "sge"; break;
                default: return false;
                }
                const auto lhs = branch_vi(is);
                const auto rhs = major == 0x28u || major == 0x29u ?
                    branch_vi(it) : "0";
                const auto taken = operation(std::string("icmp ") + predicate +
                    " i16 " + lhs + ", " + rhs);
                m_next_target = operation("select i1 " + taken + ", i32 " +
                    target + ", i32 %sequential");
            }
        }
        if (major == 0x21u || major == 0x25u)
        {
            store(it, std::to_string((m_current_pc + 16u) / 8u));
        }
        return true;
    }

    std::string operation(const std::string& instruction)
    {
        const auto value = tmp();
        line(value + " = " + instruction);
        return value;
    }

    std::string vf_pointer(uint32_t reg_index, uint32_t lane)
    {
        return operation("getelementptr i32, ptr %vf, i32 " +
            std::to_string(reg_index * 4u + lane));
    }

    std::string vf(uint32_t reg_index, uint32_t lane)
    {
        if (reg_index == 0u)
        {
            return lane == 3u ? "1065353216" : "0";
        }
        return operation("load i32, ptr " + vf_pointer(reg_index, lane) +
            ", align 4");
    }

    std::string data_address(uint32_t reg_index, int32_t offset)
    {
        const auto base = operation("zext i16 " + reg(reg_index) + " to i32");
        const auto sum = operation("add i32 " + base + ", " + std::to_string(offset));
        const auto wrapped = operation("and i32 " + sum + ", 1023");
        return operation("shl i32 " + wrapped + ", 4");
    }

    std::string data_pointer(const std::string& address, uint32_t lane)
    {
        const auto offset = operation("add i32 " + address + ", " +
            std::to_string(lane * 4u));
        return operation("getelementptr i8, ptr %data, i32 " + offset);
    }

    std::string data_lane(const std::string& address, uint32_t lane)
    {
        return operation("load i32, ptr " + data_pointer(address, lane) + ", align 1");
    }

    void store_data_lane(const std::string& address, uint32_t lane,
        const std::string& value)
    {
        line("store i32 " + value + ", ptr " + data_pointer(address, lane) +
            ", align 1");
        m_writes_data = true;
    }

    void store_vector(uint32_t source, uint32_t mask, const std::string& address)
    {
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) != 0u)
            {
                store_data_lane(address, lane, vf(source, lane));
            }
        }
    }

    void store_integer(uint32_t source, uint32_t mask, const std::string& address)
    {
        const auto value = operation("zext i16 " + reg(source) + " to i32");
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) != 0u)
            {
                store_data_lane(address, lane, value);
            }
        }
    }

    void write_vector(uint32_t target, uint32_t mask,
        const std::array<std::string, 4>& values)
    {
        if (target != 0u)
        {
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((mask & (8u >> lane)) != 0u)
                {
                    line("store i32 " + values[lane] + ", ptr " +
                        vf_pointer(target, lane) + ", align 4");
                }
            }
        }
    }

    void load_vector(uint32_t target, uint32_t mask, const std::string& address)
    {
        std::array<std::string, 4> values{};
        if (target != 0u)
        {
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((mask & (8u >> lane)) != 0u)
                {
                    values[lane] = data_lane(address, lane);
                }
            }
            write_vector(target, mask, values);
        }
    }

    void load_integer(uint32_t target, uint32_t mask, const std::string& address,
        bool first_lane)
    {
        // ILW uses the last selected lane, ILWR the first selected lane.
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) != 0u)
            {
                store(target, operation("trunc i32 " + data_lane(address, lane) + " to i16"));
                if (first_lane)
                {
                    break;
                }
            }
        }
    }

    void commit_upper()
    {
        if (m_upper_acc)
        {
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((m_upper_mask & (8u >> lane)) != 0u)
                {
                    line("store i32 " + m_upper_values[lane] + ", ptr " +
                        acc_pointer(lane) + ", align 4");
                }
            }
        }
        else
        {
            write_vector(m_upper_target, m_upper_mask, m_upper_values);
        }
        if (m_upper_flags)
        {
            emit_flags(m_upper_values, m_upper_mask);
        }
    }

    void emit_flags(const std::array<std::string, 4>& values, uint32_t mask)
    {
        std::string mac = "0";
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            const uint32_t bit = 8u >> lane;
            if ((mask & bit) == 0u)
            {
                continue;
            }
            const auto exp = operation("and i32 " + values[lane] + ", 2139095040");
            const auto zero = operation("icmp eq i32 " + exp + ", 0");
            const auto sign = operation("icmp slt i32 " + values[lane] + ", 0");
            const auto mag = operation("and i32 " + values[lane] + ", 2147483647");
            const auto overflow = operation("icmp eq i32 " + mag + ", 2147483647");
            for (const auto& test : {std::make_pair(zero, bit),
                                     std::make_pair(sign, bit << 4u),
                                     std::make_pair(overflow, bit << 12u)})
            {
                const auto flag = operation("select i1 " + test.first +
                    ", i32 " + std::to_string(test.second) + ", i32 0");
                mac = operation("or i32 " + mac + ", " + flag);
            }
        }
        line("store i32 " + mac + ", ptr %mac, align 4");
        const auto old = operation("load i32, ptr %status, align 4");
        auto status = operation("and i32 " + old + ", 4080");
        for (uint32_t group = 0u; group < 4u; ++group)
        {
            const auto bits = operation("and i32 " + mac + ", " +
                std::to_string(15u << (group * 4u)));
            const auto any = operation("icmp ne i32 " + bits + ", 0");
            const auto flag = operation("select i1 " + any + ", i32 " +
                std::to_string((1u << group) | (1u << (group + 6u))) + ", i32 0");
            status = operation("or i32 " + status + ", " + flag);
        }
        line("store i32 " + status + ", ptr %status, align 4");
        if (m_flag_pipeline)
        {
            push_ring("mac", m_flag_time, mac);
            push_ring("status", m_flag_time, status);
        }
    }

    std::string acc_pointer(uint32_t lane)
    {
        return operation("getelementptr i32, ptr %acc, i32 " +
            std::to_string(lane));
    }

    std::string acc(uint32_t lane)
    {
        return operation("load i32, ptr " + acc_pointer(lane) + ", align 4");
    }

    std::string fp_binary(const char* helper, const std::string& lhs,
        const std::string& rhs)
    {
        return operation(std::string("call i32 @") + helper +
            "(i32 " + lhs + ", i32 " + rhs + ")");
    }

    bool emit_upper(uint32_t hi)
    {
        const uint32_t funct = hi & 63u;
        const bool special = funct >= 0x3cu;
        const uint32_t op = special ?
            (((hi >> 6u) & 31u) << 2u) | (funct & 3u) : funct;
        const uint32_t mask = (hi >> 21u) & 15u;
        const uint32_t fs = (hi >> 11u) & 31u;
        const uint32_t ft = (hi >> 16u) & 31u;
        const uint32_t fd = (hi >> 6u) & 31u;
        if (special && op == 0x2fu)
        {
            return true;
        }

        const bool abs = special && op == 0x1du;
        const bool cross = op == 0x2eu;
        const bool conversion = special && op >= 0x10u && op <= 0x17u;
        if (special && op == 0x1fu)
        {
            if (!m_flag_pipeline)
            {
                return false;
            }
            const auto abs_w = operation("and i32 " + vf(ft, 3u) + ", 2147483647");
            const auto limit = operation("call i32 @vu_key(i32 " + abs_w + ")");
            const auto neg = operation("sub i32 0, " + limit);
            std::string bits = "0";
            for (uint32_t lane = 0u; lane < 3u; ++lane)
            {
                const auto x = operation("call i32 @vu_key(i32 " + vf(fs, lane) + ")");
                const auto above = operation("icmp sgt i32 " + x + ", " + limit);
                const auto below = operation("icmp slt i32 " + x + ", " + neg);
                const auto p = operation("select i1 " + above + ", i32 " +
                    std::to_string(1u << (lane * 2u)) + ", i32 0");
                const auto n = operation("select i1 " + below + ", i32 " +
                    std::to_string(2u << (lane * 2u)) + ", i32 0");
                bits = operation("or i32 " + bits + ", " + p);
                bits = operation("or i32 " + bits + ", " + n);
            }
            const auto old = operation("load i32, ptr %clip, align 4");
            const auto shifted = operation("shl i32 " + old + ", 6");
            const auto combined = operation("or i32 " + shifted + ", " + bits);
            m_pending_clip = operation("and i32 " + combined + ", 16777215");
            return true;
        }
        const bool madd = (op >= 0x08u && op <= 0x0bu) ||
            op == 0x21u || op == 0x23u || op == 0x29u;
        const bool msub = (op >= 0x0cu && op <= 0x0fu) ||
            op == 0x25u || op == 0x27u || op == 0x2du;
        const char* helper = nullptr;
        if (op <= 0x03u || op == 0x20u || op == 0x22u || op == 0x28u)
        {
            helper = "vu_add";
        }
        else if ((op >= 0x04u && op <= 0x07u) ||
            op == 0x24u || op == 0x26u || op == 0x2cu)
        {
            helper = "vu_sub";
        }
        else if ((op >= 0x18u && op <= 0x1cu) || op == 0x1eu || op == 0x2au)
        {
            helper = "vu_mul";
        }
        else if (!special && ((op >= 0x10u && op <= 0x13u) ||
            op == 0x1du || op == 0x2bu))
        {
            helper = "vu_max";
        }
        else if (!special && ((op >= 0x14u && op <= 0x17u) ||
            op == 0x1fu || op == 0x2fu))
        {
            helper = "vu_min";
        }
        if (!conversion && !abs && !cross && !madd && !msub && helper == nullptr)
        {
            return false;
        }
        const bool flags = !conversion && !abs && !(op >= 0x10u && op <= 0x17u) &&
            op != 0x1du && op != 0x1fu && op != 0x2bu && op != 0x2fu;
        const bool uses_q = op == 0x1cu || op == 0x20u || op == 0x21u ||
            op == 0x24u || op == 0x25u;
        const bool uses_i = op == 0x1du || op == 0x1eu || op == 0x1fu ||
            op == 0x22u || op == 0x23u || op == 0x26u || op == 0x27u;
        const uint32_t write_mask = cross ? 0xeu : mask;
        std::array<std::string, 4> values{};
        // Read every source before lower execution and upper writeback.
        // MADD/MSUB use separate PS2-rounded MUL and ADD/SUB, never FMA.
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((write_mask & (8u >> lane)) == 0u)
            {
                continue;
            }
            const auto lhs = vf(fs, cross ? (lane + 1u) % 3u : lane);
            if (conversion)
            {
                constexpr uint32_t fractions[]{0u, 4u, 12u, 15u};
                values[lane] = fp_binary(op <= 0x13u ? "vu_from_fixed" : "vu_to_fixed",
                    lhs, std::to_string(fractions[op & 3u]));
            }
            else if (abs)
            {
                values[lane] = operation("and i32 " + lhs + ", 2147483647");
            }
            else
            {
                const auto rhs = cross ? vf(ft, (lane + 2u) % 3u) :
                    op <= 0x1bu ? vf(ft, funct & 3u) :
                    uses_q ? operation("load i32, ptr %q, align 4") : uses_i ?
                    operation("load i32, ptr %i, align 4") : vf(ft, lane);
                if (cross || madd || msub)
                {
                    const auto product = fp_binary("vu_mul", lhs, rhs);
                    values[lane] = cross && special ? product :
                        fp_binary(madd ? "vu_add" : "vu_sub", acc(lane), product);
                }
                else
                {
                    values[lane] = fp_binary(helper, lhs, rhs);
                }
            }
        }
        m_upper_target = conversion || abs ? ft : fd;
        m_upper_mask = write_mask;
        m_upper_values = std::move(values);
        m_upper_flags = flags;
        m_upper_acc = special && !abs && !conversion;
        return true;
    }

    std::string tmp()
    {
        return "%v" + std::to_string(m_counter++);
    }

    void line(const std::string& text)
    {
        m_ir += "  " + text + "\n";
    }

    std::string pointer(uint32_t reg_index)
    {
        const auto result = tmp();
        line(result + " = getelementptr i16, ptr %vi, i32 " +
            std::to_string(reg_index));
        return result;
    }

    std::string reg(uint32_t reg_index)
    {
        if (reg_index == 0u)
        {
            return "0";
        }
        const auto address = pointer(reg_index);
        const auto value = tmp();
        line(value + " = load i16, ptr " + address + ", align 2");
        return value;
    }

    void store(uint32_t reg_index, const std::string& value)
    {
        if (reg_index != 0u)
        {
            const auto address = pointer(reg_index);
            line("store i16 " + value + ", ptr " + address + ", align 2");
        }
    }

    void binary(const char* op, uint32_t target, uint32_t source,
        const std::string& rhs)
    {
        if (target != 0u)
        {
            const auto lhs = reg(source);
            const auto result = tmp();
            // No nsw/nuw: VI arithmetic wraps at 16 bits.
            line(result + " = " + op + " i16 " + lhs + ", " + rhs);
            store(target, result);
        }
    }

    std::string m_ir;
    std::array<std::string, 4> m_upper_values{};
    uint32_t m_upper_target = 0u;
    uint32_t m_upper_mask = 0u;
    bool m_upper_flags = false;
    bool m_upper_acc = false;
    bool m_writes_data = false;
    bool m_dynamic = false;
    bool m_flag_pipeline = false;
    bool m_q_pipeline = false;
    bool m_p_pipeline = false;
    std::string m_flag_time;
    std::string m_issue_cycle;
    std::string m_pending_clip;
    int32_t m_fsset = -1;
    uint32_t m_current_pc = 0u;
    std::string m_next_target;
    uint32_t m_counter = 0u;
};
} // namespace

s_ee_vu1_ir_program vu1_lift_straight_line_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address)
{
    if ((address & 7u) != 0u)
    {
        return {};
    }
    c_straight_line_emitter emitter;
    s_ee_vu1_dependency dependencies{};
    uint32_t cycle = 16u;
    uint32_t pc = address & 0x3ff8u;
    bool ending = false;
    // A straight-line program must encounter E within one revolution
    // of micro memory, followed by its actual delay-slot pair.
    for (uint32_t steps = 1u; steps <= 2049u; ++steps)
    {
        const uint32_t lo = read_word(code.data() + pc);
        const uint32_t hi = read_word(code.data() + pc + 4u);
        if (is_flag_pair(lo, hi) || is_q_pair(lo, hi) ||
            is_p_pair(lo, hi) || is_kick_pair(lo, hi) || !emitter.emit(lo, hi))
        {
            return {};
        }
        // With no branches, Q/P producers or dynamic waits, the
        // same scoreboard can calculate all issue cycles at lift time.
        const auto use = s_ee_vu1_dependency::decode(lo, hi);
        cycle = dependencies.issue(use, cycle + 1u);
        dependencies.retire(use, cycle);
        pc = (pc + 8u) & 0x3fffu;
        if (ending)
        {
            return {emitter.finish(steps, cycle - 16u, pc), steps, cycle - 16u, pc,
                emitter.writes_data()};
        }
        ending = (hi & 0x40000000u) != 0u;
    }
    return {};
}
s_ee_vu1_ir_program vu1_lift_program_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address)
{
    auto straight = vu1_lift_straight_line_ir(code, address);
    if (!straight.m_ir.empty() || (address & 7u) != 0u)
    {
        return straight;
    }
    // Discover direct control-flow with independent PC/NPC and E-delay
    // state. For indirect branches compile the known return continuations
    // and delay slots; an unknown target exits through the native guard.
    using t_visit = std::tuple<uint32_t, uint32_t, bool>;
    std::set<t_visit> visited;
    std::vector<t_visit> queue;
    std::bitset<2048> reached;
    const uint32_t start = address & 0x3ff8u;
    const auto enqueue = [&](uint32_t pc, uint32_t npc, bool ending)
    {
        if (pc != k_unknown_pc && visited.emplace(pc, npc, ending).second)
        {
            queue.emplace_back(pc, npc, ending);
        }
    };
    enqueue(start, (start + 8u) & 0x3fffu, false);
    bool has_end = false;
    bool has_branch = false;
    bool has_flags = false;
    bool has_q = false;
    bool has_kick = false;
    bool has_p = false;
    for (size_t index = 0u; index < queue.size(); ++index)
    {
        // Avoid pathological branch-in-delay graphs at lift time.
        if (queue.size() > 16384u)
        {
            return {};
        }
        const auto [pc, npc, ending] = queue[index];
        reached.set(pc >> 3u);
        const uint32_t lo = read_word(code.data() + pc);
        const uint32_t hi = read_word(code.data() + pc + 4u);
        const bool branch = is_branch(lo, hi);
        has_branch |= branch;
        has_flags |= is_flag_pair(lo, hi);
        has_q |= is_q_pair(lo, hi);
        has_kick |= is_kick_pair(lo, hi);
        has_p |= is_p_pair(lo, hi);
        if (ending)
        {
            has_end = true;
            continue;
        }
        const bool next_end = (hi & 0x40000000u) != 0u;
        const uint32_t sequential = npc == k_unknown_pc ?
            k_unknown_pc : (npc + 8u) & 0x3fffu;
        if (!branch)
        {
            enqueue(npc, sequential, next_end);
            continue;
        }
        const uint32_t major = lo >> 25u;
        if (major == 0x21u || major == 0x25u)
        {
            // Link continuation is a possible indirect return target.
            enqueue((pc + 16u) & 0x3fffu, (pc + 24u) & 0x3fffu, false);
        }
        if (major == 0x24u || major == 0x25u)
        {
            enqueue(npc, k_unknown_pc, next_end);
            continue;
        }
        enqueue(npc, relative_target(pc, lo), next_end);
        if (major != 0x20u && major != 0x21u)
        {
            enqueue(npc, sequential, next_end);
        }
    }
    if ((!has_branch && !has_flags && !has_q && !has_kick && !has_p) || !has_end)
    {
        return {};
    }
    c_straight_line_emitter emitter;
    emitter.begin_dynamic(start, reached, has_flags, has_q, has_p);
    for (uint32_t index = 0u; index < reached.size(); ++index)
    {
        if (!reached.test(index))
        {
            continue;
        }
        const uint32_t pc = index * 8u;
        if (!emitter.emit_dynamic_pair(pc, read_word(code.data() + pc),
                read_word(code.data() + pc + 4u)))
        {
            return {};
        }
    }
    // The full image remains in library metadata, but the loader only
    // needs to compare instruction bytes for emitted switch cases.
    return {emitter.finish_dynamic(), 0u, 0u, 0u, emitter.writes_data(), reached};
}

std::string vu1_aot_module_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address, bool windows_exports)
{
    auto program = vu1_lift_program_ir(code, address);
    if (program.m_ir.empty())
    {
        return {};
    }
    const uint32_t start = address & 0x3ff8u;
    const std::string linkage = windows_exports ? "dso_local dllexport " : "dso_local ";
    if (windows_exports)
    {
        const auto at = program.m_ir.find("define dso_local i32 @ee_vu1_program");
        program.m_ir.replace(at, sizeof("define dso_local ") - 1u,
            "define " + linkage);
        // COFF codegen references the MSVC floating-point marker when
        // DIV/SQRT/RSQRT lower to host FP. These DLLs intentionally have
        // no CRT or entry point; supply the marker locally in each DLL.
        program.m_ir += "\n@_fltused = dso_local global i32 9875, align 4\n";
    }
    program.m_ir += "\n@vu1_metadata = private constant [5 x i32] [i32 " +
        std::to_string(k_vu1_aot_abi) + ", i32 " + std::to_string(start) +
        ", i32 " + std::to_string(program.m_steps) + ", i32 " +
        std::to_string(program.m_cycles) + ", i32 " +
        std::to_string(program.m_next_pc) + "]\n";
    const uint32_t size = program.m_steps == 0u ?
        c_ee_vu1::k_memory_size : program.m_steps * 8u;
    program.m_ir += "@vu1_code = private constant [" +
        std::to_string(size) + " x i8] c\"";
    constexpr char hex[] = "0123456789abcdef";
    for (uint32_t i = 0u; i < size; ++i)
    {
        const uint8_t byte = code[(start + i) & 0x3fffu];
        program.m_ir += '\\';
        program.m_ir += hex[byte >> 4u];
        program.m_ir += hex[byte & 15u];
    }
    program.m_ir += "\"\n";
    program.m_ir += "define " + linkage +
        "ptr @ee_vu1_aot_info() { ret ptr @vu1_metadata }\n";
    program.m_ir += "define " + linkage +
        "ptr @ee_vu1_aot_code() { ret ptr @vu1_code }\n";
    return program.m_ir;
}
} // namespace ee
