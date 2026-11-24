#!/usr/bin/env python3
"""Verify and execute the initial VU1 integer IR against the real interpreter."""
import ctypes
import pathlib
import random
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm


def verify_mul4(ir):
    # Test the emitted helper itself, independently of the instruction
    # fixture's interpreter comparison. All four lanes vary independently.
    assert "call <4 x i32> @vu_mul4(" in ir
    wrapper = """
define void @test_mul4(ptr %a, ptr %b, ptr %out) {
  %va = load <4 x i32>, ptr %a, align 4
  %vb = load <4 x i32>, ptr %b, align 4
  %r = call <4 x i32> @vu_mul4(<4 x i32> %va, <4 x i32> %vb)
  store <4 x i32> %r, ptr %out, align 4
  ret void
}
"""
    def scalar(a, b):
        sign = (a ^ b) & 0x80000000
        ea, eb = (a >> 23) & 255, (b >> 23) & 255
        if not ea or not eb:
            return sign
        product = ((a & 0x7fffff) | 0x800000) * ((b & 0x7fffff) | 0x800000)
        carry = product >> 47
        exponent = ea + eb - 127 + carry
        if exponent > 255:
            return sign | 0x7fffffff
        if exponent <= 0:
            return sign
        return sign | (exponent << 23) | ((product >> (23 + carry)) & 0x7fffff)

    edges = [0, 0x80000000, 1, 0x807fffff, 0x00800000, 0x80800000,
             0x3f7fffff, 0xbf7fffff, 0x3f800000, 0xbf800000,
             0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000,
             0x7fffffff, 0xffffffff]
    rng = random.Random(0x12345678)
    pairs = [(a, b) for a in edges for b in edges]
    pairs += [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(16384)]
    lanes = ctypes.c_uint32 * 4
    for opt in (0, 3):
        module = llvm.parse_assembly(ir + wrapper)
        module.verify()
        target = llvm.Target.from_default_triple().create_target_machine(opt=opt)
        module.triple = llvm.get_default_triple()
        module.data_layout = str(target.target_data)
        engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target)
        engine.add_module(module)
        engine.finalize_object()
        address = engine.get_function_address("test_mul4")
        assert address
        function = ctypes.CFUNCTYPE(None, *([ctypes.POINTER(ctypes.c_uint32)] * 3))(address)
        out = lanes()
        for i in range(0, len(pairs), 4):
            group = pairs[i:i + 4]
            a, b = zip(*group)
            function(lanes(*a), lanes(*b), out)
            assert list(out) == [scalar(x, y) for x, y in group], (opt, group)


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
                verify_mul4(ir)
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
                    ctypes.c_void_p)(address)
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
                steps = function(vi, ctypes.byref(immediate), ctypes.byref(tpc),
                                 0x12345, 0x23456, vf, ctypes.byref(q),
                                 ctypes.byref(mac), ctypes.byref(status), data,
                                 ctypes.byref(cycles), ctypes.byref(last_pc), acc, ctypes.byref(clip), None, None,
                                 ctypes.byref(p), ctypes.byref(r), None)
                actual = [steps, cycles.value, tpc.value, immediate.value, last_pc.value,
                          *vi, *vf, mac.value, status.value, clip.value, q.value, p.value, r.value, *acc, *data]
                assert actual == expected, (case, opt, actual, expected)
    print("VU1 integer LLVM interpreter differential: PASS")


if __name__ == "__main__":
    main()
