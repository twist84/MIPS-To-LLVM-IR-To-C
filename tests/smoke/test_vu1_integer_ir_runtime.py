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
        for case in ("normal", "wrap"):
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
                address = engine.get_function_address("ee_vu1_integer_program")
                assert address, (case, opt)
                function = ctypes.CFUNCTYPE(
                    ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint16),
                    ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32),
                    ctypes.c_uint32, ctypes.c_uint32)(address)
                vi = (ctypes.c_uint16 * 16)(0, 0xfffe, 5)
                immediate = ctypes.c_uint32(0x12345678)
                tpc = ctypes.c_uint32(0)
                steps = function(vi, ctypes.byref(immediate), ctypes.byref(tpc),
                                 0x12345, 0x23456)
                assert [steps, steps, tpc.value, immediate.value, *vi] == expected, (
                    case, opt, expected, list(vi))
    print("VU1 integer LLVM interpreter differential: PASS")


if __name__ == "__main__":
    main()
