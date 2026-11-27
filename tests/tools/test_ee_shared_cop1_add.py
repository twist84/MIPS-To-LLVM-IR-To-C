#!/usr/bin/env python3
"""Compare emitted addition, MAC and VU0 FMAC results after cross-module sharing."""
import ctypes
from pathlib import Path
import random
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import llvmlite.binding as llvm
from audit_ee_helper_inventory import ir_inventory
from build_ee_cop1_add_experiment import ADD, SHARED_ADD, rewrite_add
from build_ee_cop1_mul_experiment import DEPENDENCIES, ROOTS, rewrite_mul, shared


def emitted(path, index=0):
    header = (ROOT / "src/llvm" / path).read_bytes()
    return header.split(b'R"IR(')[index+1].split(b')IR"', 1)[0]


def wrapper():
    lines = ["define i64 @guest_a(i32 %op, i32 %acc, i32 %a, i32 %b, i32 %latch) {",
             "entry:", "  %subbit = and i32 %op, 1",
             "  %sub = icmp ne i32 %subbit, 0",
             "  %latched = icmp ne i32 %latch, 0",
             "  switch i32 %op, label %vu [ i32 0, label %add i32 1, label %add",
             "      i32 2, label %mac i32 3, label %mac ]"]
    for label, types, call in (
            ("add", ["i32", "i1", "i1"], "@ee_cop1_add_bits(i32 %a, i32 %b, i1 %sub)"),
            ("mac", ["i32", "i1", "i1", "i1"], "@ee_cop1_mac_bits(i32 %acc, i32 %a, i32 %b, i1 %sub, i1 %latched)"),
            ("vu", ["i32", "i1", "i1", "i32"], "@vu_fmac(i32 %vop, i32 %acc, i32 %a, i32 %b, i1 %latched)")):
        lines.append(label + ":")
        if label == "vu":
            lines.append("  %vop = sub i32 %op, 4")
        ty = "{ " + ", ".join(types) + " }"
        lines.append(f"  %{label}r = call {ty} {call}")
        previous = None
        for i, field in enumerate(types):
            name = f"%{label}{i}"
            lines.append(f"  {name} = extractvalue {ty} %{label}r, {i}")
            lines.append(f"  {name}wide = zext {field} {name} to i64")
            if i == 0:
                previous = name + "wide"
            else:
                lines.append(f"  {name}shift = shl i64 {name}wide, {31+i}")
                lines.append(f"  {name}pack = or i64 {previous}, {name}shift")
                previous = name + "pack"
        lines.append(f"  ret i64 {previous}")
    lines.append("}")
    return ("\n".join(lines) + "\n").encode()


def main():
    raw = (b"declare i32 @llvm.ctlz.i32(i32, i1)\n" +
           emitted("ee_cop1_add_ir.h") + emitted("ee_cop1_mul_ir.h") +
           emitted("ee_cop1_mul_ir.h", 1) + emitted("ee_vu_fmac_ir.h") + wrapper())
    info = ir_inventory(raw, "guest_a")
    expected = info[ADD]["body_sha256"]
    mul = {n: info[n]["body_sha256"] for n in DEPENDENCIES}
    other = raw.replace(b"@guest_a", b"@guest_b")
    # Put the addition and multiplication owners in different modules.
    parent_a = rewrite_mul(raw, raw, "guest_a", mul, True)
    parent_b = rewrite_mul(other, other, "guest_b", mul, False)
    a = rewrite_add(raw, parent_a, "guest_a", expected, False)
    b = rewrite_add(other, parent_b, "guest_b", expected, True)
    for source, prior in (
            (raw.replace(b"%safe_shift = and", b"%safe_shift = or"), parent_a),
            (raw, parent_a.replace(b"%safe_shift = and", b"%safe_shift = or")),
            (raw, parent_a + b"\n@escape = global ptr @ee_cop1_add_bits\n"),
            (raw, parent_a.replace(b"declare i32 @llvm.ctlz.i32(i32, i1)",
                                   b"declare i32 @llvm.ctlz.i32(i32, i32)"))):
        try:
            rewrite_add(source, prior, "guest_a", expected, False)
        except ValueError:
            pass
        else:
            raise AssertionError("unsafe addition sharing accepted")
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    machine = llvm.Target.from_default_triple().create_target_machine()
    with llvm.create_context() as ctx:
        modules = [llvm.parse_assembly(text.decode(), context=ctx)
                   for text in (a, b, raw.replace(b"@guest_a", b"@reference"))]
        for module in modules:
            module.verify()
        linked, second, ref_module = modules
        llvm.link_modules(linked, second)
        llvm.link_modules(linked, ref_module)
        linked.verify()
        for name in (SHARED_ADD, *(shared(n) for n in ROOTS)):
            assert sum(fn.name == name and not fn.is_declaration for fn in linked.functions) == 1
        with llvm.create_mcjit_compiler(linked, machine) as engine:
            engine.finalize_object()
            signature = ctypes.CFUNCTYPE(ctypes.c_uint64, *([ctypes.c_uint32] * 5))
            ref, f, g = [signature(engine.get_function_address(n))
                         for n in ("reference", "guest_a", "guest_b")]
            edge = [0, 1, 0x80000000, 0x007fffff, 0x00800000, 0x00800001,
                    0x3f800000, 0x3f800001, 0xbf800000, 0x7f7fffff,
                    0x7f800000, 0x7fffffff, 0xffffffff]
            rng = random.Random(5900)
            cases = [(op, edge[(i+j) % len(edge)], x, y, latch)
                     for op in range(9) for i, x in enumerate(edge)
                     for j, y in enumerate(edge) for latch in (0, 1)]
            cases.extend((rng.randrange(9), rng.getrandbits(32), rng.getrandbits(32),
                          rng.getrandbits(32), rng.randrange(2)) for _ in range(10000))
            for case in cases:
                assert ref(*case) == f(*case) == g(*case), case
    print("PASS: shared COP1 addition, MAC and VU0 FMAC bit/flag equivalence")


if __name__ == "__main__":
    main()
