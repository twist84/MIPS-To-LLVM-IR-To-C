#!/usr/bin/env python3
"""Independent O0/O3 R5900 packed-word multiply-accumulate JIT regression.

64-bit modular arithmetic matches PCSX2's x86 recompiler and the
published hardware vectors. Randomized software-reference cases remain
separate from direct hardware comparisons.
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

MASK32 = (1 << 32) - 1
MASK64 = (1 << 64) - 1
MASK128 = (1 << 128) - 1
RETURN_PC = 0x00300000
DEST = 0xFEDCBA98765432100123456789ABCDEF
MODES = [
    op + suffix for op in ("pmaddw", "pmsubw", "pmadduw")
    for suffix in ("", "_alias_rs", "_alias_rt", "_zero_dest",
                   "_zero_rs", "_zero_rt", "_from_prior_cross", "_twice")
]


def signed32(value):
    value &= MASK32
    return value - (1 << 32) if value & 0x80000000 else value


def saved_word(value):
    return signed32(value) & MASK64


def reference(opcode, a, b, banks, repeat):
    updated = list(banks)
    dest = 0
    for lane in range(2):
        shift = lane * 64
        lhs, rhs = (a >> shift) & MASK32, (b >> shift) & MASK32
        product = (lhs * rhs if opcode == "pmadduw" else
                   signed32(lhs) * signed32(rhs))
        low_index, high_index = lane * 2, lane * 2 + 1
        value = ((updated[high_index] & MASK32) << 32) | (
            updated[low_index] & MASK32)
        for _ in range(2 if repeat else 1):
            value = (value - product if opcode == "pmsubw" else
                     value + product) & MASK64
        updated[low_index] = saved_word(value)
        updated[high_index] = saved_word(value >> 32)
        dest |= value << shift
    return updated, dest


def run_case(fn, mode, pattern, hardware=None):
    lo0, hi0, lo1, hi1, lhs, rhs = pattern
    backing, cpu = make_aligned_state()
    cpu.gpr[4] = U128(lhs & MASK64, lhs >> 64)
    cpu.gpr[5] = U128(rhs & MASK64, rhs >> 64)
    cpu.gpr[6] = U128(DEST & MASK64, DEST >> 64)
    cpu.gpr[10] = U128(0xDEAD, 0xFEEDFACECAFEBEEF)
    cpu.gpr[31] = U128(RETURN_PC, 0)
    cpu.lo, cpu.hi, cpu.lo1, cpu.hi1 = lo0, hi0, lo1, hi1

    opcode = mode.split("_")[0]
    prior = mode.endswith("_from_prior_cross")
    repeat = mode.endswith("_twice")
    alias_rs = mode.endswith("_alias_rs")
    alias_rt = mode.endswith("_alias_rt")
    zero_dest = mode.endswith("_zero_dest")
    zero_rs = mode.endswith("_zero_rs")
    zero_rt = mode.endswith("_zero_rt")
    effective_lhs = (lhs & (MASK128 ^ MASK64)) | 0x1234 if prior else lhs
    banks, dest = reference(opcode, 0 if zero_rs else effective_lhs,
                            0 if zero_rt else rhs,
                            (lo0, hi0, lo1, hi1), repeat)
    if hardware is not None:
        dest, banks = hardware
    expected_rs, expected_rt, expected_rd = effective_lhs, rhs, DEST
    if not zero_dest:
        if alias_rs:
            expected_rs = dest
        elif alias_rt:
            expected_rt = dest
        else:
            expected_rd = dest

    status = fn(ctypes.pointer(cpu), None)
    _ = backing
    prefix = mode + ": "
    require(status == 0 and cpu.exit_reason == 0 and cpu.pc == RETURN_PC,
            prefix + "wrong exit/PC")
    require(cpu.gpr[0].lo == 0 and cpu.gpr[0].hi == 0,
            prefix + "GPR $zero clobbered")
    require(cpu.gpr[10].lo == 0x1357 and
            cpu.gpr[10].hi == 0xFEEDFACECAFEBEEF,
            prefix + "prior ADDIU lost")
    actual_rs = cpu.gpr[4].lo | (cpu.gpr[4].hi << 64)
    actual_rt = cpu.gpr[5].lo | (cpu.gpr[5].hi << 64)
    actual_rd = cpu.gpr[6].lo | (cpu.gpr[6].hi << 64)
    require((actual_rs, actual_rt, actual_rd) ==
            (expected_rs, expected_rt, expected_rd),
            prefix + "full-width destination / alias mismatch")
    actual_banks = (cpu.lo, cpu.hi, cpu.lo1, cpu.hi1)
    require(list(actual_banks) == banks,
            prefix + "accumulators " + str([hex(x) for x in actual_banks]) +
            " expected " + str([hex(x) for x in banks]))
    require(cpu.gpr[31].lo == RETURN_PC,
            prefix + "return address modified")


def main():
    require(len(sys.argv) == 2, "usage: test_mmi_mac_ir_runtime.py fixture")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    register_ee_diagnostic_stubs()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    rng = random.Random(0x5900AC)
    patterns = [
        (0, 0, 0, 0, 0, 0),
        (MASK64, MASK64, MASK64, MASK64, MASK128, MASK128),
        (MASK32, 0, MASK32, 0, 1 | (1 << 64), 1 | (1 << 64)),
        (0, 1, 0, 1, 1 | (1 << 64), 1 | (1 << 64)),
        (0x80000000, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000,
         0x80000000800000008000000080000000,
         0xFFFFFFFF7FFFFFFF7FFFFFFF80000000),
        (0xFFFFFFFF00000000, 0x12345678DEADBEEF,
         0xABCDEF0180000000, 0xFFFFFFFF7FFFFFFF,
         0x800000007FFFFFFF0000000080000000,
         0x7FFFFFFF80000000FFFFFFFF7FFFFFFF),
    ]
    patterns.extend((
        rng.getrandbits(64), rng.getrandbits(64),
        rng.getrandbits(64), rng.getrandbits(64),
        rng.getrandbits(128), rng.getrandbits(128)
    ) for _ in range(14))
    count = 0
    with tempfile.TemporaryDirectory(prefix="ee-mmi-mac-") as directory:
        directory = pathlib.Path(directory)
        for opt in (0, 3):
            for mode in MODES:
                fn, engine, module, machine = compile_guest(
                    fixture, directory, mode, opt, target)
                for pattern in patterns:
                    run_case(fn, mode, pattern)
                    count += 1
                count += run_hardware_vectors(fn, mode, run_case)
                _ = engine, module, machine
    print("mmi_mac_ir_runtime: PASS (" + str(count) +
          " O0/O3 native executions, signed/unsigned, carry/borrow/wrap, " +
          "two HI/LO lanes, aliasing, $zero, cross-block)")


if __name__ == "__main__":
    main()
