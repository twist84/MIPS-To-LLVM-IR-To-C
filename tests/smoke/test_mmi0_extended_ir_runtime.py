#!/usr/bin/env python3
"""Independent packed MMI0, PLZCW and primary-shift JIT regressions."""

import ctypes
import pathlib
import random
import re
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs
from test_mmi_ir_runtime import EEState, U128, make_aligned_state, require


MASK64 = (1 << 64) - 1
MASK128 = (1 << 128) - 1
RETURN_PC = 0x00300000
DEST_LO = 0x0123456789ABCDEF
DEST_HI = 0xFEDCBA9876543210

PACKED = [
    "pcgtw", "pcgth", "pcgtb",
    "paddsw", "psubsw", "pextlw", "ppacw",
    "paddsh", "psubsh", "pextlh", "ppach",
    "paddsb", "psubsb", "pextlb", "ppacb",
    "pext5", "ppac5", "plzcw",
]
SHIFTS = [
    f"{operation}_s{amount}"
    for operation in ("psllh", "psrlh", "psrah", "psllw", "psrlw", "psraw")
    for amount in (0, 1, 15 if operation.endswith("h") else 31)
]
MODES = PACKED + SHIFTS
ALIASES = ("", "_alias_rs", "_alias_rt", "_zero")


def lanes(value, width):
    mask = (1 << width) - 1
    return [(value >> (i * width)) & mask for i in range(128 // width)]


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def flatten(values, width):
    result = 0
    mask = (1 << width) - 1
    for index, value in enumerate(values):
        result |= (value & mask) << (width * index)
    return result & MASK128


def reference(mode, source_a, source_b, original_dest):
    shift_match = re.fullmatch(r"(psllh|psrlh|psrah|psllw|psrlw|psraw)_s(\d+)", mode)
    if shift_match:
        name, raw_shift = shift_match.groups()
        width = 16 if name.endswith("h") else 32
        shift = int(raw_shift) & (width - 1)
        initial = lanes(source_b, width)
        values = []
        for value in initial:
            if name.startswith("psll"):
                values.append(value << shift)
            elif name.startswith("psrl"):
                values.append(value >> shift)
            else:
                values.append(signed(value, width) >> shift)
        return flatten(values, width)

    if mode == "plzcw":
        source = lanes(source_a, 32)
        counts = [
            31 - (value ^ (0xFFFFFFFF if value & 0x80000000 else 0)).bit_length()
            for value in source[:2]
        ]
        low64 = counts[0] | (counts[1] << 32)
        return (original_dest & (MASK128 ^ MASK64)) | low64

    if mode.endswith("5"):
        words = lanes(source_b, 32)
        values = []
        for word in words:
            if mode == "pext5":
                value = (((word & 0x1F) << 3) |
                         ((word & 0x3E0) << 6) |
                         ((word & 0x7C00) << 9) |
                         ((word & 0x8000) << 16))
            else:
                value = (((word >> 3) & 0x1F) |
                         ((word >> 6) & 0x3E0) |
                         ((word >> 9) & 0x7C00) |
                         ((word >> 16) & 0x8000))
            values.append(value)
        return flatten(values, 32)

    width = 32 if mode.endswith("w") else 16 if mode.endswith("h") else 8
    a = lanes(source_a, width)
    b = lanes(source_b, width)
    if mode.startswith("pcgt"):
        return flatten(
            [(1 << width) - 1 if signed(x, width) > signed(y, width) else 0
             for x, y in zip(a, b)], width,
        )
    if mode.startswith("pext"):
        count = len(a) // 2
        return flatten(
            [element for i in range(count) for element in (b[i], a[i])],
            width,
        )
    if mode.startswith("ppac"):
        return flatten(
            b[::2] + a[::2], width,
        )
    if mode.startswith("padds") or mode.startswith("psubs"):
        lower = -(1 << (width - 1))
        upper = (1 << (width - 1)) - 1
        addition = mode.startswith("padd")
        packed = []
        for x, y in zip(a, b):
            value = signed(x, width) + (
                signed(y, width) if addition else -signed(y, width)
            )
            packed.append(max(lower, min(upper, value)))
        return flatten(packed, width)
    raise AssertionError("missing independent semantics for " + mode)


def compile_guest(fixture, directory, mode, optimization, target):
    # MCJIT takes native ownership of its TargetMachine; never reuse one
    # across engines that may be finalized/destroyed between test cases.
    target_machine = target.create_target_machine(opt=0)
    name = mode + "_o" + str(optimization)
    ir_file = directory / (name + ".ll")
    subprocess.run(
        [str(fixture), str(ir_file), mode, str(optimization)], check=True
    )
    module = llvm.parse_assembly(ir_file.read_text(encoding="utf-8"))
    module.verify()
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)
    module.verify()
    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()
    address = engine.get_function_address("guest_mmi0_extended_test")
    require(address != 0, name + ": no generated JIT entry")
    fn = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )(address)
    return fn, engine, module, target_machine


