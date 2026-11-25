#pragma once

// R5900 asymmetric significand multiplier. Reconstruct the low partial
// products using radix-4 Booth recoding and the truncated carry-save tree.
// The resulting bit-15 correction precedes normalization and truncation.
// Hardware-derived model: PCSX2 PR #12001; console fixture: ps2autotests
// tests/cpu/ee_fpu/muldiv.expected. Operand order must not be exchanged.
namespace llvm
{
inline constexpr char k_ee_cop1_mul_ir[] = R"IR(
define internal { i32, i32 } @ee_cop1_booth_row(i32 %a, i32 %code, i32 %shift) {
entry:
  %base = shl i32 %a, %shift
  %twice = shl i32 %base, 1
  %double_pos = icmp eq i32 %code, 3
  %double_neg = icmp eq i32 %code, 4
  %double = or i1 %double_pos, %double_neg
  %magnitude = select i1 %double, i32 %twice, i32 %base
  %at_least_four = icmp uge i32 %code, 4
  %at_most_six = icmp ule i32 %code, 6
  %negative = and i1 %at_least_four, %at_most_six
  %position = shl i32 1, %shift
  %mask = sub i32 0, %position
  %inverted = xor i32 %magnitude, %mask
  %signed = select i1 %negative, i32 %inverted, i32 %magnitude
  %nonzero = icmp ne i32 %code, 0
  %active = and i1 %nonzero, %at_most_six
  %row = select i1 %active, i32 %signed, i32 0
  %carry = select i1 %negative, i32 %position, i32 0
  %pair0 = insertvalue { i32, i32 } poison, i32 %row, 0
  %pair1 = insertvalue { i32, i32 } %pair0, i32 %carry, 1
  ret { i32, i32 } %pair1
}
define internal { i32, i32 } @ee_cop1_mul_csa(i32 %a, i32 %b, i32 %c) {
entry:
  %parity = xor i32 %a, %b
  %sum = xor i32 %parity, %c
  %ab = and i32 %a, %b
  %pc = and i32 %parity, %c
  %majority = or i32 %ab, %pc
  %carry = shl i32 %majority, 1
  %pair0 = insertvalue { i32, i32 } poison, i32 %sum, 0
  %pair1 = insertvalue { i32, i32 } %pair0, i32 %carry, 1
  ret { i32, i32 } %pair1
}
define internal i64 @ee_cop1_mul_correct(i32 %ma, i32 %mb, i64 %full) noinline {
entry:
  %code_shift0 = shl i32 %mb, 1
  %code0 = and i32 %code_shift0, 7
  %booth0 = call { i32, i32 } @ee_cop1_booth_row(i32 %ma, i32 %code0, i32 0)
  %row0 = extractvalue { i32, i32 } %booth0, 0
  %code_shift1 = lshr i32 %mb, 1
  %code1 = and i32 %code_shift1, 7
  %booth1 = call { i32, i32 } @ee_cop1_booth_row(i32 %ma, i32 %code1, i32 2)
  %row1 = extractvalue { i32, i32 } %booth1, 0
  %code_shift2 = lshr i32 %mb, 3
  %code2 = and i32 %code_shift2, 7
  %booth2 = call { i32, i32 } @ee_cop1_booth_row(i32 %ma, i32 %code2, i32 4)
  %row2 = extractvalue { i32, i32 } %booth2, 0
  %code_shift3 = lshr i32 %mb, 5
  %code3 = and i32 %code_shift3, 7
  %booth3 = call { i32, i32 } @ee_cop1_booth_row(i32 %ma, i32 %code3, i32 6)
  %row3 = extractvalue { i32, i32 } %booth3, 0
  %code_shift4 = lshr i32 %mb, 7
  %code4 = and i32 %code_shift4, 7
  %booth4 = call { i32, i32 } @ee_cop1_booth_row(i32 %ma, i32 %code4, i32 8)
  %row4 = extractvalue { i32, i32 } %booth4, 0
  %code_shift5 = lshr i32 %mb, 9
  %code5 = and i32 %code_shift5, 7
  %booth5 = call { i32, i32 } @ee_cop1_booth_row(i32 %ma, i32 %code5, i32 10)
  %row5 = extractvalue { i32, i32 } %booth5, 0
  %correction5 = extractvalue { i32, i32 } %booth5, 1
  %code_shift6 = lshr i32 %mb, 11
  %code6 = and i32 %code_shift6, 7
  %booth6 = call { i32, i32 } @ee_cop1_booth_row(i32 %ma, i32 %code6, i32 12)
  %row6 = extractvalue { i32, i32 } %booth6, 0
  %correction6 = extractvalue { i32, i32 } %booth6, 1
  %code_shift7 = lshr i32 %mb, 13
  %code7 = and i32 %code_shift7, 7
  %booth7 = call { i32, i32 } @ee_cop1_booth_row(i32 %ma, i32 %code7, i32 14)
  %row7 = extractvalue { i32, i32 } %booth7, 0
  %correction7 = extractvalue { i32, i32 } %booth7, 1
  %compress0 = call { i32, i32 } @ee_cop1_mul_csa(i32 %row1, i32 %row2, i32 %row3)
  %sum0 = extractvalue { i32, i32 } %compress0, 0
  %carry0 = extractvalue { i32, i32 } %compress0, 1
  %masked4 = and i32 %row4, -2048
  %masked5 = and i32 %row5, -4096
  %compress1 = call { i32, i32 } @ee_cop1_mul_csa(i32 %masked4, i32 %masked5, i32 %row6)
  %sum1 = extractvalue { i32, i32 } %compress1, 0
  %carry1 = extractvalue { i32, i32 } %compress1, 1
  %bit11 = and i32 %row5, 2048
  %carry_fix = or i32 %correction6, %bit11
  %corrected_carry1 = or i32 %carry1, %carry_fix
  %bit10 = and i32 %row5, 1024
  %row_fix = add i32 %bit10, %correction5
  %corrected_row7 = or i32 %row7, %row_fix
  %compress2 = call { i32, i32 } @ee_cop1_mul_csa(i32 %row0, i32 %sum0, i32 %carry0)
  %sum2 = extractvalue { i32, i32 } %compress2, 0
  %carry2 = extractvalue { i32, i32 } %compress2, 1
  %compress3 = call { i32, i32 } @ee_cop1_mul_csa(i32 %corrected_row7, i32 %sum1, i32 %corrected_carry1)
  %sum3 = extractvalue { i32, i32 } %compress3, 0
  %carry3 = extractvalue { i32, i32 } %compress3, 1
  %compress4 = call { i32, i32 } @ee_cop1_mul_csa(i32 %carry2, i32 %sum3, i32 %carry3)
  %sum4 = extractvalue { i32, i32 } %compress4, 0
  %carry4 = extractvalue { i32, i32 } %compress4, 1
  %compress5 = call { i32, i32 } @ee_cop1_mul_csa(i32 %sum2, i32 %sum4, i32 %carry4)
  %sum5 = extractvalue { i32, i32 } %compress5, 0
  %carry5 = extractvalue { i32, i32 } %compress5, 1
  %corrected_carry5 = add i32 %carry5, %correction7
  %truncated_sum = and i32 %sum5, -32768
  %truncated_carry = and i32 %corrected_carry5, -32768
  %low = add i32 %truncated_sum, %truncated_carry
  %full_low = trunc i64 %full to i32
  %different = xor i32 %low, %full_low
  %lost = and i32 %different, 32768
  %lost64 = zext i32 %lost to i64
  %product = sub i64 %full, %lost64
  ret i64 %product
}
define internal { i32, i1, i1 } @ee_cop1_mul_bits(i32 %lhs, i32 %rhs) {
entry:
  %ea_shift = lshr i32 %lhs, 23
  %eb_shift = lshr i32 %rhs, 23
  %ea = and i32 %ea_shift, 255
  %eb = and i32 %eb_shift, 255
  %a_zero = icmp eq i32 %ea, 0
  %b_zero = icmp eq i32 %eb, 0
  %zero = or i1 %a_zero, %b_zero
  %sign_xor = xor i32 %lhs, %rhs
  %sign = and i32 %sign_xor, -2147483648
  br i1 %zero, label %flush, label %multiply
flush:
  %zero_pair = insertvalue { i32, i1, i1 } zeroinitializer, i32 %sign, 0
  ret { i32, i1, i1 } %zero_pair
multiply:
  %fa = and i32 %lhs, 8388607
  %fb = and i32 %rhs, 8388607
  %ma = or i32 %fa, 8388608
  %mb = or i32 %fb, 8388608
  %ma64 = zext i32 %ma to i64
  %mb64 = zext i32 %mb to i64
  %full = mul i64 %ma64, %mb64
  ; Subtracting at most 2^15 cannot cross a 2^23 truncation boundary
  ; unless the low 23 bits are strictly below 2^15. Keep the original
  ; asymmetric Booth calculation off the common path.
  %low23 = and i64 %full, 8388607
  %needs_correction = icmp ult i64 %low23, 32768
  br i1 %needs_correction, label %correct, label %normalize
correct:
  %corrected = call i64 @ee_cop1_mul_correct(i32 %ma, i32 %mb, i64 %full)
  br label %normalize
normalize:
  %product = phi i64 [ %full, %multiply ], [ %corrected, %correct ]
  %shifted = lshr i64 %product, 23
  %sig = trunc i64 %shifted to i32
  %wide = icmp ugt i32 %sig, 16777215
  %shift = zext i1 %wide to i32
  %norm = lshr i32 %sig, %shift
  %sum_exp = add i32 %ea, %eb
  %biased = sub i32 %sum_exp, 127
  %exponent = add i32 %biased, %shift
  %overflow = icmp sgt i32 %exponent, 255
  %underflow = icmp slt i32 %exponent, 1
  %fraction = and i32 %norm, 8388607
  %exp_bits = shl i32 %exponent, 23
  %normal = or i32 %exp_bits, %fraction
  %bounded = select i1 %overflow, i32 2147483647, i32 %normal
  %absolute = select i1 %underflow, i32 0, i32 %bounded
  %bits = or i32 %absolute, %sign
  %result0 = insertvalue { i32, i1, i1 } poison, i32 %bits, 0
  %result1 = insertvalue { i32, i1, i1 } %result0, i1 %overflow, 1
  %result2 = insertvalue { i32, i1, i1 } %result1, i1 %underflow, 2
  ret { i32, i1, i1 } %result2
}
)IR";

