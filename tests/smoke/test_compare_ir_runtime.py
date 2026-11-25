#!/usr/bin/env python3
"""JIT test for signed/unsigned 64-bit EE compare operations."""

import ctypes
import pathlib
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs


class U128(ctypes.Structure):
    _fields_ = [("lo", ctypes.c_uint64), ("hi", ctypes.c_uint64)]


class EEState(ctypes.Structure):
    _fields_ = [
        ("gpr", U128 * 32),
        ("hi", ctypes.c_uint64),
        ("lo", ctypes.c_uint64),
        ("hi1", ctypes.c_uint64),
        ("lo1", ctypes.c_uint64),
        ("sa", ctypes.c_uint32),
        ("pc", ctypes.c_uint32),
        ("exit_reason", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("cop0", ctypes.c_uint32 * 32),
        ("fpr", ctypes.c_uint64 * 32),
        ("vu0", U128 * 32),
        ("fcsr", ctypes.c_uint32),
        ("fpu_acc", ctypes.c_uint32),
        ("fpu_acc_overflow", ctypes.c_uint32),
    ]


def make_aligned_state():
    backing = ctypes.create_string_buffer(ctypes.sizeof(EEState) + 15)
    address = ctypes.addressof(backing)
    aligned_address = (address + 15) & ~15
    state = ctypes.cast(aligned_address, ctypes.POINTER(EEState)).contents
    return backing, state


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_compare_ir_runtime.py fixture_executable")

    fixture_exe = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="ee-compare-") as temp:
        ir_path = pathlib.Path(temp) / "scalar_compare.ll"
        subprocess.run([str(fixture_exe), str(ir_path)], check=True)

        module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
        module.verify()
        module.triple = llvm.get_default_triple()

        llvm.initialize_native_target()
        llvm.initialize_native_asmprinter()
        register_ee_diagnostic_stubs()
        target = llvm.Target.from_default_triple()
        target_machine = target.create_target_machine()
        module.data_layout = str(target_machine.target_data)

        engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
        engine.add_module(module)
        engine.finalize_object()

        address = engine.get_function_address("guest_scalar_compare_test")
        require(address != 0, "JIT did not expose scalar compare function")

        guest_fn_type = ctypes.CFUNCTYPE(
            ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
        )
        guest_fn = guest_fn_type(address)

        backing, state = make_aligned_state()
        _ = backing

        mask64 = 0xFFFFFFFFFFFFFFFF
        state.gpr[4].lo = mask64 - 2       # -3 signed, very large unsigned
        state.gpr[5].lo = 1
        state.gpr[6].lo = 0
        state.gpr[7].lo = mask64
        state.gpr[10].lo = mask64         # -1 signed
        state.gpr[11].lo = 0
        state.gpr[12].lo = mask64         # UINT64_MAX destination/source
        state.gpr[13].lo = 1

        # Every scalar compare must leave the upper half of its destination.
        for reg in (2, 3, 6, 7, 8, 9, 10, 12):
            state.gpr[reg].hi = 0xA500000000000000 | reg

        result = guest_fn(ctypes.pointer(state), None)

        require(result == 3, f"expected dispatcher exit 3, got {result}")
        require(state.exit_reason == 3,
                f"expected EE_EXIT_DISPATCH, got {state.exit_reason}")
        require(state.pc == 0x00120024,
                f"wrong sequential dispatch PC 0x{state.pc:08x}")
        expected = {
            2: 1,   # -3 < -2 (signed)
            3: 1,   # unsigned(-3) < unsigned(-1), immediate is sign-extended
            6: 1,   # 0 < 1, with rs=rt destination alias
            7: 0,   # UINT64_MAX < UINT64_MAX is false
            8: 1,   # signed -3 < +1
            9: 0,   # unsigned UINT64_MAX-2 < 1 is false
            10: 1,  # signed -1 < 0, rd=rs
            12: 1,  # unsigned 1 < UINT64_MAX, rd=rt
            0: 0,   # zero register remains zero
        }
        for reg, value in expected.items():
            require(state.gpr[reg].lo == value,
                    f"GPR[{reg}] expected {value}, got 0x{state.gpr[reg].lo:x}")

        for reg in (2, 3, 6, 7, 8, 9, 10, 12):
            expected_hi = 0xA500000000000000 | reg
            require(state.gpr[reg].hi == expected_hi,
                    f"GPR[{reg}] upper half changed: 0x{state.gpr[reg].hi:x}")

    print("compare_ir_runtime: PASS")


if __name__ == "__main__":
    main()