def run_case(fn, full_mode, source_a, source_b, rd):
    backing, cpu = make_aligned_state()
    cpu.gpr[4] = U128(source_a & MASK64, source_a >> 64)
    cpu.gpr[5] = U128(source_b & MASK64, source_b >> 64)
    cpu.gpr[6] = U128(DEST_LO, DEST_HI)
    cpu.gpr[10] = U128(0xDEAD, 0x0123012301230123)
    cpu.gpr[31] = U128(RETURN_PC, 0)
    cpu.hi, cpu.lo = 0x765432109ABCDEF0, 0xFEDCBA9876543210
    cpu.hi1, cpu.lo1 = 0xAABBCCDDEEFF0011, 0x1100FFEEDDCCBBAA

    name = full_mode
    for suffix in ALIASES[1:]:
        if name.endswith(suffix):
            name = name[:-len(suffix)]
            break

    original = [0, 0, 0, 0, 0, 0, DEST_LO | (DEST_HI << 64)]
    original[4], original[5] = source_a, source_b
    expected = reference(name, source_a, source_b, original[rd]) if rd else 0

    result = fn(ctypes.pointer(cpu), None)
    _ = backing

    prefix = full_mode + ": "
    require(result == 0 and cpu.exit_reason == 0 and cpu.pc == RETURN_PC,
            prefix + f"unexpected return/PC {result}/{cpu.pc:08x}")
    require(cpu.gpr[10].lo == 0x1234 and
            cpu.gpr[10].hi == 0x0123012301230123,
            prefix + "preceding ADDIU disappeared")
    actual = cpu.gpr[rd].lo | (cpu.gpr[rd].hi << 64)
    require(actual == expected,
            prefix + f"got {actual:032x}, expected {expected:032x}")
    require(cpu.gpr[0].lo == 0 and cpu.gpr[0].hi == 0,
            prefix + "zero GPR must remain fixed")
    if rd != 4:
        require((cpu.gpr[4].lo | cpu.gpr[4].hi << 64) == source_a,
                prefix + "unrelated rs changed")
    if rd != 5:
        require((cpu.gpr[5].lo | cpu.gpr[5].hi << 64) == source_b,
                prefix + "unrelated rt changed")
    if rd != 6:
        require((cpu.gpr[6].lo | cpu.gpr[6].hi << 64) ==
                (DEST_LO | DEST_HI << 64),
                prefix + "unrelated destination changed")
    require(cpu.hi == 0x765432109ABCDEF0 and
            cpu.lo == 0xFEDCBA9876543210 and
            cpu.hi1 == 0xAABBCCDDEEFF0011 and
            cpu.lo1 == 0x1100FFEEDDCCBBAA,
            prefix + "packed operation modified HI/LO")


def main():
    require(len(sys.argv) == 2,
            "usage: test_mmi0_extended_ir_runtime.py fixture")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    register_ee_diagnostic_stubs()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    rng = random.Random(0x5900)
    patterns = [
        (0, 0),
        (MASK128, MASK128),
        (0x80007FFF0001FFFF800000007FFFFFFF,
         0x00017FFF80000000FFFFFFFF00000001),
        (0x0123456789ABCDEFFEDCBA9876543210,
         0x102030405060708090A0B0C0D0E0F000),
        (0x80808080808080808080808080808080,
         0x7F7F7F7F7F7F7F7F7F7F7F7F7F7F7F),
    ]
    patterns += [(rng.getrandbits(128), rng.getrandbits(128))
                 for _ in range(5)]
    executions = 0
    with tempfile.TemporaryDirectory(prefix="ee-mmi0-exact-") as tmp:
        for opt_level in (0, 3):
            for base in MODES:
                for suffix, destination in (
                    ("", 6), ("_alias_rs", 4),
                    ("_alias_rt", 5), ("_zero", 0)
                ):
                    full_mode = base + suffix
                    print(f"MMI0 case: O{opt_level} {full_mode}", flush=True)
                    fn, engine, module, target_machine = compile_guest(
                        fixture, pathlib.Path(tmp), full_mode,
                        opt_level, target,
                    )
                    for a, b in patterns:
                        run_case(fn, full_mode, a, b, destination)
                        executions += 1
                    _ = engine, module, target_machine

    print(f"mmi0_extended_ir_runtime: PASS ({executions} executions; "
          "O0/O3, signed saturation, packed lanes, 128-bit aliases/zero)")


if __name__ == "__main__":
    main()
