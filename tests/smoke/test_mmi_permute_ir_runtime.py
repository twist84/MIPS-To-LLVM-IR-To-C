#!/usr/bin/env python3
"""Native LLVM parity for R5900 MMI2/MMI3 unary lane permutations.

Reference maps are transcribed independently from PCSX2's MMI interpreter.
Each mode runs at O0 and O3 with distinctive 128-bit halfword/word patterns.
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


MASK64 = (1 << 64) - 1
MASK128 = (1 << 128) - 1
RETURN_PC = 0x00300000
DEST = 0xFEDCBA98765432100123456789ABCDEF
SUFFIXES = (
    "", "_alias_rs", "_alias_rt", "_zero_dest", "_zero_rt",
    "_from_prior_cross", "_from_prior_rt_cross", "_twice",
)
FAMILIES = {
    "mmi2": ("pexeh", "prevh", "pexew", "prot3w"),
    "mmi3": ("pexch", "pexcw"),
}
MAPS = {
    "pexeh": (16, (2, 1, 0, 3, 6, 5, 4, 7)),
    "prevh": (16, (3, 2, 1, 0, 7, 6, 5, 4)),
    "pexew": (32, (2, 1, 0, 3)),
    "prot3w": (32, (1, 2, 0, 3)),
    "pexch": (16, (0, 2, 1, 3, 4, 6, 5, 7)),
    "pexcw": (32, (0, 2, 1, 3)),
}


def make_lanes(values, width):
    require(len(values) * width == 128, "invalid reference vector width")
    return sum((v & ((1 << width) - 1)) << (index * width)
               for index, v in enumerate(values))


def reference(opcode, input_value):
    width, layout = MAPS[opcode]
    mask = (1 << width) - 1
    source = [(input_value >> (i * width)) & mask
              for i in range(128 // width)]
    return make_lanes([source[index] for index in layout], width)


def run_case(fn, mode, pattern):
    lo0, hi0, lo1, hi1, rs_value, rt_value = pattern
    backing, cpu = make_aligned_state()
    cpu.gpr[4] = U128(rs_value & MASK64, rs_value >> 64)
    cpu.gpr[5] = U128(rt_value & MASK64, rt_value >> 64)
    cpu.gpr[6] = U128(DEST & MASK64, DEST >> 64)
    cpu.gpr[10] = U128(0xDEAD, 0xFEEDFACECAFEBEEF)
    cpu.gpr[31] = U128(RETURN_PC, 0)
    cpu.lo, cpu.hi, cpu.lo1, cpu.hi1 = lo0, hi0, lo1, hi1

    op = mode.split("_")[0]
    prior_rs = mode.endswith("_from_prior_cross")
    prior_rt = mode.endswith("_from_prior_rt_cross")
    final_rs = (rs_value & (MASK128 ^ MASK64)) | 0x1234 if prior_rs else rs_value
    final_rt = (rt_value & (MASK128 ^ MASK64)) | 0x4321 if prior_rt else rt_value
    target = 0 if mode.endswith("_zero_rt") else final_rt
    result = reference(op, target)
    expected_rs, expected_rt, expected_dest = final_rs, final_rt, DEST
    if not mode.endswith("_zero_dest"):
        if mode.endswith("_alias_rs"):
            expected_rs = result
        elif mode.endswith("_alias_rt"):
            expected_rt = result
        else:
            expected_dest = result

    status = fn(ctypes.pointer(cpu), None)
    _ = backing
    label = mode + ": "
    require(status == 0 and cpu.exit_reason == 0 and cpu.pc == RETURN_PC,
            label + "wrong exit status or return PC")
    require(cpu.gpr[0].lo == 0 and cpu.gpr[0].hi == 0,
            label + "$zero modified")
    actual_rs = cpu.gpr[4].lo | (cpu.gpr[4].hi << 64)
    actual_rt = cpu.gpr[5].lo | (cpu.gpr[5].hi << 64)
    actual_dest = cpu.gpr[6].lo | (cpu.gpr[6].hi << 64)
    require((actual_rs, actual_rt, actual_dest) ==
            (expected_rs, expected_rt, expected_dest),
            label + "128-bit permutation mismatch: actual " +
            str([hex(actual_rs), hex(actual_rt), hex(actual_dest)]) +
            " expected " +
            str([hex(expected_rs), hex(expected_rt), hex(expected_dest)]))
    require(cpu.gpr[10].lo == 0x1357 and
            cpu.gpr[10].hi == 0xFEEDFACECAFEBEEF,
            label + "preceding ADDIU / unrelated upper GPR changed")
    require(cpu.gpr[31].lo == RETURN_PC,
            label + "return address changed")
    require((cpu.lo, cpu.hi, cpu.lo1, cpu.hi1) == (lo0, hi0, lo1, hi1),
            label + "unexpected HI/LO modification")


def main():
    require(len(sys.argv) == 3 and sys.argv[2] in FAMILIES,
            "usage: test_mmi_permute_ir_runtime.py fixture mmi2|mmi3")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    family = sys.argv[2]
    register_ee_diagnostic_stubs()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    rng = random.Random(0x5900A4D)
    distinct_h = make_lanes(
        [0x0101, 0x1234, 0x2345, 0x3456, 0x4567, 0x5678, 0x6789, 0x789a], 16)
    reversed_h = make_lanes(
        [0xfedc, 0xedcb, 0xdcba, 0xcba9, 0xba98, 0xa987, 0x9876, 0x8765], 16)
    distinct_w = make_lanes([0x12345678, 0x90abcdef,
                             0x76543210, 0xfedcba09], 32)
    patterns = [
        (0, 0, 0, 0, 0, 0),
        (MASK64, MASK64, MASK64, MASK64, MASK128, MASK128),
        (0, 0, 0, 0, distinct_h, reversed_h),
        (0, 0, 0, 0, reversed_h, distinct_h),
        (0, 0, 0, 0, distinct_w, distinct_h),
        (MASK64, 0, MASK64, 0, distinct_h, distinct_w),
        (0, MASK64, 0, MASK64, 0, reversed_h),
        (MASK64, 0, MASK64, 0, MASK128, distinct_h),
    ]
    patterns.extend((
        rng.getrandbits(64), rng.getrandbits(64),
        rng.getrandbits(64), rng.getrandbits(64),
        rng.getrandbits(128), rng.getrandbits(128)
    ) for _ in range(16))
    require(len(patterns) == 24, "pattern count changed")
    modes = [op + suffix for op in FAMILIES[family] for suffix in SUFFIXES]

    count = 0
    with tempfile.TemporaryDirectory(prefix="ee-mmi-permute-") as d:
        folder = pathlib.Path(d)
        for optimization in (0, 3):
            for mode in modes:
                fn, engine, module, machine = compile_guest(
                    fixture, folder, mode, optimization, target)
                for pattern in patterns:
                    run_case(fn, mode, pattern)
                    count += 1
                _ = engine, module, machine

    expected = len(FAMILIES[family]) * len(SUFFIXES) * 24 * 2
    require(count == expected, "native execution count changed")
    print("mmi_permute_" + family + ": PASS (" + str(count) +
          " O0/O3 native cases; all lanes, aliases, $zero, CFG, HI/LO)")


if __name__ == "__main__":
    main()
