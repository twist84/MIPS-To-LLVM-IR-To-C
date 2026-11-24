#!/usr/bin/env python3
"""Verify and execute the initial VU1 integer IR against the real interpreter."""
import ctypes
import pathlib
import random
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm


def verify_fmac(ir):
    # Exercise the generated result/exception tuple independently of
    # the interpreter differential, at both optimization levels.
    assert "@vu_fmac(" in ir
    wrapper = """
define void @test_fmac(i32 %op, i32 %acc, i32 %a, i32 %b, i32 %latch, ptr %out) {
  %latched = icmp ne i32 %latch, 0
  %r = call { i32, i1, i1, i32 } @vu_fmac(i32 %op, i32 %acc, i32 %a, i32 %b, i1 %latched)
  %bits = extractvalue { i32, i1, i1, i32 } %r, 0
  %o = extractvalue { i32, i1, i1, i32 } %r, 1
  %u = extractvalue { i32, i1, i1, i32 } %r, 2
  %sticky = extractvalue { i32, i1, i1, i32 } %r, 3
  %oi = zext i1 %o to i32
  %ui = zext i1 %u to i32
  %p1 = getelementptr i32, ptr %out, i32 1
  %p2 = getelementptr i32, ptr %out, i32 2
  %p3 = getelementptr i32, ptr %out, i32 3
  store i32 %bits, ptr %out
  store i32 %oi, ptr %p1
  store i32 %ui, ptr %p2
  store i32 %sticky, ptr %p3
  ret void
}
"""
    # Literal hardware cancellation/multiplier vectors and modeled MAC
    # priority controls. The latter require additional console validation.
    cases = [
        ((1, 0, 0x0080008b, 0x008000ea, 0), (0x803e0000, 0, 1, 0)),
        ((0, 0, 0x8080002c, 0x008000ec, 0), (0x00400000, 0, 1, 0)),
        ((0, 0, 0x80800036, 0x00829f6a, 0), (0x0027cd00, 0, 1, 0)),
        ((0, 0, 0x80c1aa65, 0x01200000, 0), (0x007cab36, 0, 1, 0)),
        ((2, 0, 0x3fffffff, 0x3f800000, 0), (0x3fffffff, 0, 0, 0)),
        ((2, 0, 0x3f800000, 0x3fffffff, 0), (0x3ffffffe, 0, 0, 0)),
        ((2, 0, 0x7fffffff, 0x3f800000, 0), (0x7fffffff, 0, 0, 0)),
        ((2, 0, 0x16a08bd5, 0x03f4fe21, 0), (0, 0, 1, 0)),
        ((3, 0x01000000, 0x00800000, 0x3f000000, 0), (0x01000000, 0, 0, 0x100)),
        ((3, 0x7fffffff, 0x7fffffff, 0xbf800000, 1), (0x7fffffff, 1, 0, 0x80)),
        ((4, 0x7fffffff, 0x7fffffff, 0x40000000, 1), (0xffffffff, 1, 0, 0)),
    ]
    for opt in (0, 3):
        module = llvm.parse_assembly(ir + wrapper)
        module.verify()
        target = llvm.Target.from_default_triple().create_target_machine(opt=opt)
        module.triple = llvm.get_default_triple()
        module.data_layout = str(target.target_data)
        engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target)
        engine.add_module(module)
        engine.finalize_object()
        address = engine.get_function_address("test_fmac")
        assert address
        function = ctypes.CFUNCTYPE(None, *([ctypes.c_uint32] * 5),
                                    ctypes.POINTER(ctypes.c_uint32))(address)
        out = (ctypes.c_uint32 * 4)()
        for args, expected in cases:
            function(*args, out)
            assert tuple(out) == expected, (opt, args, tuple(out), expected)


def verify_minmax(ir):
    wrapper = """
define i32 @test_max(i32 %a, i32 %b) {
  %r = call i32 @vu_max(i32 %a, i32 %b)
  ret i32 %r
}
define i32 @test_min(i32 %a, i32 %b) {
  %r = call i32 @vu_min(i32 %a, i32 %b)
  ret i32 %r
}
"""
    # Explicit reference pairs: (a, b, max, min). Check both operand orders.
    vectors = [
        (0x80000000, 0, 0, 0x80000000),
        (0, 1, 1, 0),
        (1, 2, 2, 1),
        (0x80000001, 0x80000002, 0x80000001, 0x80000002),
        (0x80000000, 0x80000001, 0x80000000, 0x80000001),
        (0x7f800000, 0x7f800001, 0x7f800001, 0x7f800000),
        (0xff800000, 0xff800001, 0xff800000, 0xff800001),
        (0x7f7fffff, 0x7f800000, 0x7f800000, 0x7f7fffff),
        (0xffffffff, 0x7fffffff, 0x7fffffff, 0xffffffff),
    ]
    signature = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32)
    for opt in (0, 3):
        module = llvm.parse_assembly(ir + wrapper)
        module.verify()
        target = llvm.Target.from_default_triple().create_target_machine(opt=opt)
        module.triple = llvm.get_default_triple()
        module.data_layout = str(target.target_data)
        engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target)
        engine.add_module(module)
        engine.finalize_object()
        addresses = [engine.get_function_address(name) for name in ("test_max", "test_min")]
        assert all(addresses)
        vmax, vmin = map(signature, addresses)
        for a, b, maximum, minimum in vectors:
            for x, y in ((a, b), (b, a)):
                assert vmax(x, y) == maximum, (opt, x, y, "MAX")
                assert vmin(x, y) == minimum, (opt, x, y, "MINI")


