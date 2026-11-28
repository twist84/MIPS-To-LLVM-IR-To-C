#pragma once

// Four-lane integer FMAC. Mirrors the scalar EE/VU arithmetic; potentially
// observable Booth corrections use an exact vector cold path.
namespace llvm
{
inline constexpr char k_vu_fmac4_ir[] = R"IR(
declare <4 x i32> @llvm.umax.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.umin.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.abs.v4i32(<4 x i32>, i1 immarg)

; The two aligned 25-bit significands sum to a magnitude below 2^26.
; For n >= 4, removing the two low bits preserves its highest set bit
; and makes it exactly representable as binary32. For n < 4, keep n.
; The conversion is exact under every host rounding mode; only its
; exponent is used for counting bits, never for guest FP arithmetic.
; This avoids the long packed ctlz expansion on AVX2 hosts.
define internal <4 x i32> @vu_clz26_4(<4 x i32> %magnitude) alwaysinline {
entry:
  %high = and <4 x i32> %magnitude, <i32 -4, i32 -4, i32 -4, i32 -4>
  %low = and <4 x i32> %magnitude, <i32 3, i32 3, i32 3, i32 3>
  %exact = call <4 x i32> @llvm.umax.v4i32(<4 x i32> %high, <4 x i32> %low)
  %encoded = sitofp <4 x i32> %exact to <4 x float>
  %bits = bitcast <4 x float> %encoded to <4 x i32>
  %exp = lshr <4 x i32> %bits, <i32 23, i32 23, i32 23, i32 23>
  %raw = sub <4 x i32> <i32 158, i32 158, i32 158, i32 158>, %exp
  %leading = call <4 x i32> @llvm.umin.v4i32(<4 x i32> %raw, <4 x i32> <i32 32, i32 32, i32 32, i32 32>)
  ret <4 x i32> %leading
}

define internal { <4 x i32>, <4 x i1>, <4 x i1> } @vu_add4(<4 x i32> %lhs, <4 x i32> %rhs, i1 %subtract) alwaysinline {
entry:
  %subtract_sign = select i1 %subtract, <4 x i32> <i32 -2147483648, i32 -2147483648, i32 -2147483648, i32 -2147483648>, <4 x i32> <i32 0, i32 0, i32 0, i32 0>
  %b = xor <4 x i32> %rhs, %subtract_sign
  %ea_shift = lshr <4 x i32> %lhs, <i32 23, i32 23, i32 23, i32 23>
  %eb_shift = lshr <4 x i32> %b, <i32 23, i32 23, i32 23, i32 23>
  %ea = and <4 x i32> %ea_shift, <i32 255, i32 255, i32 255, i32 255>
  %eb = and <4 x i32> %eb_shift, <i32 255, i32 255, i32 255, i32 255>
  %a_zero = icmp eq <4 x i32> %ea, <i32 0, i32 0, i32 0, i32 0>
  %b_zero = icmp eq <4 x i32> %eb, <i32 0, i32 0, i32 0, i32 0>
  %a_fraction = and <4 x i32> %lhs, <i32 8388607, i32 8388607, i32 8388607, i32 8388607>
  %b_fraction = and <4 x i32> %b, <i32 8388607, i32 8388607, i32 8388607, i32 8388607>
  %a_hidden = or <4 x i32> %a_fraction, <i32 8388608, i32 8388608, i32 8388608, i32 8388608>
  %b_hidden = or <4 x i32> %b_fraction, <i32 8388608, i32 8388608, i32 8388608, i32 8388608>
  %a_guard = shl <4 x i32> %a_hidden, <i32 1, i32 1, i32 1, i32 1>
  %b_guard = shl <4 x i32> %b_hidden, <i32 1, i32 1, i32 1, i32 1>
  %ma = select <4 x i1> %a_zero, <4 x i32> <i32 0, i32 0, i32 0, i32 0>, <4 x i32> %a_guard
  %mb = select <4 x i1> %b_zero, <4 x i32> <i32 0, i32 0, i32 0, i32 0>, <4 x i32> %b_guard
  ; Exponents are unsigned bytes. The alignment shifts saturate at
  ; 25, which discards the entire 25-bit guarded significand.
  %e = call <4 x i32> @llvm.umax.v4i32(<4 x i32> %ea, <4 x i32> %eb)
  %da = sub <4 x i32> %e, %ea
  %db = sub <4 x i32> %e, %eb
  %sa = call <4 x i32> @llvm.umin.v4i32(<4 x i32> %da, <4 x i32> <i32 25, i32 25, i32 25, i32 25>)
  %sb = call <4 x i32> @llvm.umin.v4i32(<4 x i32> %db, <4 x i32> <i32 25, i32 25, i32 25, i32 25>)
  %aligned_a = lshr <4 x i32> %ma, %sa
  %aligned_b = lshr <4 x i32> %mb, %sb
  %a_negative = icmp slt <4 x i32> %lhs, <i32 0, i32 0, i32 0, i32 0>
  %b_negative = icmp slt <4 x i32> %b, <i32 0, i32 0, i32 0, i32 0>
  %neg_a = sub <4 x i32> <i32 0, i32 0, i32 0, i32 0>, %aligned_a
  %neg_b = sub <4 x i32> <i32 0, i32 0, i32 0, i32 0>, %aligned_b
  %signed_a = select <4 x i1> %a_negative, <4 x i32> %neg_a, <4 x i32> %aligned_a
  %signed_b = select <4 x i1> %b_negative, <4 x i32> %neg_b, <4 x i32> %aligned_b
  %sum = add <4 x i32> %signed_a, %signed_b
  %zero = icmp eq <4 x i32> %sum, <i32 0, i32 0, i32 0, i32 0>
  ; The sum is strictly between -2^26 and 2^26. Packed abs is
  ; exact, including zero, and cannot encounter INT_MIN.
  %magnitude = call <4 x i32> @llvm.abs.v4i32(<4 x i32> %sum, i1 false)
  %leading = call <4 x i32> @vu_clz26_4(<4 x i32> %magnitude)
  %safe_shift = and <4 x i32> %leading, <i32 31, i32 31, i32 31, i32 31>
  %normalized = shl <4 x i32> %magnitude, %safe_shift
  %significand = lshr <4 x i32> %normalized, <i32 8, i32 8, i32 8, i32 8>
  %fraction = and <4 x i32> %significand, <i32 8388607, i32 8388607, i32 8388607, i32 8388607>
  %offset = sub <4 x i32> <i32 7, i32 7, i32 7, i32 7>, %leading
  %exponent = add <4 x i32> %e, %offset
  %above = icmp sgt <4 x i32> %exponent, <i32 255, i32 255, i32 255, i32 255>
  %below = icmp slt <4 x i32> %exponent, <i32 1, i32 1, i32 1, i32 1>
  %nonzero = xor <4 x i1> %zero, <i1 true, i1 true, i1 true, i1 true>
  %overflow = and <4 x i1> %above, %nonzero
  %underflow = and <4 x i1> %below, %nonzero
  ; A zero sum has a negative sign only for two negative inputs.
  ; For a nonzero sum its sign bit is already in the integer sum.
  %both_sign = and <4 x i32> %lhs, %b
  %sign_source = select <4 x i1> %zero, <4 x i32> %both_sign, <4 x i32> %sum
  %sign = and <4 x i32> %sign_source, <i32 -2147483648, i32 -2147483648, i32 -2147483648, i32 -2147483648>
  %exp_bits = shl <4 x i32> %exponent, <i32 23, i32 23, i32 23, i32 23>
  %normal = or <4 x i32> %exp_bits, %fraction
  %bounded = select <4 x i1> %overflow, <4 x i32> <i32 2147483647, i32 2147483647, i32 2147483647, i32 2147483647>, <4 x i32> %normal
  ; A cancellation underflow retains the normalized fraction with an
  ; exponent of zero. Only an exactly zero sum loses the fraction.
  %range_result = select <4 x i1> %underflow, <4 x i32> %fraction, <4 x i32> %bounded
  %absolute = select <4 x i1> %zero, <4 x i32> <i32 0, i32 0, i32 0, i32 0>, <4 x i32> %range_result
  %bits = or <4 x i32> %absolute, %sign
  %out0 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1> } poison, <4 x i32> %bits, 0
  %out1 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1> } %out0, <4 x i1> %overflow, 1
  %out2 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1> } %out1, <4 x i1> %underflow, 2
  ret { <4 x i32>, <4 x i1>, <4 x i1> } %out2
}

