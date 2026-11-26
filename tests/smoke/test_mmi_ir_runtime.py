#!/usr/bin/env python3
"""Compile and execute generated EE LLVM at each module-builder opt level."""

import ctypes
import pathlib
import re
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs


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
        ("fpu_acc_overflow", ctypes.c_uint32),
    ]


def make_aligned_state():
    backing = ctypes.create_string_buffer(ctypes.sizeof(EEState) + 15)
    address = ctypes.addressof(backing)
    aligned_address = (address + 15) & ~15
    state = ctypes.cast(aligned_address, ctypes.POINTER(EEState)).contents
    return backing, state


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run_opt_level(fixture_exe, temp_dir, opt_level, target_machine):
    ir_path = temp_dir / f"mmi_transfer_o{opt_level}.ll"
    subprocess.run([str(fixture_exe), str(ir_path), str(opt_level)], check=True)

    ir = ir_path.read_text(encoding="utf-8")
    if opt_level == 0:
        # The aliased PAND result feeds the immediately following PXOR
        # directly as a packed SSA value, not an extract/reinsert pair.
        guest = ir.split("@guest_mmi_transfer_test(", 1)[1]
        packed_ands = re.findall(r"(%t\d+) = and <2 x i64> ", guest)
        packed_xors = re.findall(r"= xor <2 x i64> (%t\d+), ", guest)
        require(bool(set(packed_ands) & set(packed_xors)),
                "packed producer is not reused directly by its consumer")
    module = llvm.parse_assembly(ir)
    module.verify()
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()

    function_address = engine.get_function_address("guest_mmi_transfer_test")
    require(function_address != 0, f"O{opt_level}: JIT did not expose guest function")
    guest_fn_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    guest_fn = guest_fn_type(function_address)

    backing, state = make_aligned_state()
    _ = backing  # Keep the aligned storage alive throughout the native call.
    state.gpr[4].lo = 0x55
    state.gpr[4].hi = 0x4444444444444444
    state.gpr[5].lo = 0x66
    state.gpr[5].hi = 0x5555555555555555
    state.gpr[6].lo = 0x77
    state.gpr[6].hi = 0x6666666666666666
    state.gpr[14].lo = 0xDEAD
    state.gpr[14].hi = 0x1414141414141414
    state.gpr[8].lo = 0x0123456789ABCDEF
    state.gpr[8].hi = 0xFEDCBA9876543210
    state.gpr[9].lo = 0x00FF00FF00FF00FF
    state.gpr[9].hi = 0x0F0F0F0F0F0F0F0F
    state.hi = 0x00000000FFFFFFFE
    state.lo = 0x0000000000000003
    state.hi1 = 0x1111222233334444
    state.lo1 = 0x5555666677778888
    source_a_lo, source_a_hi = state.gpr[8].lo, state.gpr[8].hi
    source_b_lo, source_b_hi = state.gpr[9].lo, state.gpr[9].hi

    result = guest_fn(ctypes.pointer(state), None)

    prefix = f"O{opt_level}: "
    require(result == 3, prefix + f"unexpected exit result: {result}")
    require(state.exit_reason == 3, prefix + f"unexpected exit reason: {state.exit_reason}")
    require(state.pc == 0x001000B8, prefix + f"unexpected dispatcher PC: 0x{state.pc:08x}")
    require(state.gpr[4].lo == 0x1234, prefix + f"ADDIU writeback lost: 0x{state.gpr[4].lo:x}")
    require(state.gpr[4].hi == 0x4444444444444444, prefix + "ADDIU clobbered upper GPR half")
    require(state.gpr[14].lo == 0x5678,
            prefix + f"unused MTHI1 rd field killed cross-block GPR14 writeback: 0x{state.gpr[14].lo:x}")
    require(state.gpr[14].hi == 0x1414141414141414,
            prefix + "cross-block writeback clobbered GPR14 upper half")
    require(state.hi == 0, prefix + f"MADD/MADDU HI accumulator wrong: 0x{state.hi:x}")
    require(state.lo == 0xFFFFFFFFFFFFFFF7,
            prefix + f"MADD/MADDU LO accumulator wrong: 0x{state.lo:x}")
    require(state.gpr[23].lo == 0xFFFFFFFFFFFFFFFD,
            prefix + f"MADD rd result wrong: 0x{state.gpr[23].lo:x}")
    require(state.gpr[24].lo == 0xFFFFFFFFFFFFFFF7,
            prefix + f"MADDU rd result wrong: 0x{state.gpr[24].lo:x}")
    require(state.gpr[29].lo == 0,
            prefix + f"MFHI did not read accumulated HI: 0x{state.gpr[29].lo:x}")
    require(state.gpr[30].lo == 0xFFFFFFFFFFFFFFF7,
            prefix + f"MFLO did not read accumulated LO: 0x{state.gpr[30].lo:x}")
    require(state.gpr[25].lo == 0,
            prefix + f"MADD1/MADDU1 HI1 accumulator wrong: 0x{state.gpr[25].lo:x}")
    require(state.gpr[26].lo == 0xFFFFFFFFFFFFFFF7,
            prefix + f"MADD1/MADDU1 LO1 accumulator wrong: 0x{state.gpr[26].lo:x}")
    require(state.gpr[5].lo == 0x1234, prefix + f"MFHI1 result missing: 0x{state.gpr[5].lo:x}")
    require(state.gpr[5].hi == 0x5555555555555555, prefix + "MFHI1 clobbered upper GPR half")
    require(state.gpr[6].lo == 0x1234, prefix + f"MFLO1 result missing: 0x{state.gpr[6].lo:x}")
    require(state.gpr[6].hi == 0x6666666666666666, prefix + "MFLO1 clobbered upper GPR half")
    require(state.gpr[1].lo == 0xFFFFFFFFFFFFFFFA,
            prefix + f"MULT1 rd result wrong: 0x{state.gpr[1].lo:x}")
    require(state.gpr[2].lo == 0xFFFFFFFFFFFFFFFF,
            prefix + f"MULT1 HI1 result wrong: 0x{state.gpr[2].lo:x}")
    require(state.gpr[3].lo == 0xFFFFFFFFFFFFFFFA,
            prefix + f"MULT1 LO1 result wrong: 0x{state.gpr[3].lo:x}")
    require(state.hi1 == 2, prefix + f"MULTU1 HI1 result wrong: 0x{state.hi1:x}")
    require(state.lo1 == 0xFFFFFFFFFFFFFFFA,
            prefix + f"MULTU1 LO1 result wrong: 0x{state.lo1:x}")
    require(state.gpr[27].lo == 2,
            prefix + f"MFHI1 after MULTU1 wrong: 0x{state.gpr[27].lo:x}")
    require(state.gpr[28].lo == 0xFFFFFFFFFFFFFFFA,
            prefix + f"MFLO1 after MULTU1 wrong: 0x{state.gpr[28].lo:x}")

    a_lo, a_hi = source_a_lo, source_a_hi
    b_lo, b_hi = source_b_lo, source_b_hi
    mask64 = 0xFFFFFFFFFFFFFFFF
    mask128 = (1 << 128) - 1
    a128 = (a_hi << 64) | a_lo
    b128 = (b_hi << 64) | b_lo

    def packed_lanes(width, operation):
        lane_mask = (1 << width) - 1
        result = 0
        for shift in range(0, 128, width):
            lhs = (a128 >> shift) & lane_mask
            rhs = (b128 >> shift) & lane_mask
            if operation == "add":
                value = lhs + rhs
            elif operation == "sub":
                value = lhs - rhs
            elif operation == "max_signed":
                sign = 1 << (width - 1)
                lhs_signed = lhs - (1 << width) if lhs & sign else lhs
                rhs_signed = rhs - (1 << width) if rhs & sign else rhs
                value = max(lhs_signed, rhs_signed)
            else:
                raise AssertionError("unknown packed operation")
            result |= (value & lane_mask) << shift
        return result & mask128

    def halves(value):
        return value & mask64, (value >> 64) & mask64

    expected = {
        7: (a_lo & b_lo, a_hi & b_hi),
        10: (a_lo ^ b_lo, a_hi ^ b_hi),
        11: (a_lo | b_lo, a_hi | b_hi),
        12: (~(a_lo | b_lo) & mask64, ~(a_hi | b_hi) & mask64),
        8: (a_lo & b_lo, a_hi & b_hi),
        13: ((a_lo & b_lo) ^ b_lo, (a_hi & b_hi) ^ b_hi),
        15: halves(packed_lanes(32, "add")),
        16: halves(packed_lanes(32, "sub")),
        17: halves(packed_lanes(32, "max_signed")),
        18: halves(packed_lanes(16, "add")),
        19: halves(packed_lanes(16, "sub")),
        20: halves(packed_lanes(16, "max_signed")),
        21: halves(packed_lanes(8, "add")),
        22: halves(packed_lanes(8, "sub")),
        31: (0x1234 ^ b_lo, (a_hi & b_hi) ^ b_hi),
        0: (0, 0),
    }
    for register, (expected_lo, expected_hi) in expected.items():
        actual = state.gpr[register]
        require(actual.lo == expected_lo and actual.hi == expected_hi,
                prefix + f"MMI logical result GPR{register} wrong: "
                f"got ({actual.hi:016x}:{actual.lo:016x}), "
                f"expected ({expected_hi:016x}:{expected_lo:016x})")

    return engine, module


def main():
    require(len(sys.argv) == 2, "usage: test_mmi_ir_runtime.py path/to/ee_mmi_ir_fixture")
    fixture_exe = pathlib.Path(sys.argv[1]).resolve()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    register_ee_diagnostic_stubs()
    target = llvm.Target.from_default_triple()
    engines = []
    target_machines = []
    modules = []

    with tempfile.TemporaryDirectory(prefix="ee_mmi_ir_") as directory:
        temp_dir = pathlib.Path(directory)
        for opt_level in range(4):
            target_machine = target.create_target_machine(opt=0)
            target_machines.append(target_machine)
            result = run_opt_level(fixture_exe, temp_dir, opt_level, target_machine)
            engines.append(result[0])
            modules.append(result[1])

    print("mmi_ir_runtime: PASS (O0-O3; CFG writeback; HI1/LO1; MADD/MADDU and MULT1/MULTU1; aliasing; upper-GPR preservation; MMI logical and packed arithmetic lanes)")


if __name__ == "__main__":
    main()
