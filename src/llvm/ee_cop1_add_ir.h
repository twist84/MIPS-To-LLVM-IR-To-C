#pragma once

// R5900 addition/subtraction: align with one retained guard bit, then
// truncate the normalized magnitude. Exponent 255 is finite, not IEEE
// NaN/Inf. Integer arithmetic is independent of host rounding and FTZ.
// Reference: ps2autotests ee_fpu/arithmetic.expected, PCSX2 PR #12001.
namespace llvm
{
inline constexpr char k_ee_cop1_add_ir[] = R"IR(
define internal { i32, i1, i1 } @ee_cop1_add_bits(i32 %lhs, i32 %rhs, i1 %subtract) {
entry:
  %subtract_sign = select i1 %subtract, i32 -2147483648, i32 0
  %b = xor i32 %rhs, %subtract_sign
  %ea_shift = lshr i32 %lhs, 23
  %eb_shift = lshr i32 %b, 23
  %ea = and i32 %ea_shift, 255
  %eb = and i32 %eb_shift, 255
  %a_zero = icmp eq i32 %ea, 0
  %b_zero = icmp eq i32 %eb, 0
  %a_fraction = and i32 %lhs, 8388607
  %b_fraction = and i32 %b, 8388607
  %a_hidden = or i32 %a_fraction, 8388608
  %b_hidden = or i32 %b_fraction, 8388608
  %a_guard = shl i32 %a_hidden, 1
  %b_guard = shl i32 %b_hidden, 1
  %ma = select i1 %a_zero, i32 0, i32 %a_guard
  %mb = select i1 %b_zero, i32 0, i32 %b_guard
  %a_larger = icmp uge i32 %ea, %eb
  %e = select i1 %a_larger, i32 %ea, i32 %eb
  %da = sub i32 %e, %ea
  %db = sub i32 %e, %eb
  %a_far = icmp uge i32 %da, 25
  %b_far = icmp uge i32 %db, 25
  %sa = select i1 %a_far, i32 25, i32 %da
  %sb = select i1 %b_far, i32 25, i32 %db
  %aligned_a = lshr i32 %ma, %sa
  %aligned_b = lshr i32 %mb, %sb
  %a_negative = icmp slt i32 %lhs, 0
  %b_negative = icmp slt i32 %b, 0
  %neg_a = sub i32 0, %aligned_a
  %neg_b = sub i32 0, %aligned_b
  %signed_a = select i1 %a_negative, i32 %neg_a, i32 %aligned_a
  %signed_b = select i1 %b_negative, i32 %neg_b, i32 %aligned_b
  %sum = add i32 %signed_a, %signed_b
  %zero = icmp eq i32 %sum, 0
  %negative = icmp slt i32 %sum, 0
  %neg_sum = sub i32 0, %sum
  %magnitude = select i1 %negative, i32 %neg_sum, i32 %sum
  %leading = call i32 @llvm.ctlz.i32(i32 %magnitude, i1 false)
  %safe_shift = and i32 %leading, 31
  %normalized = shl i32 %magnitude, %safe_shift
  %significand = lshr i32 %normalized, 8
  %fraction = and i32 %significand, 8388607
  %offset = sub i32 7, %leading
  %exponent = add i32 %e, %offset
  %above = icmp sgt i32 %exponent, 255
  %below = icmp slt i32 %exponent, 1
  %nonzero = xor i1 %zero, true
  %overflow = and i1 %above, %nonzero
  %underflow = and i1 %below, %nonzero
  %both_negative = and i1 %a_negative, %b_negative
  %result_negative = select i1 %zero, i1 %both_negative, i1 %negative
  %sign = select i1 %result_negative, i32 -2147483648, i32 0
  %exp_bits = shl i32 %exponent, 23
  %normal = or i32 %exp_bits, %fraction
  %bounded = select i1 %overflow, i32 2147483647, i32 %normal
  ; A cancellation underflow retains the normalized fraction with an
  ; exponent of zero. Only an exactly zero sum loses the fraction.
  %range_result = select i1 %underflow, i32 %fraction, i32 %bounded
  %absolute = select i1 %zero, i32 0, i32 %range_result
  %bits = or i32 %absolute, %sign
  %out0 = insertvalue { i32, i1, i1 } poison, i32 %bits, 0
  %out1 = insertvalue { i32, i1, i1 } %out0, i1 %overflow, 1
  %out2 = insertvalue { i32, i1, i1 } %out1, i1 %underflow, 2
  ret { i32, i1, i1 } %out2
}
)IR";
}
