#!/usr/bin/env python3
"""Verify and execute the initial VU1 integer IR against the real interpreter."""
import ctypes
import pathlib
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm


def main():
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    with tempfile.TemporaryDirectory() as directory:
        for case in ("integer", "normal", "wrap", "stores"):
            path = pathlib.Path(directory) / f"{case}.ll"
            expected = list(map(int, subprocess.check_output(
                [sys.argv[1], str(path), case], text=True).split()))
            for opt in (0, 3):
                module = llvm.parse_assembly(path.read_text(encoding="utf-8"))
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
                    ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32,
                    ctypes.POINTER(ctypes.c_uint32),
                    ctypes.POINTER(ctypes.c_uint32),
                    ctypes.POINTER(ctypes.c_uint8))(address)
                vi = (ctypes.c_uint16 * 16)(0, 0xfffe, 5)
                immediate = ctypes.c_uint32(0x12345678)
                tpc = ctypes.c_uint32(0)
                vf = (ctypes.c_uint32 * 128)()
                vf[3] = 0x3f800000
                vf[8:12] = [0x3f800000, 0x80000001, 0x7fffffff, 0xbf800000]
                vf[12:16] = [0x40000000, 0x80000000, 0x7f7fffff, 0x3f800000]
                vf[24:28] = [11, 22, 33, 44]
                vf[52:56] = [55, 66, 77, 88]
                mac = ctypes.c_uint32(0x1234)
                status = ctypes.c_uint32(0x30)
                data = (ctypes.c_uint8 * 16384)(
                    *((i * 13 + (i >> 8)) & 255 for i in range(16384)))
                steps = function(vi, ctypes.byref(immediate), ctypes.byref(tpc),
                                 0x12345, 0x23456, vf, 0x3f800000,
                                 ctypes.byref(mac), ctypes.byref(status), data)
                # Cycle count is determined statically by the emitter and
                # checked against the interpreter by the fixture.
                actual = [steps, expected[1], tpc.value, immediate.value,
                          *vi, *vf, mac.value, status.value, *data]
                assert actual == expected, (case, opt, actual, expected)
    print("VU1 integer LLVM interpreter differential: PASS")


if __name__ == "__main__":
    main()
