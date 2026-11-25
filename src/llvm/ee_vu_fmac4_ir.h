#pragma once

// Four-lane integer FMAC. Mirrors the scalar EE/VU arithmetic; only
// potentially observable Booth corrections enter the scalar cold path.
namespace llvm
{
inline constexpr char k_vu_fmac4_ir[] = R"IR(
declare <4 x i32> @llvm.ctlz.v4i32(<4 x i32>, i1 immarg)

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
  %a_larger = icmp uge <4 x i32> %ea, %eb
  %e = select <4 x i1> %a_larger, <4 x i32> %ea, <4 x i32> %eb
  %da = sub <4 x i32> %e, %ea
  %db = sub <4 x i32> %e, %eb
  %a_far = icmp uge <4 x i32> %da, <i32 25, i32 25, i32 25, i32 25>
  %b_far = icmp uge <4 x i32> %db, <i32 25, i32 25, i32 25, i32 25>
  %sa = select <4 x i1> %a_far, <4 x i32> <i32 25, i32 25, i32 25, i32 25>, <4 x i32> %da
  %sb = select <4 x i1> %b_far, <4 x i32> <i32 25, i32 25, i32 25, i32 25>, <4 x i32> %db
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
  %negative = icmp slt <4 x i32> %sum, <i32 0, i32 0, i32 0, i32 0>
  %neg_sum = sub <4 x i32> <i32 0, i32 0, i32 0, i32 0>, %sum
  %magnitude = select <4 x i1> %negative, <4 x i32> %neg_sum, <4 x i32> %sum
  %leading = call <4 x i32> @llvm.ctlz.v4i32(<4 x i32> %magnitude, i1 false)
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
  %both_negative = and <4 x i1> %a_negative, %b_negative
  %result_negative = select <4 x i1> %zero, <4 x i1> %both_negative, <4 x i1> %negative
  %sign = select <4 x i1> %result_negative, <4 x i32> <i32 -2147483648, i32 -2147483648, i32 -2147483648, i32 -2147483648>, <4 x i32> <i32 0, i32 0, i32 0, i32 0>
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

define internal <4 x i64> @vu_mul_correct4(<4 x i32> %ma, <4 x i32> %mb, <4 x i64> %full, <4 x i1> %needs) noinline {
entry:
  %needs0 = extractelement <4 x i1> %needs, i32 0
  br i1 %needs0, label %correct0, label %merge0
correct0:
  %a0 = extractelement <4 x i32> %ma, i32 0
  %b0 = extractelement <4 x i32> %mb, i32 0
  %p0 = extractelement <4 x i64> %full, i32 0
  %c0 = call i64 @ee_cop1_mul_correct(i32 %a0, i32 %b0, i64 %p0)
  %i0 = insertelement <4 x i64> %full, i64 %c0, i32 0
  br label %merge0
merge0:
  %v0 = phi <4 x i64> [ %full, %entry ], [ %i0, %correct0 ]
  %needs1 = extractelement <4 x i1> %needs, i32 1
  br i1 %needs1, label %correct1, label %merge1
correct1:
  %a1 = extractelement <4 x i32> %ma, i32 1
  %b1 = extractelement <4 x i32> %mb, i32 1
  %p1 = extractelement <4 x i64> %full, i32 1
  %c1 = call i64 @ee_cop1_mul_correct(i32 %a1, i32 %b1, i64 %p1)
  %i1 = insertelement <4 x i64> %v0, i64 %c1, i32 1
  br label %merge1
merge1:
  %v1 = phi <4 x i64> [ %v0, %merge0 ], [ %i1, %correct1 ]
  %needs2 = extractelement <4 x i1> %needs, i32 2
  br i1 %needs2, label %correct2, label %merge2
correct2:
  %a2 = extractelement <4 x i32> %ma, i32 2
  %b2 = extractelement <4 x i32> %mb, i32 2
  %p2 = extractelement <4 x i64> %full, i32 2
  %c2 = call i64 @ee_cop1_mul_correct(i32 %a2, i32 %b2, i64 %p2)
  %i2 = insertelement <4 x i64> %v1, i64 %c2, i32 2
  br label %merge2
merge2:
  %v2 = phi <4 x i64> [ %v1, %merge1 ], [ %i2, %correct2 ]
  %needs3 = extractelement <4 x i1> %needs, i32 3
  br i1 %needs3, label %correct3, label %merge3
correct3:
  %a3 = extractelement <4 x i32> %ma, i32 3
  %b3 = extractelement <4 x i32> %mb, i32 3
  %p3 = extractelement <4 x i64> %full, i32 3
  %c3 = call i64 @ee_cop1_mul_correct(i32 %a3, i32 %b3, i64 %p3)
  %i3 = insertelement <4 x i64> %v2, i64 %c3, i32 3
  br label %merge3
merge3:
  %v3 = phi <4 x i64> [ %v2, %merge2 ], [ %i3, %correct3 ]
  ret <4 x i64> %v3
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
  %low23 = and <4 x i64> %full, <i64 8388607, i64 8388607, i64 8388607, i64 8388607>
  %boundary = icmp ult <4 x i64> %low23, <i64 32768, i64 32768, i64 32768, i64 32768>
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
  %norm = lshr <4 x i32> %sig, %shift
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