def main():
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    with tempfile.TemporaryDirectory() as directory:
        for case in ("integer", "normal", "wrap", "stores", "branches", "branch-wrap", "acc", "acc-branches", "conversions", "flags", "q", "random"):
            path = pathlib.Path(directory) / f"{case}.ll"
            expected = list(map(int, subprocess.check_output(
                [sys.argv[1], str(path), case], text=True).split()))
            ir = path.read_text(encoding="utf-8")
            if case == "acc":
                verify_fmac(ir)
                verify_minmax(ir)
            for opt in (0, 3):
                module = llvm.parse_assembly(ir)
                module.verify()
                target = llvm.Target.from_default_triple().create_target_machine(opt=opt)
                module.triple = llvm.get_default_triple()
                module.data_layout = str(target.target_data)
                engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target)
                engine.add_module(module)
                engine.finalize_object()
                address = engine.get_function_address("ee_vu1_program")
                assert address, (case, opt)
                function = ctypes.CFUNCTYPE(
                    ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint16),
                    ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32),
                    ctypes.c_uint32, ctypes.c_uint32,
                    ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32),
                    ctypes.POINTER(ctypes.c_uint32),
                    ctypes.POINTER(ctypes.c_uint32),
                    ctypes.POINTER(ctypes.c_uint8),
                    ctypes.POINTER(ctypes.c_uint32),
                    ctypes.POINTER(ctypes.c_uint32),
                    ctypes.POINTER(ctypes.c_uint32),
                    ctypes.POINTER(ctypes.c_uint32),
                    ctypes.c_void_p, ctypes.c_void_p,
                    ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32),
                    ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32))(address)
                vi = (ctypes.c_uint16 * 16)(0, 0xfffe, 5)
                immediate = ctypes.c_uint32(0x12345678)
                tpc = ctypes.c_uint32(0)
                vf = (ctypes.c_uint32 * 128)()
                vf[3] = 0x3f800000
                vf[8:12] = [0x3f800000, 0x80000001, 0x7fffffff, 0xbf800000]
                vf[12:16] = [0x40000000, 0x80000000, 0x7f7fffff, 0x3f800000]
                vf[24:28] = [11, 22, 33, 44]
                vf[52:56] = [55, 66, 77, 88]
                vf[84:88] = [0x3f800001, 0xbf800001, 0x7fffffff, 0x00800000]
                vf[88:92] = [0x40000000, 0x3f800000, 0x80000000, 0x00800001]
                vf[103] = 0x12345678
                acc = (ctypes.c_uint32 * 4)(0x3f000000, 0xbf800000, 0x80000000, 0x7fffffff)
                mac = ctypes.c_uint32(0x1234)
                status = ctypes.c_uint32(0x30)
                clip = ctypes.c_uint32(0x345678)
                q = ctypes.c_uint32(0x3f800000)
                p = ctypes.c_uint32(0x12345678)
                r = ctypes.c_uint32(0x54321)
                data = (ctypes.c_uint8 * 16384)(
                    *((i * 13 + (i >> 8)) & 255 for i in range(16384)))
                if case == "conversions":
                    edges = [
                        0, 0x80000000, 1, 0x807fffff, 0x00800000, 0x80800000,
                        0x3f7fffff, 0xbf7fffff, 0x3f800000, 0xbf800000,
                        0x3fc00000, 0xbfc00000, 0x4effffff, 0xceffffff,
                        0x4f000000, 0xcf000000, 0x7fffffff, 0xffffffff,
                        0x7f800000, 0xff800000, 0x00ffffff, 0x01000001,
                        0x7ffffffe, 0x80000001, 0x01000000, 0x01000001,
                        0x01ffffff, 0x02000001, 0xfeffffff, 0x3f000000,
                        0x40000000, 0xc0000000,
                    ]
                    random = 0x12345678
                    for i in range(128):
                        random = (random * 1664525 + 1013904223) & 0xffffffff
                        value = edges[i] if i < len(edges) else random
                        offset = 0x3000 + i * 4
                        data[offset:offset + 4] = value.to_bytes(4, "little")
                cycles = ctypes.c_uint32(0)
                last_pc = ctypes.c_uint32(0)
                acc_overflow = ctypes.c_uint32(0)
                steps = function(vi, ctypes.byref(immediate), ctypes.byref(tpc),
                                 0x12345, 0x23456, vf, ctypes.byref(q),
                                 ctypes.byref(mac), ctypes.byref(status), data,
                                 ctypes.byref(cycles), ctypes.byref(last_pc), acc, ctypes.byref(clip), None, None,
                                 ctypes.byref(p), ctypes.byref(r), None, ctypes.byref(acc_overflow))
                actual = [steps, cycles.value, tpc.value, immediate.value, last_pc.value,
                          *vi, *vf, mac.value, status.value, clip.value, q.value, p.value, r.value, *acc, acc_overflow.value, *data]
                assert actual == expected, (case, opt, actual, expected)
    print("VU1 integer LLVM interpreter differential: PASS")


if __name__ == "__main__":
    main()