// Non-fused MAC with product-overflow priority over the independent ACC
// latch. A finite exponent-255 value alone does not imply overflow.
inline constexpr char k_ee_cop1_mac_ir[] = R"IR(
define internal { i32, i1, i1, i1 } @ee_cop1_mac_bits(i32 %acc, i32 %lhs, i32 %rhs, i1 %subtract, i1 %acc_overflow) {
entry:
  %product = call { i32, i1, i1 } @ee_cop1_mul_bits(i32 %lhs, i32 %rhs)
  %product_bits = extractvalue { i32, i1, i1 } %product, 0
  %product_overflow = extractvalue { i32, i1, i1 } %product, 1
  %product_underflow = extractvalue { i32, i1, i1 } %product, 2
  %sum = call { i32, i1, i1 } @ee_cop1_add_bits(i32 %acc, i32 %product_bits, i1 %subtract)
  %sum_bits = extractvalue { i32, i1, i1 } %sum, 0
  %sum_overflow = extractvalue { i32, i1, i1 } %sum, 1
  %sum_underflow = extractvalue { i32, i1, i1 } %sum, 2
  %subtract_sign = select i1 %subtract, i32 -2147483648, i32 0
  %signed_product = xor i32 %product_bits, %subtract_sign
  %acc_sign = and i32 %acc, -2147483648
  %saturated_acc = or i32 %acc_sign, 2147483647
  %acc_or_sum = select i1 %acc_overflow, i32 %saturated_acc, i32 %sum_bits
  %bits = select i1 %product_overflow, i32 %signed_product, i32 %acc_or_sum
  %prior_overflow = or i1 %product_overflow, %acc_overflow
  %overflow = or i1 %prior_overflow, %sum_overflow
  %no_prior_overflow = xor i1 %prior_overflow, true
  %underflow = and i1 %sum_underflow, %no_prior_overflow
  %out0 = insertvalue { i32, i1, i1, i1 } poison, i32 %bits, 0
  %out1 = insertvalue { i32, i1, i1, i1 } %out0, i1 %overflow, 1
  %out2 = insertvalue { i32, i1, i1, i1 } %out1, i1 %underflow, 2
  %out3 = insertvalue { i32, i1, i1, i1 } %out2, i1 %product_underflow, 3
  ret { i32, i1, i1, i1 } %out3
}
)IR";
}
