#!/usr/bin/env python3
"""Check opt-in VU1 multiply profile IR instrumentation."""
import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "build_vu1_aot", ROOT / "tools" / "build_vu1_aot.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def main():
    source = """define internal { <4 x i32>, <4 x i1>, <4 x i1> } @vu_mul4(<4 x i32> %lhs, <4 x i32> %rhs) alwaysinline {
entry:
  %mask = bitcast <4 x i1> zeroinitializer to i4
  %any = icmp ne i4 %mask, 0
  br i1 %any, label %correct, label %normalize
correct:
  %result = call <4 x i64> @vu_mul_correct4(<4 x i32> zeroinitializer, <4 x i32> zeroinitializer, <4 x i64> zeroinitializer, <4 x i1> zeroinitializer)
  br label %normalize
normalize:
  ret { <4 x i32>, <4 x i1>, <4 x i1> } zeroinitializer
}
"""
    instrumented = MODULE.instrument_mul_corrections(source)
    required = (
        "@vu_mul_profile_counts = internal global [3 x i64]",
        "declare i4 @llvm.ctpop.i4(i4)",
        "%vu_mul_profile_call_new = add i64 %vu_mul_profile_call_old, 1",
        "%vu_mul_profile_correct_new = add i64 %vu_mul_profile_correct_old, 1",
        "call i4 @llvm.ctpop.i4(i4 %mask)",
        "@ee_vu1_mul_profile(ptr %out)",
    )
    if any(item not in instrumented for item in required):
        raise SystemExit("profile instrumentation missed a counter or accessor")
    if instrumented.count("%vu_mul_profile_call_new =") != 1:
        raise SystemExit("mul4 call counter was injected more than once")
    try:
        MODULE.instrument_mul_corrections("define void @missing() { ret void }\n")
    except ValueError:
        pass
    else:
        raise SystemExit("profile instrumentation accepted IR without vu_mul4")
    print("VU1 multiply correction profile instrumentation: PASS")


if __name__ == "__main__":
    main()
