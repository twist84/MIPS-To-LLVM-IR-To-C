#pragma once

// Independent LLVM IR implementation of the R5900 signed-digit recurrence.
// Algorithm references: DOI 10.1109/ARITH.1995.465363 and the hardware-derived
// model in PCSX2 PR #12001. Result words checked against ps2autotests
// tests/cpu/ee_fpu/sqrt.expected. All arithmetic wraps at 32 bits.
// Keep these helpers private and emit them only in modules using DIV.S/RSQRT.S.
namespace llvm
{
inline constexpr char k_ee_cop1_rsqrt_ir[] = R"IR(
define internal { i32, i32, i32 } @ee_cop1_radix_step(i32 %sum, i32 %carry, i32 %magnitude, i32 %digit) {
entry:
  %positive = icmp sgt i32 %digit, 0
  %negative = icmp slt i32 %digit, 0
  %idle = icmp eq i32 %digit, 0
  %complement = xor i32 %magnitude, -1
  %negative_term = select i1 %negative, i32 %magnitude, i32 0
  %term = select i1 %positive, i32 %complement, i32 %negative_term
  %borrow = zext i1 %positive to i32
  %adjusted_carry = add i32 %carry, %borrow
  %parity = xor i32 %sum, %adjusted_carry
  %new_sum = xor i32 %parity, %term
  %both = and i32 %sum, %adjusted_carry
  %third = and i32 %parity, %term
  %majority = or i32 %both, %third
  %new_carry = shl i32 %majority, 1
  ; The selector observes the upper remainder slice before the shift.
  %test_sum = select i1 %idle, i32 %sum, i32 %new_sum
  %test_carry = select i1 %idle, i32 %carry, i32 %new_carry
  %upper = and i32 %test_sum, -16777216
  %lower = and i32 %test_sum, 16777215
  %upper_total = add i32 %upper, %test_carry
  %estimate = or i32 %upper_total, %lower
  %choose_pos = icmp sge i32 %estimate, 8388608
  %choose_neg = icmp slt i32 %estimate, -16777216
  %zero_or_neg = select i1 %choose_neg, i32 -1, i32 0
  %next_digit = select i1 %choose_pos, i32 1, i32 %zero_or_neg
  %shifted_sum = shl i32 %new_sum, 1
  %shifted_carry = shl i32 %new_carry, 1
  %result0 = insertvalue { i32, i32, i32 } poison, i32 %shifted_sum, 0
  %result1 = insertvalue { i32, i32, i32 } %result0, i32 %shifted_carry, 1
  %result2 = insertvalue { i32, i32, i32 } %result1, i32 %next_digit, 2
  ret { i32, i32, i32 } %result2
}

define internal i32 @ee_cop1_radix_root(i32 %raw) {
entry:
  %shifted = lshr i32 %raw, 23
  %exponent = and i32 %shifted, 255
  %zero = icmp eq i32 %exponent, 0
  br i1 %zero, label %flush, label %prepare
flush:
  ret i32 0
prepare:
  %fraction = and i32 %raw, 8388607
  %significand = or i32 %fraction, 8388608
  %parity = and i32 %exponent, 1
  %initial_shift = sub i32 2, %parity
  %initial_sum = shl i32 %significand, %initial_shift
  br label %digits
digits:
  %remaining = phi i32 [ 24, %prepare ], [ %next_remaining, %digits ]
  %sum = phi i32 [ %initial_sum, %prepare ], [ %next_sum, %digits ]
  %carry = phi i32 [ 0, %prepare ], [ %next_carry, %digits ]
  %digit = phi i32 [ 1, %prepare ], [ %next_digit, %digits ]
  %root = phi i32 [ 0, %prepare ], [ %next_root, %digits ]
  %increment = shl i32 %digit, %remaining
  %trial = add i32 %root, %increment
  %twice_increment = shl i32 %increment, 1
  %next_root = add i32 %root, %twice_increment
  %step = call { i32, i32, i32 } @ee_cop1_radix_step(i32 %sum, i32 %carry, i32 %trial, i32 %digit)
  %next_sum = extractvalue { i32, i32, i32 } %step, 0
  %next_carry = extractvalue { i32, i32, i32 } %step, 1
  %next_digit = extractvalue { i32, i32, i32 } %step, 2
  %next_remaining = sub i32 %remaining, 1
  %last = icmp eq i32 %remaining, 0
  br i1 %last, label %pack, label %digits
pack:
  %biased_twice = add i32 %exponent, 127
  %biased = lshr i32 %biased_twice, 1
  %exponent_bits = shl i32 %biased, 23
  %root_significand = lshr i32 %next_root, 2
  %root_fraction = and i32 %root_significand, 8388607
  %bits = or i32 %exponent_bits, %root_fraction
  ret i32 %bits
}

define internal i32 @ee_cop1_radix_quotient(i32 %numerator, i32 %denominator) {
entry:
  %num_shifted = lshr i32 %numerator, 23
  %den_shifted = lshr i32 %denominator, 23
  %num_exp = and i32 %num_shifted, 255
  %den_exp = and i32 %den_shifted, 255
  %sign_xor = xor i32 %numerator, %denominator
  %sign = and i32 %sign_xor, -2147483648
  %den_zero = icmp eq i32 %den_exp, 0
  br i1 %den_zero, label %saturate, label %check_numerator
check_numerator:
  %num_zero = icmp eq i32 %num_exp, 0
  br i1 %num_zero, label %flush, label %prepare
prepare:
  %num_fraction = and i32 %numerator, 8388607
  %den_fraction = and i32 %denominator, 8388607
  %num_significand = or i32 %num_fraction, 8388608
  %den_significand = or i32 %den_fraction, 8388608
  %initial_sum = shl i32 %num_significand, 2
  %divisor = shl i32 %den_significand, 2
  br label %digits
digits:
  %remaining = phi i32 [ 25, %prepare ], [ %next_remaining, %digits ]
  %sum = phi i32 [ %initial_sum, %prepare ], [ %next_sum, %digits ]
  %carry = phi i32 [ 0, %prepare ], [ %next_carry, %digits ]
  %digit = phi i32 [ 1, %prepare ], [ %next_digit, %digits ]
  %quotient = phi i32 [ 0, %prepare ], [ %next_quotient, %digits ]
  %twice = shl i32 %quotient, 1
  %next_quotient = add i32 %twice, %digit
  %step = call { i32, i32, i32 } @ee_cop1_radix_step(i32 %sum, i32 %carry, i32 %divisor, i32 %digit)
  %next_sum = extractvalue { i32, i32, i32 } %step, 0
  %next_carry = extractvalue { i32, i32, i32 } %step, 1
  %next_digit = extractvalue { i32, i32, i32 } %step, 2
  %next_remaining = sub i32 %remaining, 1
  %last = icmp eq i32 %next_remaining, 0
  br i1 %last, label %normalize, label %digits
normalize:
  %wide = icmp uge i32 %next_quotient, 16777216
  %shift = zext i1 %wide to i32
  %significand = lshr i32 %next_quotient, %shift
  %difference = sub i32 %num_exp, %den_exp
  %base = add i32 %difference, 126
  %exponent = add i32 %base, %shift
  %too_large = icmp sgt i32 %exponent, 255
  br i1 %too_large, label %saturate, label %check_underflow
check_underflow:
  %too_small = icmp slt i32 %exponent, 1
  br i1 %too_small, label %flush, label %pack
pack:
  %exponent_bits = shl i32 %exponent, 23
  %fraction = and i32 %significand, 8388607
  %absolute = or i32 %exponent_bits, %fraction
  %bits = or i32 %sign, %absolute
  ret i32 %bits
saturate:
  %maximum = or i32 %sign, 2147483647
  ret i32 %maximum
flush:
  ret i32 %sign
}

define internal i32 @ee_cop1_rsqrt_bits(i32 %numerator, i32 %denominator) {
entry:
  %root = call i32 @ee_cop1_radix_root(i32 %denominator)
  %result = call i32 @ee_cop1_radix_quotient(i32 %numerator, i32 %root)
  ret i32 %result
}
)IR";
}
