#!/usr/bin/env python3
"""Independent native LLVM O0/O3 parity tests for PINTH/PINTEH.

Both instructions interleave eight halfwords into one full 128-bit GPR.
The PCSX2 interpreter uses distinct source halfword selections for each.
Neither instruction changes HI/LO.
"""

import ctypes
import pathlib
import random
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs
from test_mmi_ir_runtime import U128, make_aligned_state, require
from test_mmi_word_ir_runtime import compile_guest

MASK32 = (1 << 32) - 1
MASK64 = (1 << 64) - 1
MASK128 = (1 << 128) - 1
RETURN_PC = 0x00300000
DEST = 0xFEDCBA98765432100123456789ABCDEF

SUFFIXES = (
    "", "_alias_rs", "_alias_rt", "_zero_dest", "_zero_rs",
    "_zero_rt", "_same_source", "_from_prior_cross",
    "_from_prior_rt_cross", "_twice",
)
MODES = [op + suffix for op in ("pinth", "pinteh")
         for suffix in SUFFIXES]


def make_halfwords(words):
    require(len(words) == 8, "expected eight halfwords")
    return sum((word & 0xffff) << (16 * index)
               for index, word in enumerate(words))


def make_source(low_word, high_word, low_unused=0, high_unused=0):
    # Build two 64-bit portions with distinct low/high 32-bit fields.
    lo = (low_word & MASK32) | ((low_unused & MASK32) << 32)
    hi = (high_word & MASK32) | ((high_unused & MASK32) << 32)
    return lo | (hi << 64)


def reference(opcode, source_a, source_b):
    # Independent scalar model of PCSX2's halfword selection.
    a = [(source_a >> (16 * i)) & 0xffff for i in range(8)]
    b = [(source_b >> (16 * i)) & 0xffff for i in range(8)]
    if opcode == "pinth":
        output = [part for i in range(4) for part in (b[i], a[i + 4])]
    elif opcode == "pinteh":
        output = [part for i in range(4) for part in (b[i * 2], a[i * 2])]
    else:
        raise AssertionError("unexpected interleave mode " + opcode)
    return make_halfwords(output)

def run_case(fn, mode, pattern):
    lo0, hi0, lo1, hi1, rs_value, rt_value = pattern
    backing, cpu = make_aligned_state()
    cpu.gpr[4] = U128(rs_value & MASK64, rs_value >> 64)
    cpu.gpr[5] = U128(rt_value & MASK64, rt_value >> 64)
    cpu.gpr[6] = U128(DEST & MASK64, DEST >> 64)
    cpu.gpr[10] = U128(0xDEAD, 0xFEEDFACECAFEBEEF)
    cpu.gpr[31] = U128(RETURN_PC, 0)
    cpu.lo, cpu.hi, cpu.lo1, cpu.hi1 = lo0, hi0, lo1, hi1

    opcode = mode.split("_")[0]
    alias_rs = mode.endswith("_alias_rs")
    alias_rt = mode.endswith("_alias_rt")
    zero_dest = mode.endswith("_zero_dest")
    zero_rs = mode.endswith("_zero_rs")
    zero_rt = mode.endswith("_zero_rt")
    same_source = mode.endswith("_same_source")
    prior_rs = mode.endswith("_from_prior_cross")
    prior_rt = mode.endswith("_from_prior_rt_cross")
    repeat = mode.endswith("_twice")

    final_rs = ((rs_value & (MASK128 ^ MASK64)) | 0x1234
                if prior_rs else rs_value)
    final_rt = ((rt_value & (MASK128 ^ MASK64)) | 0x4321
                if prior_rt else rt_value)
    effective_rs = 0 if zero_rs else final_rs
    effective_rt = (0 if zero_rt else
                    effective_rs if same_source else final_rt)

    result = reference(opcode, effective_rs, effective_rt)
    # In repeated mode both original sources remain constant because
    # the fixture uses destination $6, not the input registers $4/$5.
    if repeat:
        require(result == reference(opcode, effective_rs, effective_rt),
                "repeat reference is not stable")

    expected_rs, expected_rt, expected_dest = final_rs, final_rt, DEST
    if not zero_dest:
        if alias_rs:
            expected_rs = result
        elif alias_rt:
            expected_rt = result
        else:
            expected_dest = result

    status = fn(ctypes.pointer(cpu), None)
    _ = backing
    prefix = mode + ": "
    require(status == 0 and cpu.exit_reason == 0 and cpu.pc == RETURN_PC,
            prefix + "wrong return PC, native exit code or exit reason")
    require(cpu.gpr[0].lo == 0 and cpu.gpr[0].hi == 0,
            prefix + "$zero modified")
    require(cpu.gpr[10].lo == 0x1357 and
            cpu.gpr[10].hi == 0xFEEDFACECAFEBEEF,
            prefix + "preceding ADDIU lost")
    actual_rs = cpu.gpr[4].lo | (cpu.gpr[4].hi << 64)
    actual_rt = cpu.gpr[5].lo | (cpu.gpr[5].hi << 64)
    actual_dest = cpu.gpr[6].lo | (cpu.gpr[6].hi << 64)
    require((actual_rs, actual_rt, actual_dest) ==
            (expected_rs, expected_rt, expected_dest),
            prefix + "GPR result or alias mismatch: " +
            str([hex(actual_rs), hex(actual_rt), hex(actual_dest)]) +
            " expected " +
            str([hex(expected_rs), hex(expected_rt), hex(expected_dest)]))
    require((cpu.lo, cpu.hi, cpu.lo1, cpu.hi1) ==
            (lo0, hi0, lo1, hi1), prefix + "HI/LO banks changed")
    require(cpu.gpr[31].lo == RETURN_PC, prefix + "return address changed")


