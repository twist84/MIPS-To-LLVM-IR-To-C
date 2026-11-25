#!/usr/bin/env python3
"""Exercise actual emitted LLVM timing observers for reached and annulled slots."""

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
        ("hi", ctypes.c_uint64), ("lo", ctypes.c_uint64),
        ("hi1", ctypes.c_uint64), ("lo1", ctypes.c_uint64),
        ("sa", ctypes.c_uint32),
        ("pc", ctypes.c_uint32),
        ("exit_reason", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("cop0", ctypes.c_uint32 * 32),
        ("fpr", ctypes.c_uint64 * 32),
        ("vu0", U128 * 32),
        ("fcsr", ctypes.c_uint32), ("fpu_acc", ctypes.c_uint32),
        ("fpu_acc_overflow", ctypes.c_uint32),
    ]


OBSERVER = ctypes.CFUNCTYPE(
    ctypes.c_bool, ctypes.c_void_p,
    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
    ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64
)


def aligned_buffer(size):
    backing = ctypes.create_string_buffer(size + 15)
    ptr = (ctypes.addressof(backing) + 15) & ~15
    return backing, ptr


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_ee_timing_observer_ir_runtime.py fixture")

    fixture = pathlib.Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="ee-observer-jit-") as tmp:
        ir_path = pathlib.Path(tmp) / "observer.ll"
        emitted = subprocess.run(
            [str(fixture), "bltzall", str(ir_path)],
            text=True, capture_output=True, check=True
        )
        layout = next(
            (line.split() for line in emitted.stdout.splitlines()
             if line.startswith("TIMING_OBSERVER_LAYOUT ")), None
        )
        assert layout is not None and len(layout) == 7, emitted.stdout
        size, user_offset, callback_offset, failed_offset, attempt_offset, slot_offset = (
            int(field) for field in layout[1:]
        )
        assert (0 <= user_offset < size and
                0 <= callback_offset < size and
                0 <= failed_offset < size and
                0 <= attempt_offset < size and
                0 <= slot_offset < size), layout

        llvm.initialize_native_target()
        llvm.initialize_native_asmprinter()
        register_ee_diagnostic_stubs()
        target = llvm.Target.from_default_triple()
        machine = target.create_target_machine(opt=0)
        module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
        module.verify()
        module.triple = llvm.get_default_triple()
        module.data_layout = str(machine.target_data)
        engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), machine)
        engine.add_module(module)
        engine.finalize_object()
        address = engine.get_function_address("guest_bltzall_delay_test")
        assert address != 0
        guest_fn = ctypes.CFUNCTYPE(
            ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
        )(address)

        def run(reject):
            _, state_ptr = (state_backing := aligned_buffer(ctypes.sizeof(EEState)))
            state = ctypes.cast(state_ptr, ctypes.POINTER(EEState)).contents
            state.gpr[2].lo = 1  # BLTZALL false: its delay slot is annulled.
            state.gpr[8].lo = 0xABCD
            guest_backing, guest_ptr = aligned_buffer(size)
            _ = (state_backing, guest_backing)
            seen = []
            token = ctypes.c_uint64(0x12345678)

            @OBSERVER
            def observer(user, pc, opcode, kind, service, attempt, slot):
                assert user == ctypes.addressof(token)
                seen.append((pc, opcode, kind, service, attempt, slot))
                return not reject

            ctypes.c_void_p.from_address(guest_ptr + user_offset).value = (
                ctypes.addressof(token)
            )
            ctypes.c_void_p.from_address(guest_ptr + callback_offset).value = (
                ctypes.cast(observer, ctypes.c_void_p).value
            )
            result = guest_fn(ctypes.pointer(state), ctypes.c_void_p(guest_ptr))
            failed = ctypes.c_uint32.from_address(guest_ptr + failed_offset).value
            attempts = ctypes.c_uint64.from_address(guest_ptr + attempt_offset).value
            slots = ctypes.c_uint64.from_address(guest_ptr + slot_offset).value
            assert result == 3 and state.pc == 0x00100008, (result, state.pc)
            assert state.gpr[8].lo == 0xABCD, "annulled delay slot executed"
            assert attempts == 1 and slots == 2, (attempts, slots)
            return seen, failed

        observed, failed = run(False)
        assert failed == 0, failed
        assert observed == [
            (0x00100000, 0x04520004, 0, 0, 1, 1),
            (0x00100004, 0x27e80000, 1, 0, 1, 2),
        ], observed

        observed, failed = run(True)
        assert failed == 1 and observed == [
            (0x00100000, 0x04520004, 0, 0, 1, 1)
        ], (observed, failed)

    print("ee_timing_observer_ir_runtime: PASS")


if __name__ == "__main__":
    main()
