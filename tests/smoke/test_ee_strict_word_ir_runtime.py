#!/usr/bin/env python3
"""Run all 21 opt-in strict native GPR memory forms against interpreter state."""

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
        "ee_strict_native_prepare_transfer",
        ctypes.cast(helper.ee_strict_native_prepare_transfer, ctypes.c_void_p).value,
    )
    for name in ("ee_strict_native_check_branch", "ee_strict_native_prepare_delay_transfer"):
        llvm.add_symbol(name, ctypes.cast(getattr(helper, name), ctypes.c_void_p).value)
    test = helper.ee_test_strict_native_word
    test.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    test.restype = ctypes.c_uint32
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    count = 0
    with tempfile.TemporaryDirectory(prefix="ee-strict-word-") as tmp:
        path = pathlib.Path(tmp) / "word.ll"
        widths = {
            0x20: 1, 0x24: 1, 0x21: 2, 0x25: 2, 0x23: 4, 0x27: 4, 0x37: 8,
            0x28: 1, 0x29: 2, 0x2b: 4, 0x3f: 8, 0x1e: 16, 0x1f: 16,
            0x22: 4, 0x26: 4, 0x1a: 8, 0x1b: 8,
            0x2a: 4, 0x2e: 4, 0x2c: 8, 0x2d: 8,
        }
        for primary, width in widths.items():
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
                    expected = 12 + width
                    if not address or test(address, opcode) != expected:
                        raise RuntimeError(f"strict native transfer failed: {opcode:#x} O{opt}")
                    count += expected
        sequence = helper.ee_test_strict_native_sequence
        sequence.argtypes = [ctypes.c_void_p]
        sequence.restype = ctypes.c_uint32
        subprocess.run([str(fixture), str(path), "0x00400ff8",
                        "0xac820000", "0x8cc50000", "0xac850004"], check=True)
        source = path.read_text(encoding="utf-8")
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
            if not address or sequence(address) != 8:
                raise RuntimeError(f"strict native sequence failed: O{opt}")
            count += 8
        branch_test = helper.ee_test_strict_native_branch
        branch_test.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
        branch_test.restype = ctypes.c_uint32
        for primary in (4, 5, 0x14, 0x15):
            for rt in (0, 3):
                branch = (primary << 26) | (5 << 21) | (rt << 16) | 3
                for slot in (0x8c850000, 0xac850000):
                    subprocess.run([str(fixture), str(path), "0x00400ffc",
                                    hex(branch), hex(slot)], check=True)
                    source = path.read_text(encoding="utf-8")
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
                        if not address or branch_test(address, branch, slot) != 17:
                            raise RuntimeError(f"strict native branch failed: {branch:#x}/{slot:#x} O{opt}")
                        count += 17
        # A branch in a memory sequence, unsupported opcode, or wrapping sequence must not
        # silently enter the straight-line lowering.
        for pc, words in (
            ("0x00400000", ["0xac820000", "0x10000000"]),
            ("0x00400000", ["0x10000000", "0x10000000"]),
            ("0x00400000", ["0x0c100000", "0xac820000"]),
            ("0x00400000", ["0xac820000", "0x0"]),
            ("0xfffffffc", ["0xac820000", "0x8cc50000"]),
        ):
            rejected = subprocess.run([str(fixture), str(path), pc, *words])
            if rejected.returncode != 3:
                raise RuntimeError("strict sequence emitter accepted invalid CFG")
        # The separate emitter must not accept arbitrary instructions.
        rejected = subprocess.run([str(fixture), str(path), "0x00400000", "0x0"])
        if rejected.returncode != 3:
            raise RuntimeError("strict emitter accepted unsupported opcode")
    print(f"ee_strict_word_ir_runtime: PASS ({count} native/interpreter cases)")


if __name__ == "__main__":
    main()