define internal { <4 x i32>, <4 x i32> } @vu_mul_booth_row4(<4 x i32> %a, <4 x i32> %code, i32 %shift) alwaysinline {
entry:
  %shift_seed = insertelement <4 x i32> poison, i32 %shift, i32 0
  %shift_vector = shufflevector <4 x i32> %shift_seed, <4 x i32> poison, <4 x i32> zeroinitializer
  %position_scalar = shl i32 1, %shift
  %position_seed = insertelement <4 x i32> poison, i32 %position_scalar, i32 0
  %position = shufflevector <4 x i32> %position_seed, <4 x i32> poison, <4 x i32> zeroinitializer
  %base = shl <4 x i32> %a, %shift_vector
  %twice = shl <4 x i32> %base, <i32 1, i32 1, i32 1, i32 1>
  %double_pos = icmp eq <4 x i32> %code, <i32 3, i32 3, i32 3, i32 3>
  %double_neg = icmp eq <4 x i32> %code, <i32 4, i32 4, i32 4, i32 4>
  %double = or <4 x i1> %double_pos, %double_neg
  %magnitude = select <4 x i1> %double, <4 x i32> %twice, <4 x i32> %base
  %at_least_four = icmp uge <4 x i32> %code, <i32 4, i32 4, i32 4, i32 4>
  %at_most_six = icmp ule <4 x i32> %code, <i32 6, i32 6, i32 6, i32 6>
  %negative = and <4 x i1> %at_least_four, %at_most_six
  %mask = sub <4 x i32> zeroinitializer, %position
  %inverted = xor <4 x i32> %magnitude, %mask
  %signed = select <4 x i1> %negative, <4 x i32> %inverted, <4 x i32> %magnitude
  %nonzero = icmp ne <4 x i32> %code, zeroinitializer
  %active = and <4 x i1> %nonzero, %at_most_six
  %row = select <4 x i1> %active, <4 x i32> %signed, <4 x i32> zeroinitializer
  %carry = select <4 x i1> %negative, <4 x i32> %position, <4 x i32> zeroinitializer
  %out0 = insertvalue { <4 x i32>, <4 x i32> } poison, <4 x i32> %row, 0
  %out1 = insertvalue { <4 x i32>, <4 x i32> } %out0, <4 x i32> %carry, 1
  ret { <4 x i32>, <4 x i32> } %out1
}

