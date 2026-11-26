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


def check_alu_count_run(fixture, root, target):
    # MCJIT takes ownership of its target machine. Never share one across
    # engines; disposing either engine can invalidate the other.
    machine = target.create_target_machine(opt=0)
    path = root / "alu.ll"
    emitted = subprocess.run([str(fixture), "alu_count_run", str(path)],
                             text=True, capture_output=True, check=True)
    layout = next(line.split() for line in emitted.stdout.splitlines()
                  if line.startswith("ALU_COUNT_LAYOUT "))
    (size, user, callback, failed, attempts, slots, trace, capacity,
     used, last_pc, entry_size) = map(int, layout[1:])
    ir = path.read_text(encoding="utf-8")
    assert "call i1 @ee_try_count_alu_run(ptr %rt, i64 4)" in ir
    module = llvm.parse_assembly(ir)
    module.verify()
    module.triple = llvm.get_default_triple()
    module.data_layout = str(machine.target_data)
    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), machine)
    engine.add_module(module)
    engine.finalize_object()
    address = engine.get_function_address("guest_alu_count_run")
    assert address
    guest = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.POINTER(EEState),
                             ctypes.c_void_p)(address)

    def run(mode):
        state_backing, state_ptr = aligned_buffer(ctypes.sizeof(EEState))
        state = ctypes.cast(state_ptr, ctypes.POINTER(EEState)).contents
        state.pc = 0x100000
        state.gpr[31].lo = 0x120000
        state.gpr[8].lo = 7
        for reg in (2, 8, 9, 10, 11):
            state.gpr[reg].hi = 0xFEED0000 + reg
        runtime_backing, rt = aligned_buffer(size)
        ctypes.c_uint64.from_address(rt + attempts).value = 10
        ctypes.c_uint64.from_address(rt + slots).value = 20
        events = []
        token = ctypes.c_uint64(42)

        trace_buffer = ctypes.create_string_buffer(entry_size * 16)

        @OBSERVER
        def observe(arg, pc, opcode, kind, service, attempt, slot):
            events.append((arg, pc, kind, attempt, slot,
                           ctypes.c_uint64.from_address(rt + attempts).value,
                           ctypes.c_uint64.from_address(rt + slots).value))
            if mode == "install_trace" and len(events) == 1:
                ctypes.c_void_p.from_address(rt + trace).value = ctypes.addressof(trace_buffer)
                ctypes.c_uint32.from_address(rt + capacity).value = 16
                ctypes.c_uint32.from_address(rt + last_pc).value = 0xffffffff
            return mode != "reject"

        if mode in ("observe", "reject", "both", "install_trace"):
            ctypes.c_void_p.from_address(rt + user).value = ctypes.addressof(token)
            ctypes.c_void_p.from_address(rt + callback).value = ctypes.cast(
                observe, ctypes.c_void_p).value
        if mode in ("trace", "both"):
            ctypes.c_void_p.from_address(rt + trace).value = ctypes.addressof(trace_buffer)
            ctypes.c_uint32.from_address(rt + capacity).value = 16
            ctypes.c_uint32.from_address(rt + last_pc).value = 0xffffffff

        result = guest(ctypes.pointer(state), None if mode == "null" else rt)
        assert result == 0 and state.pc == 0x120000, (result, state.pc)
        for reg, expected in ((8, 78), (9, 72), (10, 80), (2, 78), (11, 81)):
            assert state.gpr[reg].lo == expected, (mode, reg, state.gpr[reg].lo)
            assert state.gpr[reg].hi == 0xFEED0000 + reg
        if mode != "null":
            assert ctypes.c_uint64.from_address(rt + attempts).value == 17
            assert ctypes.c_uint64.from_address(rt + slots).value == 27
        if mode in ("observe", "reject", "both", "install_trace"):
            count = 1 if mode == "reject" else 7
            assert events == [(ctypes.addressof(token), 0x100000 + 4*i, 0,
                               11+i, 21+i, 11+i, 21+i) for i in range(count)], events
        else:
            assert not events
        assert ctypes.c_uint32.from_address(rt + failed).value == (mode == "reject")
        if mode in ("trace", "both", "install_trace"):
            assert ctypes.c_uint32.from_address(rt + used).value == 7
            # m_attempt is the first uint64 field in every trace entry.
            assert [ctypes.c_uint64.from_address(ctypes.addressof(trace_buffer) +
                                                i * entry_size).value
                    for i in range(7)] == list(range(11, 18))
        else:
            assert ctypes.c_uint32.from_address(rt + used).value == 0
        return bytes(state)

    results = [run(mode) for mode in ("fast", "observe", "reject", "trace", "both", "install_trace", "null")]
    assert all(result == results[0] for result in results)


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

        check_alu_count_run(fixture, pathlib.Path(tmp), target)

    print("ee_timing_observer_ir_runtime: PASS")


if __name__ == "__main__":
    main()
