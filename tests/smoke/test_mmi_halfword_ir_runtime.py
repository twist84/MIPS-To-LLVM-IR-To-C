#!/usr/bin/env python3
"""Reference-driven O0/O3 native JIT tests for R5900 MMI2 halfword operations.

The model follows the public PCSX2 interpreter and x86 recompiler.
Direct PS2 vectors also verify the observed PHMADH/PHMSBH odd accumulator
words; their behavior outside the measured cases remains undocumented.
"""

import ctypes
import pathlib
import random
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs
from mmi_hardware_vectors import run_hardware_vectors
from test_mmi_ir_runtime import U128, make_aligned_state, require
from test_mmi_word_ir_runtime import compile_guest

MASK16 = (1 << 16) - 1
MASK32 = (1 << 32) - 1
MASK64 = (1 << 64) - 1
MASK128 = (1 << 128) - 1
RETURN_PC = 0x00300000
DEST = 0xFEDCBA98765432100123456789ABCDEF

OPCODES = ("pmaddh", "pmsubh", "phmadh", "phmsbh", "pmulth")
SUFFIXES = ("", "_alias_rs", "_alias_rt", "_zero_dest",
            "_zero_rs", "_zero_rt", "_from_prior_cross", "_twice")
MODES = [opcode + suffix for opcode in OPCODES for suffix in SUFFIXES]


def signed16(value):
    value &= MASK16
    return value - 65536 if value >= 32768 else value


def reference_step(opcode, source_a, source_b, before):
    """Compute each 32-bit accumulator word, then pack even results into rd."""
    product = [
        signed16(source_a >> (lane * 16)) *
        signed16(source_b >> (lane * 16))
        for lane in range(8)
    ]
    after = []
    destination = 0
    for bank in range(4):
        even, odd = product[bank * 2:bank * 2 + 2]
        previous = before[bank]
        low = previous & MASK32
        high = (previous >> 32) & MASK32

        if opcode == "pmaddh":
            low, high = low + even, high + odd
        elif opcode == "pmsubh":
            low, high = low - even, high - odd
        elif opcode == "phmadh":
            low, high = even + odd, odd
        elif opcode == "phmsbh":
            low, high = odd - even, ~odd
        elif opcode == "pmulth":
            low, high = even, odd
        else:
            raise AssertionError("unexpected halfword opcode")

        low &= MASK32
        high &= MASK32
        after.append(low | (high << 32))
        destination |= low << (bank * 32)
    return after, destination


def test_case(fn, mode, pattern, hardware=None):
    lo0, hi0, lo1, hi1, lhs, rhs = pattern
    backing, cpu = make_aligned_state()
    cpu.gpr[4] = U128(lhs & MASK64, lhs >> 64)
    cpu.gpr[5] = U128(rhs & MASK64, rhs >> 64)
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
    prior_cross = mode.endswith("_from_prior_cross")
    repeat = mode.endswith("_twice")

    effective_lhs = (lhs & (MASK128 ^ MASK64)) | 0x1234 if prior_cross else lhs
    source_a = 0 if zero_rs else effective_lhs
    source_b = 0 if zero_rt else rhs
    expected_banks = [lo0, hi0, lo1, hi1]
    expected_result = DEST
    for _ in range(2 if repeat else 1):
        expected_banks, expected_result = reference_step(
            opcode, source_a, source_b, expected_banks)

    if hardware is not None:
        expected_result, expected_banks = hardware
    expected_rs, expected_rt, expected_rd = effective_lhs, rhs, DEST
    if not zero_dest:
        if alias_rs:
            expected_rs = expected_result
        elif alias_rt:
            expected_rt = expected_result
        else:
            expected_rd = expected_result

    exit_code = fn(ctypes.pointer(cpu), None)
    _ = backing
    prefix = mode + ": "
    require(exit_code == 0 and cpu.exit_reason == 0 and cpu.pc == RETURN_PC,
            prefix + "unexpected native exit or return PC")
    require(cpu.gpr[0].lo == 0 and cpu.gpr[0].hi == 0,
            prefix + "$zero was modified")
    require(cpu.gpr[10].lo == 0x1357 and
            cpu.gpr[10].hi == 0xFEEDFACECAFEBEEF,
            prefix + "preceding ADDIU was dropped")
    actual_rs = cpu.gpr[4].lo | (cpu.gpr[4].hi << 64)
    actual_rt = cpu.gpr[5].lo | (cpu.gpr[5].hi << 64)
    actual_rd = cpu.gpr[6].lo | (cpu.gpr[6].hi << 64)
    require((actual_rs, actual_rt, actual_rd) ==
            (expected_rs, expected_rt, expected_rd),
            prefix + "incorrect 128-bit destination or alias write " +
            str([hex(actual_rs), hex(actual_rt), hex(actual_rd)]) +
            " expected " +
            str([hex(expected_rs), hex(expected_rt), hex(expected_rd)]))
    actual_banks = [cpu.lo, cpu.hi, cpu.lo1, cpu.hi1]
    require(actual_banks == expected_banks,
            prefix + "full HI/LO word state mismatch " +
            str([hex(x) for x in actual_banks]) +
            " expected " + str([hex(x) for x in expected_banks]))
    require(cpu.gpr[31].lo == RETURN_PC,
            prefix + "guest return-address register changed")


def main():
    require(len(sys.argv) == 2,
            "usage: test_mmi_halfword_ir_runtime.py fixture")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    register_ee_diagnostic_stubs()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()

    rng = random.Random(0x5900A1F)
    patterns = [
        (0, 0, 0, 0, 0, 0),
        (MASK64, MASK64, MASK64, MASK64, MASK128, MASK128),
        (MASK64, 0, MASK64, 0, 0, 0),
        # Four pairs of (-32768)*(-32768) wrap PHMADH even words.
        (0x800000007FFFFFFF, 0xFFFFFFFF00000000,
         0x7FFFFFFF80000000, 0x00000000FFFFFFFF,
         0x80008000800080008000800080008000,
         0x80008000800080008000800080008000),
        # Different signed signs in every halfword and every bank.
        (0x0000000180000000, 0x7FFFFFFF00000001,
         0xFFFFFFFFFFFFFFFF, 0xFEDCBA9876543210,
         0x0001FFFF7FFF8000FFFF00017FFF8000,
         0xFFFF000180007FFF80007FFF0001FFFF),
        # Independent odd-word carry, borrow and saturation-boundary cases.
        (0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF,
         0x8000000000000001, 0x7FFFFFFF80000000,
         0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF,
         0x7FFF7FFF7FFF7FFF7FFF7FFF7FFF7FFF),
    ]
    patterns.extend((
        rng.getrandbits(64), rng.getrandbits(64),
        rng.getrandbits(64), rng.getrandbits(64),
        rng.getrandbits(128), rng.getrandbits(128)
    ) for _ in range(14))

    executions = 0
    with tempfile.TemporaryDirectory(prefix="ee-mmi-halfword-") as directory:
        directory = pathlib.Path(directory)
        for opt_level in (0, 3):
            for mode in MODES:
                fn, engine, module, machine = compile_guest(
                    fixture, directory, mode, opt_level, target)
                for pattern in patterns:
                    test_case(fn, mode, pattern)
                    executions += 1
                executions += run_hardware_vectors(fn, mode, test_case)
                _ = engine, module, machine

    print("mmi_halfword_ir_runtime: PASS (" + str(executions) +
          " native executions O0/O3; signed 16x16 products, all eight " +
          "32-bit accumulator words, rd aliases, zero, cross-block and repeat)")


if __name__ == "__main__":
    main()
