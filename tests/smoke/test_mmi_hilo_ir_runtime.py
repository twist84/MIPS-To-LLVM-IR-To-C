#!/usr/bin/env python3
"""JIT-validate PMFHL/PMTHL full HI/LO-bank semantics and partial writes."""

import ctypes
import pathlib
import random
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs
from test_mmi_ir_runtime import EEState, U128, make_aligned_state, require


MASK32 = (1 << 32) - 1
MASK64 = (1 << 64) - 1
MASK128 = (1 << 128) - 1
RETURN_PC = 0x00300000
DESTINATION = 0xFEDCBA98765432100123456789ABCDEF
RETURN_MODES = ("lw", "uw", "slw", "lh", "sh")
MODES = (
    [f"pmfhl_{mode}" for mode in RETURN_MODES]
    + [f"pmfhl_{mode}_zero" for mode in RETURN_MODES]
    + [f"pmthl_then_pmfhl_{mode}" for mode in RETURN_MODES]
    + ["pmthl", "pmthl_zero", "pmthl_from_prior_gpr",
       "pmthl_from_prior_gpr_cross"]
)


def signed(value, bits):
    if value & (1 << (bits - 1)):
        return value - (1 << bits)
    return value


def saturate(value, bits):
    return max(-(1 << (bits - 1)), min((1 << (bits - 1)) - 1, value))


def pack(elements, width):
    result = 0
    for index, value in enumerate(elements):
        result |= (value & ((1 << width) - 1)) << (index * width)
    return result


def split32(value):
    return [value & MASK32, (value >> 32) & MASK32]


def reference_pmthl(registers, source):
    # HI/LO are represented as four 64-bit halves: LO0, HI0, LO1, HI1.
    # Only word 0 of each is overwritten, preserving word 1 unmodified.
    words = [(source >> (index * 32)) & MASK32 for index in range(4)]
    return [
        (old & ~MASK32) | word
        for old, word in zip(registers, words)
    ]


def reference_pmfhl(mode, registers):
    lo0, hi0, lo1, hi1 = registers
    if mode in ("lw", "uw"):
        shift = 0 if mode == "lw" else 32
        return pack(
            [(reg >> shift) & MASK32 for reg in (lo0, hi0, lo1, hi1)],
            32,
        )
    if mode == "slw":
        values = []
        for lo, hi in ((lo0, hi0), (lo1, hi1)):
            pair = ((hi & MASK32) << 32) | (lo & MASK32)
            clamped = saturate(signed(pair, 64), 32)
            values.append(clamped & MASK64)
        return pack(values, 64)
    if mode == "lh":
        # Copy the low halfword of each word, first LO then HI, per bank.
        return pack(
            [(register >> shift) & 0xFFFF
             for register in (lo0, hi0, lo1, hi1)
             for shift in (0, 32)],
            16,
        )
    if mode == "sh":
        return pack(
            [saturate(signed(word, 32), 16)
             for register in (lo0, hi0, lo1, hi1)
             for word in split32(register)],
            16,
        )
    raise AssertionError("unhandled PMFHL mode " + mode)


def create_guest(fixture, directory, mode, opt_level, target):
    ir_file = directory / f"{mode}_O{opt_level}.ll"
    subprocess.run(
        [str(fixture), str(ir_file), mode, str(opt_level)], check=True
    )
    module = llvm.parse_assembly(ir_file.read_text(encoding="utf-8"))
    module.verify()
    machine = target.create_target_machine(opt=0)
    module.triple = llvm.get_default_triple()
    module.data_layout = str(machine.target_data)
    module.verify()
    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), machine)
    engine.add_module(module)
    engine.finalize_object()
    entry = engine.get_function_address("guest_mmi_hilo_test")
    require(entry != 0, mode + ": no JIT entry point")
    function_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    return function_type(entry), engine, module, machine