define internal { <4 x i32>, <4 x i32> } @vu_mul_csa4(<4 x i32> %a, <4 x i32> %b, <4 x i32> %c) alwaysinline {
entry:
  %parity = xor <4 x i32> %a, %b
  %sum = xor <4 x i32> %parity, %c
  %ab = and <4 x i32> %a, %b
  %pc = and <4 x i32> %parity, %c
  %majority = or <4 x i32> %ab, %pc
  %carry = shl <4 x i32> %majority, <i32 1, i32 1, i32 1, i32 1>
  %out0 = insertvalue { <4 x i32>, <4 x i32> } poison, <4 x i32> %sum, 0
  %out1 = insertvalue { <4 x i32>, <4 x i32> } %out0, <4 x i32> %carry, 1
  ret { <4 x i32>, <4 x i32> } %out1
}

define internal <4 x i64> @vu_mul_correct4(<4 x i32> %ma, <4 x i32> %mb, <4 x i64> %full, <4 x i1> %needs) noinline {
entry:
  %code_shift0 = shl <4 x i32> %mb, <i32 1, i32 1, i32 1, i32 1>
  %code0 = and <4 x i32> %code_shift0, <i32 7, i32 7, i32 7, i32 7>
  %booth0 = call { <4 x i32>, <4 x i32> } @vu_mul_booth_row4(<4 x i32> %ma, <4 x i32> %code0, i32 0)
  %row0 = extractvalue { <4 x i32>, <4 x i32> } %booth0, 0
  %code_shift1 = lshr <4 x i32> %mb, <i32 1, i32 1, i32 1, i32 1>
  %code1 = and <4 x i32> %code_shift1, <i32 7, i32 7, i32 7, i32 7>
  %booth1 = call { <4 x i32>, <4 x i32> } @vu_mul_booth_row4(<4 x i32> %ma, <4 x i32> %code1, i32 2)
  %row1 = extractvalue { <4 x i32>, <4 x i32> } %booth1, 0
  %code_shift2 = lshr <4 x i32> %mb, <i32 3, i32 3, i32 3, i32 3>
  %code2 = and <4 x i32> %code_shift2, <i32 7, i32 7, i32 7, i32 7>
  %booth2 = call { <4 x i32>, <4 x i32> } @vu_mul_booth_row4(<4 x i32> %ma, <4 x i32> %code2, i32 4)
  %row2 = extractvalue { <4 x i32>, <4 x i32> } %booth2, 0
  %code_shift3 = lshr <4 x i32> %mb, <i32 5, i32 5, i32 5, i32 5>
  %code3 = and <4 x i32> %code_shift3, <i32 7, i32 7, i32 7, i32 7>
  %booth3 = call { <4 x i32>, <4 x i32> } @vu_mul_booth_row4(<4 x i32> %ma, <4 x i32> %code3, i32 6)
  %row3 = extractvalue { <4 x i32>, <4 x i32> } %booth3, 0
  %code_shift4 = lshr <4 x i32> %mb, <i32 7, i32 7, i32 7, i32 7>
  %code4 = and <4 x i32> %code_shift4, <i32 7, i32 7, i32 7, i32 7>
  %booth4 = call { <4 x i32>, <4 x i32> } @vu_mul_booth_row4(<4 x i32> %ma, <4 x i32> %code4, i32 8)
  %row4 = extractvalue { <4 x i32>, <4 x i32> } %booth4, 0
  %code_shift5 = lshr <4 x i32> %mb, <i32 9, i32 9, i32 9, i32 9>
  %code5 = and <4 x i32> %code_shift5, <i32 7, i32 7, i32 7, i32 7>
  %booth5 = call { <4 x i32>, <4 x i32> } @vu_mul_booth_row4(<4 x i32> %ma, <4 x i32> %code5, i32 10)
  %row5 = extractvalue { <4 x i32>, <4 x i32> } %booth5, 0
  %correction5 = extractvalue { <4 x i32>, <4 x i32> } %booth5, 1
  %code_shift6 = lshr <4 x i32> %mb, <i32 11, i32 11, i32 11, i32 11>
  %code6 = and <4 x i32> %code_shift6, <i32 7, i32 7, i32 7, i32 7>
  %booth6 = call { <4 x i32>, <4 x i32> } @vu_mul_booth_row4(<4 x i32> %ma, <4 x i32> %code6, i32 12)
  %row6 = extractvalue { <4 x i32>, <4 x i32> } %booth6, 0
  %correction6 = extractvalue { <4 x i32>, <4 x i32> } %booth6, 1
  %code_shift7 = lshr <4 x i32> %mb, <i32 13, i32 13, i32 13, i32 13>
  %code7 = and <4 x i32> %code_shift7, <i32 7, i32 7, i32 7, i32 7>
  %booth7 = call { <4 x i32>, <4 x i32> } @vu_mul_booth_row4(<4 x i32> %ma, <4 x i32> %code7, i32 14)
  %row7 = extractvalue { <4 x i32>, <4 x i32> } %booth7, 0
  %correction7 = extractvalue { <4 x i32>, <4 x i32> } %booth7, 1
  %compress0 = call { <4 x i32>, <4 x i32> } @vu_mul_csa4(<4 x i32> %row1, <4 x i32> %row2, <4 x i32> %row3)
  %sum0 = extractvalue { <4 x i32>, <4 x i32> } %compress0, 0
  %carry0 = extractvalue { <4 x i32>, <4 x i32> } %compress0, 1
  %masked4 = and <4 x i32> %row4, <i32 -2048, i32 -2048, i32 -2048, i32 -2048>
  %masked5 = and <4 x i32> %row5, <i32 -4096, i32 -4096, i32 -4096, i32 -4096>
  %compress1 = call { <4 x i32>, <4 x i32> } @vu_mul_csa4(<4 x i32> %masked4, <4 x i32> %masked5, <4 x i32> %row6)
  %sum1 = extractvalue { <4 x i32>, <4 x i32> } %compress1, 0
  %carry1 = extractvalue { <4 x i32>, <4 x i32> } %compress1, 1
  %bit11 = and <4 x i32> %row5, <i32 2048, i32 2048, i32 2048, i32 2048>
  %carry_fix = or <4 x i32> %correction6, %bit11
  %corrected_carry1 = or <4 x i32> %carry1, %carry_fix
  %bit10 = and <4 x i32> %row5, <i32 1024, i32 1024, i32 1024, i32 1024>
  %row_fix = add <4 x i32> %bit10, %correction5
  %corrected_row7 = or <4 x i32> %row7, %row_fix
  %compress2 = call { <4 x i32>, <4 x i32> } @vu_mul_csa4(<4 x i32> %row0, <4 x i32> %sum0, <4 x i32> %carry0)
  %sum2 = extractvalue { <4 x i32>, <4 x i32> } %compress2, 0
  %carry2 = extractvalue { <4 x i32>, <4 x i32> } %compress2, 1
  %compress3 = call { <4 x i32>, <4 x i32> } @vu_mul_csa4(<4 x i32> %corrected_row7, <4 x i32> %sum1, <4 x i32> %corrected_carry1)
  %sum3 = extractvalue { <4 x i32>, <4 x i32> } %compress3, 0
  %carry3 = extractvalue { <4 x i32>, <4 x i32> } %compress3, 1
  %compress4 = call { <4 x i32>, <4 x i32> } @vu_mul_csa4(<4 x i32> %carry2, <4 x i32> %sum3, <4 x i32> %carry3)
  %sum4 = extractvalue { <4 x i32>, <4 x i32> } %compress4, 0
  %carry4 = extractvalue { <4 x i32>, <4 x i32> } %compress4, 1
  %compress5 = call { <4 x i32>, <4 x i32> } @vu_mul_csa4(<4 x i32> %sum2, <4 x i32> %sum4, <4 x i32> %carry4)
  %sum5 = extractvalue { <4 x i32>, <4 x i32> } %compress5, 0
  %carry5 = extractvalue { <4 x i32>, <4 x i32> } %compress5, 1
  %corrected_carry5 = add <4 x i32> %carry5, %correction7
  %truncated_sum = and <4 x i32> %sum5, <i32 -32768, i32 -32768, i32 -32768, i32 -32768>
  %truncated_carry = and <4 x i32> %corrected_carry5, <i32 -32768, i32 -32768, i32 -32768, i32 -32768>
  %low = add <4 x i32> %truncated_sum, %truncated_carry
  %full_low = trunc <4 x i64> %full to <4 x i32>
  %different = xor <4 x i32> %low, %full_low
  %lost = and <4 x i32> %different, <i32 32768, i32 32768, i32 32768, i32 32768>
  %lost64 = zext <4 x i32> %lost to <4 x i64>
  %product = sub <4 x i64> %full, %lost64
  %result = select <4 x i1> %needs, <4 x i64> %product, <4 x i64> %full
  ret <4 x i64> %result
}

