#!/usr/bin/env python3
"""Validate folded scalar IR, dynamic fallback, and independent resume."""
import ctypes
import pathlib
import re
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs
from test_compare_ir_runtime import EEState, make_aligned_state

MASK = (1 << 64) - 1
ENTRY = 0x00120000
RESUME = ENTRY + 22 * 4


def run(fixture):
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    register_ee_diagnostic_stubs()
    with tempfile.TemporaryDirectory(prefix="ee-scalar-specialize-") as temp:
        path = pathlib.Path(temp) / "fixture.ll"
        subprocess.run([fixture, str(path)], check=True)
        source = path.read_text(encoding="utf-8")
        # Folded instructions must not leave arithmetic/comparison SSA,
        # even when the counted run emits separate fast/observed copies.
        for pc in (ENTRY + 8, ENTRY + 12, ENTRY + 16, ENTRY + 20,
                   ENTRY + 24, ENTRY + 32, ENTRY + 36, ENTRY + 40,
                   ENTRY + 26 * 4, ENTRY + 27 * 4, ENTRY + 28 * 4):
            bodies = re.findall(
                rf"(?m)^  ; 0x{pc:x}: [^\n]*\n((?:  [^\n]*\n)*)", source
            )
            assert bodies, f"missing guest instruction {pc:x}"
            # Stop before the next guest comment when the body spans a run.
            for body in bodies:
                body = body.split("  ; ")[0]
                assert not re.search(
                    r" = (?:add|sub|and|or|xor|shl|lshr|ashr|icmp) i(?:32|64) ",
                    body
                ), (hex(pc), body)
        assert "shl i64 " in source and ", 63\n" in source
        for index in (25, 30):
            bodies = re.findall(rf"(?m)^  ; 0x{ENTRY + index * 4:x}: [^\n]*\n((?:  [^\n]*\n)*)", source)
            assert bodies
            for body in bodies:
                body = body.split("  ; ")[0]
                if index == 25:
                    assert not re.search(r" = and i(?:32|64) ", body), body
                else:
                    assert not re.search(r" = (?:trunc|sext) ", body), body
        assert re.search(r"call void @ee_guard_guest_v0_trace\(", source)
        module = llvm.parse_assembly(source)
        module.verify()
        tm = llvm.Target.from_default_triple().create_target_machine(opt=0)
        module.triple = llvm.get_default_triple()
        module.data_layout = str(tm.target_data)
        engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), tm)
        engine.add_module(module)
        engine.finalize_object()
        address = engine.get_function_address("guest_scalar_specialize_test")
        assert address
        fn = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.POINTER(EEState),
                              ctypes.c_void_p)(address)
        counter = int(re.search(
            r"%counter_ptr = getelementptr i8, ptr %rt, i64 (\d+)", source
        ).group(1))
        reference = int(re.search(
            r"%ref_counter_ptr = getelementptr i8, ptr %rt, i64 (\d+)", source
        ).group(1))
        offsets = [int(x) for x in re.findall(
            r"getelementptr i8, ptr %rt, i64 (\d+)", source
        )]
        runtime_size = max(offsets + [counter, reference]) + 64

        for x in (0, 1, 0x1234567880000001, MASK):
            for resume in (False, True):
                backing, state = make_aligned_state()
                state.pc = RESUME if resume else ENTRY
                initial = {}
                for reg in range(1, 32):
                    state.gpr[reg].lo = (0x1234567800000000 | reg)
                    state.gpr[reg].hi = 0xA500000000000000 | reg
                    initial[reg] = state.gpr[reg].lo
                state.gpr[10].lo = x
                initial[10] = x
                if resume:
                    state.gpr[3].lo = 3
                    state.gpr[4].lo = 5
                    initial[3], initial[4] = 3, 5
                    expected = {23: 8, 24: 3 << 32}
                else:
                    word = x & 0xffffffff
                    if word & 0x80000000:
                        word |= 0xffffffff00000000
                    expected = {
                        2: MASK, 3: 0xffffffff80000000, 4: MASK,
                        5: 0x7fffffff, 6: 0xffffffff7fffffff,
                        7: 0, 8: 0, 9: 0, 11: x, 12: 0, 13: 0,
                        14: MASK, 15: word, 16: (x << 63) & MASK,
                        17: 0, 18: x, 19: word, 20: 0, 21: x ^ MASK,
                        22: 0x7fffffff, 23: 0xffffffff7fffffff,
                        24: 0x8000000000000000,
                    }
                expected.update({
                    25: x & 31, 26: (x << (x & 31)) & MASK, 27: 1, 28: 0,
                    29: 31, 30: MASK ^ 0x7fffffff if x & 1 else 0,
                    31: MASK ^ 0x7fffffff if x & 1 else 0,
                })
                runtime = ctypes.create_string_buffer(runtime_size)
                result = fn(ctypes.pointer(state), ctypes.cast(runtime, ctypes.c_void_p))
                assert result == 3, (result, state.pc)
                assert state.pc == ENTRY + 31 * 4, hex(state.pc)
                assert ctypes.c_uint64.from_buffer(runtime, counter).value == (9 if resume else 31)
                assert ctypes.c_uint64.from_buffer(runtime, reference).value == (9 if resume else 31)
                assert state.gpr[0].lo == state.gpr[0].hi == 0
                for reg in range(1, 32):
                    assert state.gpr[reg].lo == expected.get(reg, initial[reg]), (reg, resume, hex(x))
                    assert state.gpr[reg].hi == 0xA500000000000000 | reg
                _ = backing
    print("scalar_specialize_ir_runtime: PASS")


if __name__ == "__main__":
    run(str(pathlib.Path(sys.argv[1]).resolve()))
