#!/usr/bin/env python3
"""Independent JIT execution of exact MMI2/MMI3 word ops and HI/LO transfers."""

import ctypes
import pathlib
import random
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs
from mmi_hardware_vectors import run_hardware_vectors
from test_mmi_ir_runtime import EEState, U128, make_aligned_state, require


MASK32 = (1 << 32) - 1
MASK64 = (1 << 64) - 1
MASK128 = (1 << 128) - 1
DEST = 0xFEDCBA98765432100123456789ABCDEF
RETURN_PC = 0x00300000

MODES = (
    ["pmfhi", "pmflo", "pmfhi_zero_dest", "pmflo_zero_dest",
     "pmthi", "pmtlo", "pmthi_zero_rs", "pmtlo_zero_rs",
     "pmthi_from_prior", "pmtlo_from_prior",
     "pmthi_from_prior_cross", "pmtlo_from_prior_cross",
     "pmthi_then_pmfhi", "pmtlo_then_pmflo"]
    + [name + suffix for name in ("pmultw", "pmultuw")
       for suffix in ("", "_alias_rs", "_alias_rt", "_zero_dest")]
    + ["pmultw_from_prior_cross"]
    + [name + suffix for name in ("pdivw", "pdivuw")
       for suffix in ("", "_zero_rs", "_zero_rt")]
)


def signed32(number):
    number &= MASK32
    return number - (1 << 32) if number & (1 << 31) else number


def sext32(number):
    return signed32(number) & MASK64


def reference(opcode, source_a, source_b, banks, target, chain):
    lo0, hi0, lo1, hi1 = banks
    original = [lo0, hi0, lo1, hi1]
    destination = DEST
    if opcode in ("pmfhi", "pmflo"):
        if target:
            source = [hi0, hi1] if opcode == "pmfhi" else [lo0, lo1]
            destination = source[0] | (source[1] << 64)
        return original, destination
    if opcode in ("pmthi", "pmtlo"):
        if opcode == "pmthi":
            original[1] = source_a & MASK64
            original[3] = (source_a >> 64) & MASK64
        else:
            original[0] = source_a & MASK64
            original[2] = (source_a >> 64) & MASK64
        if chain:
            source = [original[1], original[3]] if opcode == "pmthi" else [
                original[0], original[2]
            ]
            destination = source[0] | (source[1] << 64)
        return original, destination

    a_words = [source_a & MASK32, (source_a >> 64) & MASK32]
    b_words = [source_b & MASK32, (source_b >> 64) & MASK32]
    products = []
    for lane, (lhs, rhs) in enumerate(zip(a_words, b_words)):
        lo_index = 2 * lane
        hi_index = lo_index + 1
        if opcode.startswith("pmult"):
            if opcode == "pmultw":
                product = signed32(lhs) * signed32(rhs)
            else:
                product = lhs * rhs
            products.append(product & MASK64)
            quotient = product & MASK32
            remainder = (product >> 32) & MASK32
        else:
            if opcode == "pdivw":
                a, b = signed32(lhs), signed32(rhs)
                if b == 0:
                    quotient = 1 if a < 0 else -1
                    remainder = a
                else:
                    quotient = (abs(a) // abs(b)) * (
                        -1 if (a < 0) != (b < 0) else 1
                    )
                    remainder = a - quotient * b
            else:
                if rhs == 0:
                    quotient, remainder = -1, lhs
                else:
                    quotient, remainder = divmod(lhs, rhs)
        original[lo_index] = sext32(quotient)
        original[hi_index] = sext32(remainder)

    if opcode.startswith("pmult") and target:
        destination = products[0] | (products[1] << 64)
    return original, destination


def compile_guest(fixture, directory, mode, opt_level, target):
    ir_path = directory / f"{mode}_o{opt_level}.ll"
    subprocess.run([str(fixture), str(ir_path), mode, str(opt_level)],
                   check=True)
    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    machine = target.create_target_machine(opt=0)
    module.triple = llvm.get_default_triple()
    module.data_layout = str(machine.target_data)
    module.verify()
    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), machine)
    engine.add_module(module)
    engine.finalize_object()
    address = engine.get_function_address("guest_mmi_word_test")
    require(address != 0, mode + ": LLVM entry missing")
    fn = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.POINTER(EEState),
                          ctypes.c_void_p)(address)
    return fn, engine, module, machine


