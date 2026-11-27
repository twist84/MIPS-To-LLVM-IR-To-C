#!/usr/bin/env python3
"""Run opt-in strict native LW/SW against independent interpreter state."""

import ctypes
import pathlib
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
from verify_ir import optimize_guest_ir


def main():
    fixture = pathlib.Path(sys.argv[1]).resolve()
    helper = ctypes.CDLL(str(pathlib.Path(sys.argv[2]).resolve()))
    llvm.add_symbol(
        "ee_strict_native_prepare_word",
        ctypes.cast(helper.ee_strict_native_prepare_word, ctypes.c_void_p).value,
    )
    test = helper.ee_test_strict_native_word
    test.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    test.restype = ctypes.c_uint32
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    count = 0
    with tempfile.TemporaryDirectory(prefix="ee-strict-word-") as tmp:
        path = pathlib.Path(tmp) / "word.ll"
        for primary in (0x23, 0x2b):
            for rt in (0, 2):
                opcode = (primary << 26) | (4 << 21) | (rt << 16) | 0xfffc
                subprocess.run([str(fixture), str(path), "0x00400000", hex(opcode)], check=True)
                source = path.read_text(encoding="utf-8")
                rejected = source.split("rejected:", 1)[1].split("transfer:", 1)[0]
                if "ret i32 %status" not in rejected or "store" in rejected or "load" in rejected:
                    raise RuntimeError("rejected edge is not side-effect-free")
                for opt in (0, 2):
                    machine = target.create_target_machine(opt=opt)
                    module = llvm.parse_assembly(source)
                    module.triple = llvm.get_default_triple()
                    module.data_layout = str(machine.target_data)
                    module.verify()
                    if opt:
                        optimize_guest_ir(module, machine)
                    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), machine)
                    engine.add_module(module)
                    engine.finalize_object()
                    address = engine.get_function_address("guest_strict_word")
                    if not address or test(address, opcode) != 12:
                        raise RuntimeError(f"strict native LW/SW failed: {opcode:#x} O{opt}")
                    count += 12
        # The separate emitter must not accept arbitrary instructions.
        rejected = subprocess.run([str(fixture), str(path), "0x00400000", "0x0"])
        if rejected.returncode != 3:
            raise RuntimeError("strict emitter accepted unsupported opcode")
    print(f"ee_strict_word_ir_runtime: PASS ({count} native/interpreter cases)")


if __name__ == "__main__":
    main()