define internal { <4 x i32>, <4 x i1>, <4 x i1> } @vu_mul4(<4 x i32> %lhs, <4 x i32> %rhs) alwaysinline {
entry:
  %ea_shift = lshr <4 x i32> %lhs, <i32 23, i32 23, i32 23, i32 23>
  %eb_shift = lshr <4 x i32> %rhs, <i32 23, i32 23, i32 23, i32 23>
  %ea = and <4 x i32> %ea_shift, <i32 255, i32 255, i32 255, i32 255>
  %eb = and <4 x i32> %eb_shift, <i32 255, i32 255, i32 255, i32 255>
  %a_zero = icmp eq <4 x i32> %ea, <i32 0, i32 0, i32 0, i32 0>
  %b_zero = icmp eq <4 x i32> %eb, <i32 0, i32 0, i32 0, i32 0>
  %zero = or <4 x i1> %a_zero, %b_zero
  %sign_xor = xor <4 x i32> %lhs, %rhs
  %sign = and <4 x i32> %sign_xor, <i32 -2147483648, i32 -2147483648, i32 -2147483648, i32 -2147483648>
  %fa = and <4 x i32> %lhs, <i32 8388607, i32 8388607, i32 8388607, i32 8388607>
  %fb = and <4 x i32> %rhs, <i32 8388607, i32 8388607, i32 8388607, i32 8388607>
  %ma = or <4 x i32> %fa, <i32 8388608, i32 8388608, i32 8388608, i32 8388608>
  %mb = or <4 x i32> %fb, <i32 8388608, i32 8388608, i32 8388608, i32 8388608>
  %ma64 = zext <4 x i32> %ma to <4 x i64>
  %mb64 = zext <4 x i32> %mb to <4 x i64>
  %full = mul <4 x i64> %ma64, %mb64
  ; Only product bits 15..22 determine whether subtracting 2^15
  ; could cross the truncation boundary. A packed 32-bit zero test
  ; avoids wide compares and discards no observable product bits.
  %full_low = trunc <4 x i64> %full to <4 x i32>
  %boundary_bits = and <4 x i32> %full_low, <i32 8355840, i32 8355840, i32 8355840, i32 8355840>
  %boundary = icmp eq <4 x i32> %boundary_bits, zeroinitializer
  %nonzero = xor <4 x i1> %zero, <i1 true, i1 true, i1 true, i1 true>
  %needs = and <4 x i1> %boundary, %nonzero
  %mask = bitcast <4 x i1> %needs to i4
  %any = icmp ne i4 %mask, 0
  br i1 %any, label %correct, label %normalize
correct:
  %corrected = call <4 x i64> @vu_mul_correct4(<4 x i32> %ma, <4 x i32> %mb, <4 x i64> %full, <4 x i1> %needs)
  br label %normalize
normalize:
  %product = phi <4 x i64> [ %full, %entry ], [ %corrected, %correct ]
  %shifted = lshr <4 x i64> %product, <i64 23, i64 23, i64 23, i64 23>
  %sig = trunc <4 x i64> %shifted to <4 x i32>
  %wide = icmp ugt <4 x i32> %sig, <i32 16777215, i32 16777215, i32 16777215, i32 16777215>
  %shift = zext <4 x i1> %wide to <4 x i32>
  %half_sig = lshr <4 x i32> %sig, <i32 1, i32 1, i32 1, i32 1>
  %norm = select <4 x i1> %wide, <4 x i32> %half_sig, <4 x i32> %sig
  %sum_exp = add <4 x i32> %ea, %eb
  %biased = sub <4 x i32> %sum_exp, <i32 127, i32 127, i32 127, i32 127>
  %exponent = add <4 x i32> %biased, %shift
  %overflow = icmp sgt <4 x i32> %exponent, <i32 255, i32 255, i32 255, i32 255>
  %underflow = icmp slt <4 x i32> %exponent, <i32 1, i32 1, i32 1, i32 1>
  %fraction = and <4 x i32> %norm, <i32 8388607, i32 8388607, i32 8388607, i32 8388607>
  %exp_bits = shl <4 x i32> %exponent, <i32 23, i32 23, i32 23, i32 23>
  %normal = or <4 x i32> %exp_bits, %fraction
  %bounded = select <4 x i1> %overflow, <4 x i32> <i32 2147483647, i32 2147483647, i32 2147483647, i32 2147483647>, <4 x i32> %normal
  %absolute = select <4 x i1> %underflow, <4 x i32> <i32 0, i32 0, i32 0, i32 0>, <4 x i32> %bounded
  %bits = or <4 x i32> %absolute, %sign
  %result_bits = select <4 x i1> %zero, <4 x i32> %sign, <4 x i32> %bits
  %result_o = and <4 x i1> %overflow, %nonzero
  %result_u = and <4 x i1> %underflow, %nonzero
  %result0 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1> } poison, <4 x i32> %result_bits, 0
  %result1 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1> } %result0, <4 x i1> %result_o, 1
  %result2 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1> } %result1, <4 x i1> %result_u, 2
  ret { <4 x i32>, <4 x i1>, <4 x i1> } %result2
}

