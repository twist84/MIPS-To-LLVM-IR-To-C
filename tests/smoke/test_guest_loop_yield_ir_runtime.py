#!/usr/bin/env python3
"""Verify exact guest-state preservation across opt-in native loop yields.

This exercises the actual LLVM generator and MCJIT. Host yields are NOT an
EE cycle source and must never be treated as evidence of a GS VBlank event.
"""
import ctypes
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs
from test_control_flow_ir_runtime import EEState, make_aligned_state


ENTRY_PC = 0x00100000
EE_EXIT_LOOP_BUDGET = 5
EE_EXIT_HOST_YIELD = 6
EE_EXIT_EVENT_YIELD = 8


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    require(len(sys.argv) == 2,
            "usage: test_guest_loop_yield_ir_runtime.py ee_control_flow_ir_fixture")
    fixture = Path(sys.argv[1]).resolve()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    register_ee_diagnostic_stubs()

    with tempfile.TemporaryDirectory(prefix="ee-loop-yield-") as folder:
        ir_path = Path(folder) / "guest-loop-yield.ll"
        proc = subprocess.run(
            [str(fixture), "loop_yield", str(ir_path)],
            capture_output=True, text=True, check=True,
        )
        match = re.search(
            r"LOOP_RUNTIME_OFFSETS ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+) "
            r"([0-9]+) ([0-9]+) ([0-9]+)",
            proc.stdout,
        )
        require(match is not None, "fixture did not report native ABI offsets")
        (limit_offset, remaining_offset, attempt_offset, yield_offset,
         slot_offset, event_yield_offset, event_deadline_offset) = (
             int(value) for value in match.groups())
        require(limit_offset < remaining_offset < attempt_offset < yield_offset < slot_offset,
                "unexpected reordered C++ safepoint runtime ABI")
        require(slot_offset < event_yield_offset < event_deadline_offset,
                "event timing fields were not appended to the runtime ABI")

        ir = ir_path.read_text(encoding="utf-8")
        require("loop_exhausted_" in ir and "select i1" in ir,
                "generated guest loop is missing its resume-capable safepoint")
        require(f"i64 {event_yield_offset}" not in ir,
                "event budget guard still loads the redundant enable flag")
        module = llvm.parse_assembly(ir)
        module.verify()

        target = llvm.Target.from_default_triple()
        machine = target.create_target_machine(opt=0)
        module.triple = llvm.get_default_triple()
        module.data_layout = str(machine.target_data)
        engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), machine)
        engine.add_module(module)
        engine.finalize_object()
        address = engine.get_function_address("guest_loop_yield_test")
        require(address != 0, "the real guest loop was not emitted")

        fn_type = ctypes.CFUNCTYPE(
            ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
        )
        guest_fn = fn_type(address)
        state_backing, state = make_aligned_state()
        state.pc = ENTRY_PC
        state.gpr[8].lo = 41
        state.gpr[8].hi = 0x1122334455667788
        runtime = ctypes.create_string_buffer(max(
            yield_offset + 16, slot_offset + 8, event_deadline_offset + 8))
        runtime_ptr = ctypes.c_void_p(ctypes.addressof(runtime))
        limit = ctypes.c_uint32.from_buffer(runtime, limit_offset)
        remaining = ctypes.c_uint32.from_buffer(runtime, remaining_offset)
        attempts = ctypes.c_uint64.from_buffer(runtime, attempt_offset)
        yield_enabled = ctypes.c_uint32.from_buffer(runtime, yield_offset)
        reference_slots = ctypes.c_uint64.from_buffer(runtime, slot_offset)
        event_yield_enabled = ctypes.c_uint32.from_buffer(runtime, event_yield_offset)
        event_deadline = ctypes.c_uint64.from_buffer(runtime, event_deadline_offset)

        # An event deadline is checked at the committed backward-branch
        # header. Three iterations consume six compatibility slots, then
        # yield at the exact resume PC before executing the next iteration.
        event_yield_enabled.value = 1
        event_deadline.value = 6
        result = guest_fn(ctypes.pointer(state), runtime_ptr)
        require(result == EE_EXIT_EVENT_YIELD and
                state.exit_reason == EE_EXIT_EVENT_YIELD,
                f"expected event yield {EE_EXIT_EVENT_YIELD}, got {result}")
        require(state.pc == ENTRY_PC and state.gpr[8].lo == 44,
                "event yield lost committed guest state or exact resume PC")
        require(attempts.value == 6 and reference_slots.value == 6,
                "event yield did not stop at the requested compatibility slot")
        event_yield_enabled.value = 0
        event_deadline.value = (1 << 64) - 1
        state.pc = ENTRY_PC
        state.exit_reason = 0
        state.gpr[8].lo = 41
        attempts.value = 0
        reference_slots.value = 0

        def run_slice(count, cooperative, expected_result, expected_lo,
                      expected_attempts):
            limit.value = count
            remaining.value = count
            yield_enabled.value = 1 if cooperative else 0
            result = guest_fn(ctypes.pointer(state), runtime_ptr)
            require(result == expected_result and
                    state.exit_reason == expected_result,
                    f"expected exit {expected_result}, got {result}")
            require(state.pc == ENTRY_PC, "safepoint did not retain exact loop PC")
            require(state.gpr[8].lo == expected_lo,
                    f"delay-slot register state lost or repeated: {state.gpr[8].lo}")
            require(state.gpr[8].hi == 0x1122334455667788,
                    "guest GPR128 upper half was not preserved")
            require(attempts.value == expected_attempts,
                    f"counter mismatch: expected {expected_attempts}, got {attempts.value}")
            require(reference_slots.value == expected_attempts,
                    "straight-line and taken branch slot accounting diverged")
            require(remaining.value == 0, "loop budget not exhausted exactly")

        # Three executed BEQ/delay pairs, followed by an exact yield before
        # executing another guest instruction. Re-enter at saved PC.
        run_slice(3, True, EE_EXIT_HOST_YIELD, 44, 6)
        run_slice(2, True, EE_EXIT_HOST_YIELD, 46, 10)

        # Opt-out must retain existing terminal watchdog semantics, not
        # silently treat budget exhaustion as a scheduler clock event.
        run_slice(1, False, EE_EXIT_LOOP_BUDGET, 47, 12)
        require(state.gpr[8].lo - 41 == attempts.value // 2,
                "resumed and continuous loop execution have different guest effects")
        _ = (state_backing, engine, module, machine)

    print("guest_loop_yield_ir_runtime: PASS (resume PC, GPR128, delay slots, "
          "per-instruction attempts, and default terminal watchdog)")


if __name__ == "__main__":
    main()
