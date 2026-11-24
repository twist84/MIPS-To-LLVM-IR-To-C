#pragma once

namespace llvm
{
inline constexpr char k_vu_fmac_ir[] = R"IR(
; Reuse the EE integer helpers. The result includes true range errors,
; rather than guessing exceptions from the final saturated word.
define internal { i32, i1, i1, i32 } @vu_fmac(i32 %op, i32 %acc, i32 %a, i32 %b, i1 %latched) alwaysinline {
entry:
  %is_add = icmp ult i32 %op, 2
  br i1 %is_add, label %add, label %product
add:
  %subtract = icmp eq i32 %op, 1
  %sum = call { i32, i1, i1 } @ee_cop1_add_bits(i32 %a, i32 %b, i1 %subtract)
  br label %plain
product:
  %p = call { i32, i1, i1 } @ee_cop1_mul_bits(i32 %a, i32 %b)
  %is_mul = icmp eq i32 %op, 2
  br i1 %is_mul, label %plain, label %mac
plain:
  %plain_result = phi { i32, i1, i1 } [ %sum, %add ], [ %p, %product ]
  %plain_bits = extractvalue { i32, i1, i1 } %plain_result, 0
  %plain_o = extractvalue { i32, i1, i1 } %plain_result, 1
  %plain_u = extractvalue { i32, i1, i1 } %plain_result, 2
  %r0 = insertvalue { i32, i1, i1, i32 } zeroinitializer, i32 %plain_bits, 0
  %r1 = insertvalue { i32, i1, i1, i32 } %r0, i1 %plain_o, 1
  %r2 = insertvalue { i32, i1, i1, i32 } %r1, i1 %plain_u, 2
  ret { i32, i1, i1, i32 } %r2
mac:
  %pb = extractvalue { i32, i1, i1 } %p, 0
  %po = extractvalue { i32, i1, i1 } %p, 1
  %pu = extractvalue { i32, i1, i1 } %p, 2
  %sub = icmp eq i32 %op, 4
  %acc_sum = call { i32, i1, i1 } @ee_cop1_add_bits(i32 %acc, i32 %pb, i1 %sub)
  %sb = extractvalue { i32, i1, i1 } %acc_sum, 0
  %so = extractvalue { i32, i1, i1 } %acc_sum, 1
  %su = extractvalue { i32, i1, i1 } %acc_sum, 2
  %sub_sign = select i1 %sub, i32 -2147483648, i32 0
  %signed_product = xor i32 %pb, %sub_sign
  %acc_sign = and i32 %acc, -2147483648
  %acc_sat = or i32 %acc_sign, 2147483647
  %acc_or_sum = select i1 %latched, i32 %acc_sat, i32 %sb
  %bits = select i1 %po, i32 %signed_product, i32 %acc_or_sum
  %prior = or i1 %po, %latched
  %overflow = or i1 %prior, %so
  %no_prior = xor i1 %prior, true
  %underflow = and i1 %no_prior, %su
  %exception = or i1 %overflow, %underflow
  %no_po = xor i1 %po, true
  %product_events = and i1 %exception, %no_po
  %pexp = and i32 %pb, 2139095040
  %pz = icmp eq i32 %pexp, 0
  %ps = icmp slt i32 %pb, 0
  %z_event = and i1 %pz, %product_events
  %s_event = and i1 %ps, %product_events
  %z_sticky = select i1 %z_event, i32 64, i32 0
  %s_sticky = select i1 %s_event, i32 128, i32 0
  %u_sticky = select i1 %pu, i32 256, i32 0
  %zs = or i32 %z_sticky, %s_sticky
  %sticky = or i32 %zs, %u_sticky
  %m0 = insertvalue { i32, i1, i1, i32 } poison, i32 %bits, 0
  %m1 = insertvalue { i32, i1, i1, i32 } %m0, i1 %overflow, 1
  %m2 = insertvalue { i32, i1, i1, i32 } %m1, i1 %underflow, 2
  %m3 = insertvalue { i32, i1, i1, i32 } %m2, i32 %sticky, 3
  ret { i32, i1, i1, i32 } %m3
}

)IR";
inline constexpr char k_vu_q_status_ir[] = R"IR(
define internal i32 @vu_q_status(i32 %old, i32 %a, i32 %b, i1 %is_sqrt, i1 %is_rsqrt) alwaysinline {
  %root_op = or i1 %is_sqrt, %is_rsqrt
  %negative = icmp slt i32 %b, 0
  %invalid_root = and i1 %root_op, %negative
  %a_exp = and i32 %a, 2139095040
  %b_exp = and i32 %b, 2139095040
  %a_zero = icmp eq i32 %a_exp, 0
  %b_zero = icmp eq i32 %b_exp, 0
  %uses_div = xor i1 %is_sqrt, true
  %zero_divisor = and i1 %uses_div, %b_zero
  %zero_flags = select i1 %a_zero, i32 16, i32 32
  %div_flags = select i1 %zero_divisor, i32 %zero_flags, i32 0
  %root_flags = select i1 %invalid_root, i32 16, i32 0
  %current = or i32 %div_flags, %root_flags
  %sticky = shl i32 %current, 6
  %cleared = and i32 %old, -49
  %merged = or i32 %cleared, %current
  %result = or i32 %merged, %sticky
  ret i32 %result
}

)IR";
}
