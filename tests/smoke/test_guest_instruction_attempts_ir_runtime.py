#!/usr/bin/env python3
"""Instruction-attempt accounting across native MIPS branches and delay slots.

The counter deliberately measures decoded guest instructions reached, NOT
CPU cycles: it is not suitable as a VBlank clock until stalls/dual-issue
and translated long-loop preemption have been verified separately.
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

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from verify_ir import optimize_guest_ir


def run_case(fixture, temp_path, target, mode, initial_v0, expected, pipeline=False):
    path = temp_path / (mode + ".ll")
    subprocess.run([str(fixture), mode, str(path)], check=True)
    source = path.read_text(encoding="utf-8")
    match = re.search(
        r"%counter_ptr = getelementptr i8, ptr %rt, i64 (\d+)", source
    )
    assert match is not None, "native IR is missing its internal counter"
    offset = int(match.group(1))
    reference_match = re.search(
        r"%ref_counter_ptr = getelementptr i8, ptr %rt, i64 (\d+)", source
    )
    assert reference_match is not None, "missing reference interpreter slot counter"
    reference_offset = int(reference_match.group(1))
    assert reference_offset > offset, "reference counter must preserve existing ABI"

    module = llvm.parse_assembly(source)
    module.verify()
    target_machine = target.create_target_machine(opt=2 if pipeline else 0)
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)
    if pipeline:
        optimize_guest_ir(module, target_machine)
    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()
    symbols = {
        "jr": "guest_jr_delay_test",
        "beq_taken": "guest_beq_taken_delay_test",
        "beq_not_taken": "guest_beq_not_taken_delay_test",
        "bltzall": "guest_bltzall_delay_test",
        "bgezall": "guest_bgezall_delay_test",
        "jal": "guest_jal_delay_test",
    }
    fn_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    address = engine.get_function_address(symbols[mode])
    assert address != 0, f"{mode}: missing host function"
    guest_fn = fn_type(address)

    backing, state = make_aligned_state()
    state.gpr[2].lo = initial_v0 & 0xffffffffffffffff
    if mode == "jr":
        state.gpr[2].lo = 0x00200000
    runtime = ctypes.create_string_buffer(max(offset + 16, reference_offset + 8))
    issued = ctypes.c_uint64.from_buffer(runtime, offset)
    reference_slots = ctypes.c_uint64.from_buffer(runtime, reference_offset)
    issued.value = 9
    reference_slots.value = 7
    result = guest_fn(ctypes.pointer(state), ctypes.cast(runtime, ctypes.c_void_p))
    assert result == 3, f"{mode}: unexpected exit {result}"
    assert issued.value == expected + 9, (
        f"{mode}: expected {expected} attempted instructions, "
        f"observed {issued.value - 9}"
    )
    # The reference interpreter charges branch + reserved delay slot
    # even when branch-likely annuls execution of the actual slot.
    assert reference_slots.value == 9, (
        f"{mode}: expected 2 reference interpreter slots, "
        f"observed {reference_slots.value - 7}"
    )

    # The helper must also tolerate null runtime in existing JIT users.
    if mode == "jr":
        backing2, second_state = make_aligned_state()
        second_state.gpr[2].lo = 0x00200000
        result2 = guest_fn(ctypes.pointer(second_state), None)
        assert result2 == 3, "null-runtime execution changed control flow"

    _ = backing
    return engine, module, target_machine


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_guest_instruction_attempts_ir_runtime.py fixture")
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    register_ee_diagnostic_stubs()

    target = llvm.Target.from_default_triple()
    retained = []
    with tempfile.TemporaryDirectory(prefix="ee-attempts-") as folder:
        temp_path = Path(folder)
        for mode, value, expected in (
            ("jr", 0, 2),                  # JR and executed delay slot
            ("beq_taken", 0, 2),           # branch and delay slot
            ("beq_not_taken", 1, 2),       # same for not taken
            ("bltzall", 5, 1),             # not taken: slot annulled
            ("bltzall", -1, 2),            # taken: slot executed
            ("bgezall", -1, 1),            # not taken: slot annulled
            ("bgezall", 1, 2),             # taken: slot executed
            ("jal", 0, 2),                 # call and executed delay slot
        ):
            for pipeline in (False, True):
                retained.extend(run_case(
                    Path(sys.argv[1]).resolve(), temp_path, target,
                    mode, value, expected, pipeline
                ))
    print("guest_instruction_attempts_ir_runtime: 16 cases passed")


if __name__ == "__main__":
    main()