define internal { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } @vu_fmac4(i32 %op, <4 x i32> %acc, <4 x i32> %a, <4 x i32> %b, <4 x i1> %latched) alwaysinline {
entry:
  %is_add = icmp ult i32 %op, 2
  br i1 %is_add, label %add, label %product
add:
  %subtract = icmp eq i32 %op, 1
  %sum = call { <4 x i32>, <4 x i1>, <4 x i1> } @vu_add4(<4 x i32> %a, <4 x i32> %b, i1 %subtract)
  br label %plain
product:
  %p = call { <4 x i32>, <4 x i1>, <4 x i1> } @vu_mul4(<4 x i32> %a, <4 x i32> %b)
  %is_mul = icmp eq i32 %op, 2
  br i1 %is_mul, label %plain, label %mac
plain:
  %plain_result = phi { <4 x i32>, <4 x i1>, <4 x i1> } [ %sum, %add ], [ %p, %product ]
  %plain_bits = extractvalue { <4 x i32>, <4 x i1>, <4 x i1> } %plain_result, 0
  %plain_o = extractvalue { <4 x i32>, <4 x i1>, <4 x i1> } %plain_result, 1
  %plain_u = extractvalue { <4 x i32>, <4 x i1>, <4 x i1> } %plain_result, 2
  %r0 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } zeroinitializer, <4 x i32> %plain_bits, 0
  %r1 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } %r0, <4 x i1> %plain_o, 1
  %r2 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } %r1, <4 x i1> %plain_u, 2
  ret { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } %r2
