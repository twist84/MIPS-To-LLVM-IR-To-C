#!/usr/bin/env python3
"""JIT-test DIV/DIVU and DIV1/DIVU1, including zero and signed overflow."""

import ctypes
import pathlib
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm


class U128(ctypes.Structure):
    _fields_ = [("lo", ctypes.c_uint64), ("hi", ctypes.c_uint64)]


class EEState(ctypes.Structure):
    _fields_ = [
        ("gpr", U128 * 32),
        ("hi", ctypes.c_uint64),
        ("lo", ctypes.c_uint64),
        ("hi1", ctypes.c_uint64),
        ("lo1", ctypes.c_uint64),
        ("sa", ctypes.c_uint32),
        ("pc", ctypes.c_uint32),
        ("exit_reason", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("cop0", ctypes.c_uint32 * 32),
        ("fpr", ctypes.c_uint64 * 32),
        ("vu0", U128 * 32),
        ("fcsr", ctypes.c_uint32),
        ("fpu_acc", ctypes.c_uint32),
    ]


def make_aligned_state():
    backing = ctypes.create_string_buffer(ctypes.sizeof(EEState) + 15)
    address = ctypes.addressof(backing)
    aligned_address = (address + 15) & ~15
    return backing, ctypes.cast(aligned_address, ctypes.POINTER(EEState)).contents


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run_case(fixture, temp_dir, opt_level, case, target_machine):
    ir_path = temp_dir / f"div1_case{case}_o{opt_level}.ll"
    subprocess.run(
        [str(fixture), str(ir_path), str(opt_level), str(case)],
        check=True,
    )
    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()
    address = engine.get_function_address("guest_div1_test")
    require(address != 0, f"O{opt_level} case {case}: missing guest function")
    guest_fn = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )(address)

    backing, state = make_aligned_state()
    _ = backing
    primary = case >= 9
    variant = case % 9
    initial_hi = 0x9999AAAABBBBCCCC
    initial_lo = 0xDDDDEEEEFFFF0000
    initial_hi1 = 0x1111222233334444
    initial_lo1 = 0x5555666677778888
    state.hi, state.lo = initial_hi, initial_lo
    state.hi1, state.lo1 = initial_hi1, initial_lo1
    state.gpr[4].hi = 0xABCDABCDABCDABCD
    state.gpr[5].hi = 0x1234123412341234
    state.gpr[6].hi = 0x1111111122222222
    state.gpr[7].hi = 0x3333333344444444

    if variant == 0:  # signed -17 / 5
        state.gpr[4].lo = (-17) & 0xFFFFFFFFFFFFFFFF
        state.gpr[5].lo = 5
        expected_hi, expected_lo = 0xFFFFFFFFFFFFFFFE, 0xFFFFFFFFFFFFFFFD
    elif variant == 1:  # signed INT_MIN / -1 (never reach unsafe sdiv)
        state.gpr[4].lo = 0x80000000
        state.gpr[5].lo = 0xFFFFFFFF
        expected_hi, expected_lo = 0, 0xFFFFFFFF80000000
    elif variant == 2:  # signed nonzero dividend / zero
        state.gpr[4].lo = (-17) & 0xFFFFFFFFFFFFFFFF
        state.gpr[5].lo = 0
        expected_hi, expected_lo = 0xFFFFFFFFFFFFFFEF, 1
    elif variant == 3:  # unsigned 0xfffffffe / 3
        state.gpr[4].lo = 0xFFFFFFFE
        state.gpr[5].lo = 3
        expected_hi, expected_lo = 2, 0x55555554
    elif variant == 4:  # unsigned nonzero dividend / zero
        state.gpr[4].lo = 0xFFFFFFFE
        state.gpr[5].lo = 0
        expected_hi, expected_lo = 0xFFFFFFFFFFFFFFFE, 0xFFFFFFFFFFFFFFFF
    elif variant == 5:  # signed zero / zero
        state.gpr[4].lo = 0
        state.gpr[5].lo = 0
        expected_hi, expected_lo = 0, 0xFFFFFFFFFFFFFFFF
    elif variant == 6:  # unsigned zero / zero
        state.gpr[4].lo = 0
        state.gpr[5].lo = 0
        expected_hi, expected_lo = 0, 0xFFFFFFFFFFFFFFFF
    elif variant == 7:  # positive signed dividend / zero
        state.gpr[4].lo = 53
        state.gpr[5].lo = 0
        expected_hi, expected_lo = 53, 0xFFFFFFFFFFFFFFFF
    else:  # signed INT_MIN / zero; high source word is irrelevant
        state.gpr[4].lo = 0x1234567880000000
        state.gpr[5].lo = 0xDEADBEEF00000000
        expected_hi, expected_lo = 0xFFFFFFFF80000000, 1

    original_rs = state.gpr[4].lo
    original_rt = state.gpr[5].lo
    result = guest_fn(ctypes.pointer(state), None)
    prefix = f"O{opt_level} case {case} ({'DIV/DIVU' if primary else 'DIV1/DIVU1'}): "
    require(result == 3, prefix + f"unexpected exit result {result}")
    require(state.exit_reason == 3, prefix + f"unexpected exit reason {state.exit_reason}")
    if primary:
        require(state.hi == expected_hi,
                prefix + f"HI got 0x{state.hi:016x}, expected 0x{expected_hi:016x}")
        require(state.lo == expected_lo,
                prefix + f"LO got 0x{state.lo:016x}, expected 0x{expected_lo:016x}")
        require(state.hi1 == initial_hi1 and state.lo1 == initial_lo1,
                prefix + "DIV/DIVU changed the independent HI1/LO1 pair")
    else:
        require(state.hi1 == expected_hi,
                prefix + f"HI1 got 0x{state.hi1:016x}, expected 0x{expected_hi:016x}")
        require(state.lo1 == expected_lo,
                prefix + f"LO1 got 0x{state.lo1:016x}, expected 0x{expected_lo:016x}")
        require(state.hi == initial_hi and state.lo == initial_lo,
                prefix + "DIV1/DIVU1 changed the independent HI/LO pair")
    require(state.gpr[6].lo == expected_hi,
            prefix + f"MFHI got 0x{state.gpr[6].lo:016x}")
    require(state.gpr[7].lo == expected_lo,
            prefix + f"MFLO got 0x{state.gpr[7].lo:016x}")
    require(state.gpr[6].hi == 0x1111111122222222
            and state.gpr[7].hi == 0x3333333344444444,
            prefix + "MFHI/MFLO overwrote an upper GPR half")
    require(state.gpr[4].lo == original_rs and state.gpr[5].lo == original_rt,
            prefix + "division unexpectedly changed source GPRs")
    require(state.gpr[4].hi == 0xABCDABCDABCDABCD
            and state.gpr[5].hi == 0x1234123412341234,
            prefix + "division unexpectedly changed source upper halves")
    return engine, module


def main():
    require(len(sys.argv) == 2, "usage: test_div1_ir_runtime.py path/to/ee_div1_ir_fixture")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    engines, modules, target_machines = [], [], []

    with tempfile.TemporaryDirectory(prefix="ee_div1_ir_") as directory:
        temp_dir = pathlib.Path(directory)
        for opt_level in range(4):
            for case in range(18):
                # MCJIT takes ownership of backend resources; give each engine
                # its own target machine rather than sharing one across engines.
                target_machine = target.create_target_machine(opt=0)
                target_machines.append(target_machine)
                engine, module = run_case(
                    fixture, temp_dir, opt_level, case, target_machine
                )
                engines.append(engine)
                modules.append(module)

    print("div1_ir_runtime: PASS (O0-O3; DIV/DIVU and DIV1/DIVU1; signed quotient/remainder; INT_MIN/-1; zero divisors; independent HI/LO banks)")


if __name__ == "__main__":
    main()
