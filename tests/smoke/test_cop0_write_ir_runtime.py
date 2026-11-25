#!/usr/bin/env python3
"""Execute decoder-generated COP0 LLVM with the actual C++ MTC0 helper.

This is a functional architectural regression, not an EE timing calibration.
"""
import ctypes
import pathlib
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs


ENTRY_PC = 0x00134000
TIMER_IP = 1 << 15
INTC_IP = 1 << 10


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


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def sign_extend_u32(value):
    return ctypes.c_uint64(ctypes.c_int32(value).value).value


def aligned_state():
    backing = ctypes.create_string_buffer(ctypes.sizeof(EEState) + 15)
    address = (ctypes.addressof(backing) + 15) & ~15
    return backing, ctypes.cast(address, ctypes.POINTER(EEState)).contents


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: test_cop0_write_ir_runtime.py fixture_exe helper_library"
        )

    fixture = pathlib.Path(sys.argv[1]).resolve()
    helper_path = pathlib.Path(sys.argv[2]).resolve()
    helper_library = ctypes.CDLL(str(helper_path))
    helper_address = ctypes.cast(
        helper_library.ee_cop0_write, ctypes.c_void_p
    ).value
    require(helper_address is not None and helper_address != 0,
            "real ee_cop0_write helper was not exported")

    with tempfile.TemporaryDirectory(prefix="ee-cop0-write-") as temp:
        path = pathlib.Path(temp) / "cop0_write.ll"
        subprocess.run([str(fixture), str(path)], check=True)
        ir_text = path.read_text(encoding="utf-8")

        # This checks the emitted calls, not only an interpreter-side shim.
        require(ir_text.count("call void @ee_cop0_write(") == 1 and
                ir_text.count("call void @ee_cop0_clock_write(") == 2,
                "MTC0 Count/Compare use clock-aware helpers; Status uses raw helper")
        for index in (11, 12, 9):
            require((f"ptr %cpu, i32 {index}, i32 " in ir_text or
                     f"ptr %cpu, ptr %rt, i32 {index}, i32 " in ir_text),
                    f"COP0 register {index} did not use the helper ABI")

        llvm.initialize_native_target()
        llvm.initialize_native_asmprinter()
        llvm.add_symbol("ee_cop0_write", helper_address)
        for name in ("ee_cop0_clock_sync", "ee_cop0_clock_write"):
            llvm.add_symbol(name, ctypes.cast(getattr(helper_library, name), ctypes.c_void_p).value)
        register_ee_diagnostic_stubs()

        module = llvm.parse_assembly(ir_text)
        module.verify()
        target = llvm.Target.from_default_triple()
        machine = target.create_target_machine()
        module.triple = llvm.get_default_triple()
        module.data_layout = str(machine.target_data)
        module.verify()

        engine = llvm.create_mcjit_compiler(
            llvm.parse_assembly(""), machine
        )
        engine.add_module(module)
        engine.finalize_object()
        address = engine.get_function_address("guest_cop0_write_test")
        require(address != 0, "guest COP0 LLVM function missing")
        guest_fn = ctypes.CFUNCTYPE(
            ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
        )(address)

        for compare_value, initial_cause in (
            (0x00000000, 0x80000034 | INTC_IP | TIMER_IP),
            (0xFFFF8000, 0x00000124 | INTC_IP | TIMER_IP),
            (0x00000006, 0x00000034 | INTC_IP),
        ):
            backing, state = aligned_state()
            _ = backing
            state.cop0[9] = 0x12345678
            state.cop0[11] = 0x22222222
            state.cop0[12] = 0x10000
            state.cop0[13] = initial_cause
            state.gpr[8].lo = 0xCAFEBABE00000000 | compare_value
            state.gpr[8].hi = 0x1122334455667788
            state.gpr[12].lo = 0xFFFF123400010001
            state.gpr[14].lo = 0xDEADBEEFAABBCCDD
            for register in (9, 10, 13, 15):
                state.gpr[register].hi = 0xABCDEF0000000000 | register

            exit_reason = guest_fn(ctypes.pointer(state), None)
            cleared_cause = initial_cause & ~TIMER_IP
            require(exit_reason == 3 and state.exit_reason == 3,
                    "generated LLVM did not exit through dispatcher")
            require(state.pc == ENTRY_PC + 7 * 4,
                    "generated LLVM returned an unexpected guest PC")
            require(state.cop0[11] == compare_value,
                    "Compare write did not retain the low 32 source bits")
            require(state.cop0[13] == cleared_cause,
                    "Compare write did not clear only IP7")
            require(state.gpr[9].lo == sign_extend_u32(cleared_cause),
                    "subsequent MFC0 Cause observed stale timer IP7")
            require(state.gpr[10].lo == sign_extend_u32(compare_value),
                    "MFC0 Compare did not read updated guest value")
            require(state.cop0[12] == 0x00010001 and
                    state.gpr[13].lo == 0x10001,
                    "non-Compare Status write/read semantics changed")
            require(state.cop0[9] == 0xAABBCCDD and
                    state.gpr[15].lo == sign_extend_u32(0xAABBCCDD),
                    "Count write/read did not preserve existing semantics")
            for register in (9, 10, 13, 15):
                require(
                    state.gpr[register].hi ==
                    (0xABCDEF0000000000 | register),
                    f"MFC0 incorrectly modified upper GPR{register} bits"
                )

    print("cop0_write_ir_runtime: PASS")


if __name__ == "__main__":
    main()