def test_run(function, mode, pattern):
    lo0, hi0, lo1, hi1, source = pattern
    cpu_backing, cpu = make_aligned_state()
    cpu.gpr[4] = U128(source & MASK64, source >> 64)
    cpu.gpr[6] = U128(DESTINATION & MASK64, DESTINATION >> 64)
    cpu.gpr[10] = U128(0xDEAD, 0x123456789ABCDEF0)
    cpu.gpr[31] = U128(RETURN_PC, 0)
    cpu.lo, cpu.hi, cpu.lo1, cpu.hi1 = lo0, hi0, lo1, hi1

    original_registers = [lo0, hi0, lo1, hi1]
    original_source = source
    writes_source = mode.startswith("pmthl_from_prior_gpr")
    if writes_source:
        # ADDIU $a0,zero,0x1234 writes low64 but preserves upper 64.
        source = (source & (MASK128 ^ MASK64)) | 0x1234

    pmthl = mode.startswith("pmthl")
    pmfhl = mode.startswith("pmfhl") or "then_pmfhl" in mode
    zero_source = mode == "pmthl_zero"
    if pmthl:
        current_registers = reference_pmthl(
            original_registers, 0 if zero_source else source
        )
    else:
        current_registers = original_registers

    if pmfhl:
        pmfhl_mode = mode.split("pmfhl_")[-1].removesuffix("_zero")
        expected_dest = (
            DESTINATION if mode.endswith("_zero")
            else reference_pmfhl(pmfhl_mode, current_registers)
        )
    else:
        expected_dest = DESTINATION

    result = function(ctypes.pointer(cpu), None)
    _ = cpu_backing
    prefix = mode + ": "
    require(result == 0 and cpu.exit_reason == 0 and cpu.pc == RETURN_PC,
            prefix + f"expected JR return, got {result} PC={cpu.pc:08x}")
    require(cpu.gpr[10].lo == 0x1357 and
            cpu.gpr[10].hi == 0x123456789ABCDEF0,
            prefix + "prior GPR write or upper half lost")
    require(cpu.gpr[0].lo == 0 and cpu.gpr[0].hi == 0,
            prefix + "zero register changed")
    actual_dest = cpu.gpr[6].lo | (cpu.gpr[6].hi << 64)
    require(actual_dest == expected_dest,
            prefix + f"rd={actual_dest:032x} wanted {expected_dest:032x}")
    actual_source = cpu.gpr[4].lo | (cpu.gpr[4].hi << 64)
    require(actual_source == source,
            prefix + f"rs={actual_source:032x} wanted {source:032x}")
    actual_registers = [cpu.lo, cpu.hi, cpu.lo1, cpu.hi1]
    require(actual_registers == current_registers,
            prefix + "HI/LO mismatch: got " +
            ", ".join(f"{reg:016x}" for reg in actual_registers) +
            "; expected " +
            ", ".join(f"{reg:016x}" for reg in current_registers))
    require(cpu.gpr[31].lo == RETURN_PC,
            prefix + "return GPR changed")
    require(original_source == (pattern[-1] & MASK128),
            prefix + "source test fixture corrupted")


def main():
    require(len(sys.argv) == 2,
            "usage: test_mmi_hilo_ir_runtime.py fixture")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    register_ee_diagnostic_stubs()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()

    rng = random.Random(0x5900F1)
    patterns = [
        (0, 0, 0, 0, 0),
        (MASK64, MASK64, MASK64, MASK64, MASK128),
        (0xFFFFFFFF80000000, 0x800000007FFFFFFF,
         0x7FFFFFFF00000001, 0x00000000FFFFFFFF,
         0x0123456789ABCDEFFEDCBA9876543210),
        (0x00000000FFFFFFFF, 0x000000007FFFFFFF,
         0xFFFFFFFF00000000, 0xFFFFFFFF80000000,
         0x80000000000000007FFFFFFF00000000),
        (0x12345678ABCDEF01, 0xFEDCBA9887654321,
         0x0123456789ABCDEF, 0xFEEDFACEBAD0C0DE,
         0x0102030405060708090A0B0C0D0E0F10),
        (0x0000000080000000, 0x000000007FFFFFFF,
         0x7FFFFFFF80000000, 0x800000007FFFFFFF,
         MASK128),
    ]
    patterns.extend(
        (rng.getrandbits(64), rng.getrandbits(64),
         rng.getrandbits(64), rng.getrandbits(64),
         rng.getrandbits(128))
        for _ in range(10)
    )

    executions = 0
    with tempfile.TemporaryDirectory(prefix="ee-mmi-hilo-") as tmp:
        for opt_level in (0, 3):
            for mode in MODES:
                function, engine, module, machine = create_guest(
                    fixture, pathlib.Path(tmp), mode, opt_level, target
                )
                for pattern in patterns:
                    test_run(function, mode, pattern)
                    executions += 1
                _ = engine, module, machine

    print(f"mmi_hilo_ir_runtime: PASS ({executions} native executions; "
          "O0/O3; PMFHL LW/UW/SLW/LH/SH; PMTHL partial write; "
          "zero source/destination; immediate and cross-block GPR hazards)")


if __name__ == "__main__":
    main()
