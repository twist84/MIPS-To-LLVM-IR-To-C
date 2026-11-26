#!/usr/bin/env python3
"""Compare real emitted multiplier bodies with shared cross-module calls."""
import ctypes
from pathlib import Path
import random
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import llvmlite.binding as llvm
from audit_ee_helper_inventory import ir_inventory
from build_ee_cop1_mul_experiment import DEPENDENCIES, ROOTS, rewrite_mul, shared

WRAPPER = b"""
define i64 @guest_a(i32 %a, i32 %b) {
entry:
  %out = call { i32, i1, i1 } @ee_cop1_mul_bits(i32 %a, i32 %b)
  %bits = extractvalue { i32, i1, i1 } %out, 0
  %over = extractvalue { i32, i1, i1 } %out, 1
  %under = extractvalue { i32, i1, i1 } %out, 2
  %bits64 = zext i32 %bits to i64
  %o = zext i1 %over to i64
  %u = zext i1 %under to i64
  %os = shl i64 %o, 32
  %us = shl i64 %u, 33
  %combined = or i64 %bits64, %os
  %packed = or i64 %combined, %us
  ret i64 %packed
}
define internal i64 @vu_test(i32 %a, i32 %b, i64 %full) {
entry:
  %r = call i64 @ee_cop1_mul_correct(i32 %a, i32 %b, i64 %full)
  ret i64 %r
}
"""


def main():
    header = (ROOT / "src/llvm/ee_cop1_mul_ir.h").read_bytes()
    body = header.split(b'R"IR(', 1)[1].split(b')IR"', 1)[0]
    raw = body + WRAPPER
    info = ir_inventory(raw, "guest_a")
    expected = {n: info[n]["body_sha256"] for n in DEPENDENCIES}
    owner = rewrite_mul(raw, raw, "guest_a", expected, True)
    other = raw.replace(b"guest_a", b"guest_b")
    nonowner = rewrite_mul(other, other, "guest_b", expected, False)
    for bad in (other.replace(b"%lost64 = zext", b"%lost64 = sext"),
                other + b"\n@escape = global ptr @ee_cop1_booth_row\n"):
        try:
            rewrite_mul(bad, bad, "guest_b", expected, False)
        except ValueError:
            pass
        else:
            raise AssertionError("unsafe multiply sharing accepted")
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    machine = llvm.Target.from_default_triple().create_target_machine()
    with llvm.create_context() as ctx:
        reference = llvm.parse_assembly(raw.replace(b"guest_a", b"reference").decode(), context=ctx)
        a = llvm.parse_assembly(owner.decode(), context=ctx)
        b = llvm.parse_assembly(nonowner.decode(), context=ctx)
        for m in (reference, a, b):
            m.verify()
        llvm.link_modules(a, b)
        llvm.link_modules(a, reference)
        a.verify()
        for n in ROOTS:
            assert sum(fn.name == shared(n) and not fn.is_declaration for fn in a.functions) == 1
        with llvm.create_mcjit_compiler(a, machine) as engine:
            engine.finalize_object()
            signature = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32)
            ref, f, g = [signature(engine.get_function_address(n)) for n in ("reference", "guest_a", "guest_b")]
            edge = [0, 1, 0x80000000, 0x007fffff, 0x00800000, 0x3f800000,
                    0x3f800001, 0x7f7fffff, 0x7f800000, 0x7fffffff, 0xffffffff]
            rng = random.Random(5900)
            pairs = [(x, y) for x in edge for y in edge]
            pairs.extend((rng.getrandbits(32), rng.getrandbits(32)) for _ in range(10000))
            for x, y in pairs:
                assert ref(x, y) == f(x, y) == g(x, y)
    print("PASS: shared COP1 multiplier bit/flag equivalence")


if __name__ == "__main__":
    main()