def test_case(fn, mode, pattern, hardware=None):
    lo0, hi0, lo1, hi1, lhs, rhs = pattern
    backing, cpu = make_aligned_state()
    cpu.gpr[4] = U128(lhs & MASK64, lhs >> 64)
    cpu.gpr[5] = U128(rhs & MASK64, rhs >> 64)
    cpu.gpr[6] = U128(DEST & MASK64, DEST >> 64)
    cpu.gpr[10] = U128(0xDEAD, 0xFEEDFACECAFEBEEF)
    cpu.gpr[31] = U128(RETURN_PC, 0)
    cpu.lo, cpu.hi, cpu.lo1, cpu.hi1 = lo0, hi0, lo1, hi1

    chain = "then" in mode
    prior = "from_prior" in mode
    zero_rs = mode.endswith("_zero_rs")
    zero_rt = mode.endswith("_zero_rt")
    zero_dest = mode.endswith("_zero_dest")
    alias_rs = mode.endswith("_alias_rs")
    alias_rt = mode.endswith("_alias_rt")

    base = mode.split("_")[0]
    if chain:
        base = "pmthi" if mode.startswith("pmthi") else "pmtlo"
    if prior:
        lhs = (lhs & (MASK128 ^ MASK64)) | 0x1234

    used_a = 0 if zero_rs else lhs
    used_b = 0 if zero_rt else rhs
    target = not zero_dest
    expected_banks, expected_destination = reference(
        base, used_a, used_b, [lo0, hi0, lo1, hi1], target, chain,
    )
    if hardware is not None:
        expected_destination, expected_banks = hardware
    if base.startswith("pmult") and alias_rs:
        wanted_rs = expected_destination
        wanted_rt = rhs
        wanted_dest = DEST
    elif base.startswith("pmult") and alias_rt:
        wanted_rs = lhs
        wanted_rt = expected_destination
        wanted_dest = DEST
    else:
        wanted_rs = lhs
        wanted_rt = rhs
        wanted_dest = expected_destination

    result = fn(ctypes.pointer(cpu), None)
    _ = backing
    prefix = mode + ": "
    require(result == 0 and cpu.exit_reason == 0 and cpu.pc == RETURN_PC,
            prefix + f"wrong exit/PC {result} 0x{cpu.pc:08x}")
    require(cpu.gpr[10].lo == 0x1357 and
            cpu.gpr[10].hi == 0xFEEDFACECAFEBEEF,
            prefix + "preceding ADDIU lost")
    require(cpu.gpr[0].lo == 0 and cpu.gpr[0].hi == 0,
            prefix + "GPR zero clobbered")
    require(cpu.gpr[4].lo | (cpu.gpr[4].hi << 64) == wanted_rs,
            prefix + "rs mismatch")
    require(cpu.gpr[5].lo | (cpu.gpr[5].hi << 64) == wanted_rt,
            prefix + "rt mismatch")
    actual_dest = cpu.gpr[6].lo | (cpu.gpr[6].hi << 64)
    require(actual_dest == wanted_dest,
            prefix + f"rd {actual_dest:032x} wanted {wanted_dest:032x}")
    actual_banks = [cpu.lo, cpu.hi, cpu.lo1, cpu.hi1]
    require(actual_banks == expected_banks,
            prefix + "HI/LO banks: got " +
            ", ".join(f"{x:016x}" for x in actual_banks) +
            "; expected " + ", ".join(f"{x:016x}" for x in expected_banks))
    require(cpu.gpr[31].lo == RETURN_PC, prefix + "return GPR changed")


def main():
    require(len(sys.argv) == 2,
            "usage: test_mmi_word_ir_runtime.py fixture")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    register_ee_diagnostic_stubs()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    rng = random.Random(0x5900D1)
    patterns = [
        (0, 0, 0, 0, 0, 0),
        (MASK64, MASK64, MASK64, MASK64, MASK128, MASK128),
        (0x0000000080000000, 0x00000000FFFFFFFF,
         0x12345678ABCDEF01, 0xFFFFFFFF7FFFFFFF,
         0x8000000080000000, 0xFFFFFFFFFFFFFFFF),
        (0x00000001FFFFFFFF, 0x12345678DEADBEEF,
         0xFFFFFFFF00000000, 0x55555555AAAAAAAA,
         0x0123456789ABCDEF800000007FFFFFFF,
         0xFEDCBA9876543210FFFFFFFF00000001),
        (0xFFFFFFFF80000000, 0x000000007FFFFFFF,
         0x7FFFFFFF80000000, 0x8000000000000000,
         0x000000007FFFFFFF0000000080000000,
         0x00000000FFFFFFFF00000000FFFFFFFF),
    ]
    patterns += [
        (rng.getrandbits(64), rng.getrandbits(64),
         rng.getrandbits(64), rng.getrandbits(64),
         rng.getrandbits(128), rng.getrandbits(128))
        for _ in range(11)
    ]
    completed = 0
    with tempfile.TemporaryDirectory(prefix="ee-mmi-word-") as directory:
        directory = pathlib.Path(directory)
        for opt_level in (0, 3):
            for mode in MODES:
                fn, engine, module, machine = compile_guest(
                    fixture, directory, mode, opt_level, target)
                for pattern in patterns:
                    test_case(fn, mode, pattern)
                    completed += 1
                completed += run_hardware_vectors(fn, mode, test_case)
                _ = engine, module, machine

    print(f"mmi_word_ir_runtime: PASS ({completed} native executions; "
          "O0/O3, signed/unsigned word lanes, zero division, INT_MIN/-1, "
          "two HI/LO banks, transfers, aliases and cross-block GPR input)")


if __name__ == "__main__":
    main()