mac:
  %pb = extractvalue { <4 x i32>, <4 x i1>, <4 x i1> } %p, 0
  %po = extractvalue { <4 x i32>, <4 x i1>, <4 x i1> } %p, 1
  %pu = extractvalue { <4 x i32>, <4 x i1>, <4 x i1> } %p, 2
  %sub = icmp eq i32 %op, 4
  %acc_sum = call { <4 x i32>, <4 x i1>, <4 x i1> } @vu_add4(<4 x i32> %acc, <4 x i32> %pb, i1 %sub)
  %sb = extractvalue { <4 x i32>, <4 x i1>, <4 x i1> } %acc_sum, 0
  %so = extractvalue { <4 x i32>, <4 x i1>, <4 x i1> } %acc_sum, 1
  %su = extractvalue { <4 x i32>, <4 x i1>, <4 x i1> } %acc_sum, 2
  %sub_sign = select i1 %sub, <4 x i32> <i32 -2147483648, i32 -2147483648, i32 -2147483648, i32 -2147483648>, <4 x i32> <i32 0, i32 0, i32 0, i32 0>
  %signed_product = xor <4 x i32> %pb, %sub_sign
  %acc_sign = and <4 x i32> %acc, <i32 -2147483648, i32 -2147483648, i32 -2147483648, i32 -2147483648>
  %acc_sat = or <4 x i32> %acc_sign, <i32 2147483647, i32 2147483647, i32 2147483647, i32 2147483647>
  %acc_or_sum = select <4 x i1> %latched, <4 x i32> %acc_sat, <4 x i32> %sb
  %bits = select <4 x i1> %po, <4 x i32> %signed_product, <4 x i32> %acc_or_sum
  %prior = or <4 x i1> %po, %latched
  %overflow = or <4 x i1> %prior, %so
  %no_prior = xor <4 x i1> %prior, <i1 true, i1 true, i1 true, i1 true>
  %underflow = and <4 x i1> %no_prior, %su
  %exception = or <4 x i1> %overflow, %underflow
  %no_po = xor <4 x i1> %po, <i1 true, i1 true, i1 true, i1 true>
  %product_events = and <4 x i1> %exception, %no_po
  %pexp = and <4 x i32> %pb, <i32 2139095040, i32 2139095040, i32 2139095040, i32 2139095040>
  %pz = icmp eq <4 x i32> %pexp, <i32 0, i32 0, i32 0, i32 0>
  %ps = icmp slt <4 x i32> %pb, <i32 0, i32 0, i32 0, i32 0>
  %z_event = and <4 x i1> %pz, %product_events
  %s_event = and <4 x i1> %ps, %product_events
  %z_sticky = select <4 x i1> %z_event, <4 x i32> <i32 64, i32 64, i32 64, i32 64>, <4 x i32> <i32 0, i32 0, i32 0, i32 0>
  %s_sticky = select <4 x i1> %s_event, <4 x i32> <i32 128, i32 128, i32 128, i32 128>, <4 x i32> <i32 0, i32 0, i32 0, i32 0>
  %u_sticky = select <4 x i1> %pu, <4 x i32> <i32 256, i32 256, i32 256, i32 256>, <4 x i32> <i32 0, i32 0, i32 0, i32 0>
  %zs = or <4 x i32> %z_sticky, %s_sticky
  %sticky = or <4 x i32> %zs, %u_sticky
  %m0 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } poison, <4 x i32> %bits, 0
  %m1 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } %m0, <4 x i1> %overflow, 1
  %m2 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } %m1, <4 x i1> %underflow, 2
  %m3 = insertvalue { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } %m2, <4 x i32> %sticky, 3
  ret { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } %m3
}
)IR";
}