def main():
    require(len(sys.argv) == 2,
            "usage: test_mmi_interleave_ir_runtime.py fixture")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    register_ee_diagnostic_stubs()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()

    rng = random.Random(0x5900A2B)
    patterns = [
        (0, 0, 0, 0, 0, 0),
        (MASK64, MASK64, MASK64, MASK64, MASK128, MASK128),
        (0, 0, 0, 0,
         make_halfwords([0x1111, 0x2222, 0x3333, 0x4444,
                         0x5555, 0x6666, 0x7777, 0x8888]),
         make_halfwords([0xeeee, 0xdddd, 0xcccc, 0xbbbb,
                         0xaaaa, 0x9999, 0x8888, 0x7777])),
        (0x123456789ABCDEF0, 0x0F0E0D0C0B0A0908,
         0xFFEEDDCCBBAA9988, 0x7766554433221100,
         make_halfwords([0, 1, 2, 3, 4, 5, 6, 7]),
         make_halfwords([7, 6, 5, 4, 3, 2, 1, 0])),
        # Distinct halfwords in the low word of each 64-bit source.
        (0, 0, 0, 0,
         make_source(0, 31), make_source(0x80000000, 0xffffffff)),
        (0, 0, 0, 0,
         make_source(31, 0), make_source(0x00000001, 0x80000000)),
        # Cross-bank upper/lower-word separation and sign-bit patterns.
        (MASK64, 0, MASK64, 0,
         make_source(32, 33), make_source(0x80000000, 0x7fffffff)),
        (MASK64, MASK64, MASK64, MASK64,
         make_source(63, 32), make_source(0xffffffff, 0x80000000)),
        # Different upper/lower words ensure all eight halfwords matter.
        (0xCAFEBABEDEADBEEF, 0x123456789ABCDEF0,
         0x1122334455667788, 0x8877665544332211,
         make_source(1, 31, 0xffffffff, 0x87654321),
         make_source(0xc0000001, 0x80000001, 0x12345678, 0xffffffff)),
        # Different halfword contents in the two 64-bit banks.
        (0, MASK64, MASK64, 0,
         make_source(3, 17), make_source(0x12345678, 0x98765432)),
        (0x7FFFFFFF80000000, 0xAAAAAAAA55555555,
         0x1234567890ABCDEF, 0xFEDCBA9876543210,
         make_source(7, 19), make_source(0x00010000, 0xff000001)),
        (MASK64, 0, 0, MASK64,
         make_source(0xffffffff, 0xffffffe0),
         make_source(0xdeadbeef, 0x80000001)),
    ]
    patterns.extend((
        rng.getrandbits(64), rng.getrandbits(64),
        rng.getrandbits(64), rng.getrandbits(64),
        rng.getrandbits(128), rng.getrandbits(128)
    ) for _ in range(12))
    require(len(patterns) == 24, "interleave pattern count changed")

    executions = 0
    with tempfile.TemporaryDirectory(prefix="ee-mmi-interleave-") as directory:
        directory = pathlib.Path(directory)
        for optimization in (0, 3):
            for mode in MODES:
                fn, engine, module, machine = compile_guest(
                    fixture, directory, mode, optimization, target)
                for pattern in patterns:
                    run_case(fn, mode, pattern)
                    executions += 1
                _ = engine, module, machine

    require(executions == 960, "native execution count changed")
    print("mmi_interleave_ir_runtime: PASS (" + str(executions) +
          " O0/O3 native executions; all halfword lanes, aliases, " +
          "$zero, CFG input writes and preserved HI/LO)")


if __name__ == "__main__":
    main()
