#!/usr/bin/env python3
"""Reference-driven O0/O3 native LLVM tests of R5900 MMI2 PDIVBW.

Independent 4x signed word / single signed halfword reference, including
PCSX2's deterministic division-by-zero and INT_MIN/-1 behaviour. Hardware
divide-by-zero outputs are not guaranteed by the EE manual.
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

MASK16 = (1 << 16) - 1
MASK32 = (1 << 32) - 1
MASK64 = (1 << 64) - 1
MASK128 = (1 << 128) - 1
RETURN_PC = 0x00300000
DEST = 0xFEDCBA98765432100123456789ABCDEF
MODES = ("pdivbw", "pdivbw_zero_rs", "pdivbw_zero_rt",
         "pdivbw_same_source", "pdivbw_from_prior_cross", "pdivbw_twice")


def signed32(value):
    value &= MASK32
    return value - 0x100000000 if value & 0x80000000 else value


def signed16(value):
    value &= MASK16
    return value - 0x10000 if value & 0x8000 else value


def pack_words(words):
    result = 0
    for lane, word in enumerate(words):
        result |= (word & MASK32) << (32 * lane)
    return result


def expected_banks(source_a, source_b):
    """HI=rem, LO=quotient for each of four independently divided rs words."""
    divisor = signed16(source_b)
    quotients = []
    remainders = []
    for lane in range(4):
        numerator = signed32(source_a >> (lane * 32))
        if divisor == 0:
            quotient = 1 if numerator < 0 else -1
            remainder = numerator
        else:
            quotient = abs(numerator) // abs(divisor)
            if (numerator < 0) != (divisor < 0):
                quotient = -quotient
            remainder = numerator - quotient * divisor
        quotients.append(quotient & MASK32)
        remainders.append(remainder & MASK32)

    lo = pack_words(quotients)
    hi = pack_words(remainders)
    return [lo & MASK64, hi & MASK64, lo >> 64, hi >> 64]


def run_case(fn, mode, pattern):
    old_lo0, old_hi0, old_lo1, old_hi1, lhs, rhs = pattern
    backing, cpu = make_aligned_state()
    cpu.gpr[4] = U128(lhs & MASK64, lhs >> 64)
    cpu.gpr[5] = U128(rhs & MASK64, rhs >> 64)
    cpu.gpr[6] = U128(DEST & MASK64, DEST >> 64)
    cpu.gpr[10] = U128(0xDEAD, 0xFEEDFACECAFEBEEF)
    cpu.gpr[31] = U128(RETURN_PC, 0)
    cpu.lo, cpu.hi, cpu.lo1, cpu.hi1 = (
        old_lo0, old_hi0, old_lo1, old_hi1)

    zero_rs = mode == "pdivbw_zero_rs"
    zero_rt = mode == "pdivbw_zero_rt"
    same_source = mode == "pdivbw_same_source"
    prior_cross = mode == "pdivbw_from_prior_cross"
    prior_a = ((lhs & (MASK128 ^ MASK64)) | 0x1234) if prior_cross else lhs
    source_a = 0 if zero_rs else prior_a
    source_b = 0 if zero_rt else (source_a if same_source else rhs)
    wanted_banks = expected_banks(source_a, source_b)

    exit_code = fn(ctypes.pointer(cpu), None)
    _ = backing
    prefix = mode + ": "
    require(exit_code == 0 and cpu.exit_reason == 0 and cpu.pc == RETURN_PC,
            prefix + "bad native exit code, reason or return PC")
    require(cpu.gpr[0].lo == 0 and cpu.gpr[0].hi == 0,
            prefix + "GPR $zero changed")
    require(cpu.gpr[10].lo == 0x1357 and
            cpu.gpr[10].hi == 0xFEEDFACECAFEBEEF,
            prefix + "previous ADDIU lost")

    actual_a = cpu.gpr[4].lo | (cpu.gpr[4].hi << 64)
    actual_b = cpu.gpr[5].lo | (cpu.gpr[5].hi << 64)
    actual_rd = cpu.gpr[6].lo | (cpu.gpr[6].hi << 64)
    require((actual_a, actual_b, actual_rd) == (prior_a, rhs, DEST),
            prefix + "PDIVBW corrupted a GPR (no rd result)")

    actual_banks = [cpu.lo, cpu.hi, cpu.lo1, cpu.hi1]
    require(actual_banks == wanted_banks,
            prefix + "HI/LO 128-bit mismatch, actual " +
            str([hex(v) for v in actual_banks]) +
            " expected " + str([hex(v) for v in wanted_banks]))
    require(cpu.gpr[31].lo == RETURN_PC,
            prefix + "return address overwritten")


def main():
    require(len(sys.argv) == 2,
            "usage: test_mmi_divbw_ir_runtime.py ee_mmi_word_ir_fixture")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    register_ee_diagnostic_stubs()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    rng = random.Random(0x5900D1B)

    patterns = [
        (0, 0, 0, 0, 0, 0),
        (MASK64, MASK64, MASK64, MASK64, MASK128, MASK128),
        # Positive/negative/zero and INT_MIN in distinct word lanes.
        (0, 1, 2, 3, pack_words([0x80000000, 0x7fffffff, 0xffffffff, 0]),
         0xffff),
        # Zero divisor with nonzero high bits and arbitrary initial banks.
        (0x123456789abcdef0, 0xfedcba9876543210,
         MASK64, 0x55555555aaaaaaaa,
         pack_words([0x80000000, 0xffffffff, 0, 0x7fffffff]),
         0xfeedfacefeed0000),
        # Signed -32768 divisor, including exact INT_MIN/-32768.
        (MASK64, 0, MASK64, 0,
         pack_words([0x80000000, 0xffffffff, 0x7fffffff, 0]),
         0xfedcba9876548000),
        # Three asymmetric signs: signed remainder follows the numerator.
        (0x1111111111111111, 0x2222222222222222,
         0x3333333333333333, 0x4444444444444444,
         pack_words([0xfffffffb, 5, 0xfffffffa, 6]),
         0xaaaa555555550003),
        (0, 0, 0, 0,
         pack_words([0xfffffffb, 5, 0xfffffffa, 6]),
         0x000000000000fffd),
        # Positive 0x7fff and nonzero other halfwords of rt.
        (MASK64, MASK64, MASK64, MASK64,
         pack_words([0x7fffffff, 0x80000000, 0x12345678, 0x87654321]),
         0x8000ffff12347fff),
    ]
    patterns.extend((
        rng.getrandbits(64), rng.getrandbits(64),
        rng.getrandbits(64), rng.getrandbits(64),
        rng.getrandbits(128), rng.getrandbits(128)
    ) for _ in range(16))
    executions = 0
    with tempfile.TemporaryDirectory(prefix="ee-mmi-pdivbw-") as directory:
        directory = pathlib.Path(directory)
        for optimization in (0, 3):
            for mode in MODES:
                fn, engine, module, machine = compile_guest(
                    fixture, directory, mode, optimization, target)
                for pattern in patterns:
                    run_case(fn, mode, pattern)
                    executions += 1
                _ = engine, module, machine

    print("mmi_divbw_ir_runtime: PASS (" + str(executions) +
          " O0/O3 native executions; 4 signed quotients + 4 remainders; " +
          "INT_MIN/-1, divide-by-zero, sign rules, rt low16 only, GPR/CFG)")


if __name__ == "__main__":
    main()
