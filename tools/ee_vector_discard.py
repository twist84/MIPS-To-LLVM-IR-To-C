"""Exact all-lane VU discard fast paths for isolated IR experiments."""

import re

SHAPE = "{ <4 x i32>, <4 x i1>, <4 x i1> }"
MUL_SIGN = ("  %sign = and <4 x i32> %sign_xor, "
            "<i32 -2147483648, i32 -2147483648, i32 -2147483648, i32 -2147483648>\n")
ADD_ZERO = "  %b_zero = icmp eq <4 x i32> %eb, <i32 0, i32 0, i32 0, i32 0>\n"
ADD_ALIGN = """  %e = call <4 x i32> @llvm.umax.v4i32(<4 x i32> %ea, <4 x i32> %eb)
  %da = sub <4 x i32> %e, %ea
  %db = sub <4 x i32> %e, %eb
"""


def replace_one(text, old, new):
    if text.count(old) != 1:
        raise ValueError("VU discard template anchor differs: " + old[:80])
    return text.replace(old, new, 1)


def helper(text, name):
    matches = list(re.finditer(r"^define [^\n]* @" + re.escape(name) + r"\(", text, re.M))
    if len(matches) != 1:
        raise ValueError("expected exactly one helper: " + name)
    start = matches[0].start()
    end = text.index("\n}", start) + 2
    return text[start:end]


def vector_discard(raw):
    text = raw.decode("utf-8")
    old = helper(text, "vu_mul4")
    new = replace_one(old, MUL_SIGN, MUL_SIGN + f"""  %zero_mask = bitcast <4 x i1> %zero to i4
  %all_zero = icmp eq i4 %zero_mask, -1
  br i1 %all_zero, label %zero_product, label %multiply
zero_product:
  %zero_tuple = insertvalue {SHAPE} zeroinitializer, <4 x i32> %sign, 0
  ret {SHAPE} %zero_tuple
multiply:
""")
    new = replace_one(new, "[ %full, %entry ]", "[ %full, %multiply ]")
    text = replace_one(text, old, new)
    old = helper(text, "vu_add4")
    # Move exponent alignment before the branch; leave the slow path
    # otherwise unchanged. A discarded lane contributes no guarded bits.
    new = replace_one(old, ADD_ALIGN, "")
    new = replace_one(new, ADD_ZERO, ADD_ZERO + ADD_ALIGN + f"""  %discard_a_dist = icmp uge <4 x i32> %da, <i32 25, i32 25, i32 25, i32 25>
  %discard_b_dist = icmp uge <4 x i32> %db, <i32 25, i32 25, i32 25, i32 25>
  %discard_a = or <4 x i1> %a_zero, %discard_a_dist
  %discard_b = or <4 x i1> %b_zero, %discard_b_dist
  %one_discard = or <4 x i1> %discard_a, %discard_b
  %discard_mask = bitcast <4 x i1> %one_discard to i4
  %all_discard = icmp eq i4 %discard_mask, -1
  br i1 %all_discard, label %discard, label %add
discard:
  %both_discard = and <4 x i1> %discard_a, %discard_b
  %survivor = select <4 x i1> %discard_a, <4 x i32> %b, <4 x i32> %lhs
  %zero_sign_raw = and <4 x i32> %lhs, %b
  %zero_sign = and <4 x i32> %zero_sign_raw, <i32 -2147483648, i32 -2147483648, i32 -2147483648, i32 -2147483648>
  %discard_result = select <4 x i1> %both_discard, <4 x i32> %zero_sign, <4 x i32> %survivor
  %discard_tuple = insertvalue {SHAPE} zeroinitializer, <4 x i32> %discard_result, 0
  ret {SHAPE} %discard_tuple
add:
""")
    text = replace_one(text, old, new)
    return text.encode("utf-8"), ["vu_mul4", "vu_add4"]
