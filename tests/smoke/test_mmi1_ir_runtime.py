#!/usr/bin/env python3
"""JIT-test exact MMI1 packed absolute/compare/min/saturating arithmetic."""

import ctypes
import pathlib
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


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def pack_lanes(lanes, width):
    mask = (1 << width) - 1
    value = 0
    for index, lane in enumerate(lanes):
        value |= (lane & mask) << (index * width)
    return value


def signed_lane(value, width):
    sign = 1 << (width - 1)
    return value - (1 << width) if value & sign else value


def lane_operation(lhs, rhs, width, operation):
    count = 128 // width
    mask = (1 << width) - 1
    result = []
    for index in range(count):
        shift = index * width
        a = (lhs >> shift) & mask
        b = (rhs >> shift) & mask

        if operation == "abs_sat":
            signed_a = signed_lane(a, width)
            value = min(abs(signed_a), (1 << (width - 1)) - 1)
        elif operation == "equal":
            value = mask if a == b else 0
        elif operation == "min_signed":
            value = min(signed_lane(a, width), signed_lane(b, width))
        elif operation == "add_unsigned_sat":
            value = min(a + b, mask)
        elif operation == "sub_unsigned_sat":
            value = max(a - b, 0)
        else:
            raise AssertionError(f"unknown lane operation: {operation}")

        result.append(value & mask)
    return pack_lanes(result, width)


def pad_s_bh(lhs, rhs):
    result = []
    for index in range(8):
        a = (lhs >> (index * 16)) & 0xFFFF
        b = (rhs >> (index * 16)) & 0xFFFF
        value = a - b if index < 4 else a + b
        result.append(value & 0xFFFF)
    return pack_lanes(result, 16)


def pext_upper(lhs, rhs, width):
    lane_count = 128 // width
    start = lane_count // 2
    mask = (1 << width) - 1
    result = []
    for index in range(start, lane_count):
        a = (lhs >> (index * width)) & mask
        b = (rhs >> (index * width)) & mask
        # Each output pair is {rs lane, rt lane} in bit notation, which
        # means rt is the lower lane and rs is the upper lane.
        result.extend((b, a))
    return pack_lanes(result, width)


def qfsrv(lhs, rhs, shift):
    mask128 = (1 << 128) - 1
    if shift == 0:
        return rhs
    return ((lhs << (128 - shift)) | (rhs >> shift)) & mask128


def split_u128(value):
    mask64 = (1 << 64) - 1
    return value & mask64, (value >> 64) & mask64


def run_opt_level(fixture_exe, temp_dir, opt_level, byte_shift, half_shift):
    ir_path = temp_dir / f"mmi1_o{opt_level}_b{byte_shift}_h{half_shift}.ll"
    subprocess.run([str(fixture_exe), str(ir_path), str(opt_level)], check=True)

    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    module.triple = llvm.get_default_triple()

    target = llvm.Target.from_default_triple()
    target_machine = target.create_target_machine(opt=0)
    module.data_layout = str(target_machine.target_data)

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()

    address = engine.get_function_address("guest_mmi1_test")
    require(address != 0, f"O{opt_level}: JIT did not expose guest function")
    guest_fn_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    guest_fn = guest_fn_type(address)

    backing = ctypes.create_string_buffer(ctypes.sizeof(EEState) + 15)
    aligned = (ctypes.addressof(backing) + 15) & ~15
    state = ctypes.cast(aligned, ctypes.POINTER(EEState)).contents

    lhs_words = [0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678]
    rhs_words = [0x7FFFFFFF, 0x80000000, 0x00000002, 0x87654321]
    lhs = pack_lanes(lhs_words, 32)
    rhs = pack_lanes(rhs_words, 32)
    state.gpr[4].lo, state.gpr[4].hi = split_u128(lhs)
    state.gpr[5].lo, state.gpr[5].hi = split_u128(rhs)
    state.gpr[25].lo = byte_shift // 8
    state.gpr[26].lo = half_shift // 16
    # Distinct lower and upper doublewords prove PCPYLD operand ordering
    # and PCPYH's independent halfword broadcast in each doubleword.
    copy_a = (0x1122334455667788, 0x99AABBCCDDEEFF01)
    copy_b = (0xFEDCBA9876543210, 0x0123456789ABCDEF)
    state.gpr[29].lo, state.gpr[29].hi = copy_a
    state.gpr[30].lo, state.gpr[30].hi = copy_b
    # Exact 0x70491BA9 from SDBZ: PCPYUD v1,v0,t1. Distinct
    # high/low halves make operand-order mistakes immediately visible.
    game_rs = (0x0123456789ABCDEF, 0x1122334455667788)
    game_rt = (0xFEDCBA9876543210, 0x8877665544332211)
    state.gpr[2].lo, state.gpr[2].hi = game_rs
    state.gpr[9].lo, state.gpr[9].hi = game_rt

    result = guest_fn(ctypes.pointer(state), None)
    prefix = f"O{opt_level} MTSAB={byte_shift} MTSAH={half_shift}: "
    require(result == 3, prefix + f"unexpected exit result {result}")
    require(state.exit_reason == 3, prefix + f"unexpected exit reason {state.exit_reason}")

    expected_values = {
        6: lane_operation(lhs, 0, 32, "abs_sat"),
        7: lane_operation(lhs, 0, 16, "abs_sat"),
        8: lane_operation(lhs, rhs, 32, "equal"),
        9: lane_operation(lhs, rhs, 16, "equal"),
        10: lane_operation(lhs, rhs, 8, "equal"),
        11: lane_operation(lhs, rhs, 32, "min_signed"),
        12: lane_operation(lhs, rhs, 16, "min_signed"),
        13: lane_operation(lhs, rhs, 32, "add_unsigned_sat"),
        14: lane_operation(lhs, rhs, 32, "sub_unsigned_sat"),
        15: lane_operation(lhs, rhs, 16, "add_unsigned_sat"),
        16: lane_operation(lhs, rhs, 16, "sub_unsigned_sat"),
        17: lane_operation(lhs, rhs, 8, "add_unsigned_sat"),
        18: lane_operation(lhs, rhs, 8, "sub_unsigned_sat"),
        19: pad_s_bh(lhs, rhs),
        20: pext_upper(lhs, rhs, 8),
        21: pext_upper(lhs, rhs, 16),
        22: pext_upper(lhs, rhs, 32),
        23: qfsrv(lhs, rhs, byte_shift),
        24: qfsrv(lhs, rhs, half_shift),
        # Final PCEQW intentionally aliases rd with rs.
        4: lane_operation(lhs, rhs, 32, "equal"),
    }

    def broadcast_halfword(value):
        return (value & 0xFFFF) * 0x0001000100010001

    # PCPYLD lower64 = rt.lower64; upper64 = rs.lower64.
    expected_values[25] = (copy_a[0] << 64) | copy_b[0]
    expected_values[26] = (broadcast_halfword(copy_b[1]) << 64) | broadcast_halfword(copy_b[0])
    expected_values[27] = (broadcast_halfword(copy_a[1]) << 64) | broadcast_halfword(copy_a[0])
    # PCPYUD writes rd.lo=rs.hi, rd.hi=rt.hi. Exercise both
    # register alias directions after PCPYLD overwrites SP, and a
    # zero-source/zero-destination combination.
    expected_values[29] = (copy_b[1] << 64) | copy_a[0]
    expected_values[30] = (copy_b[1] << 64) | copy_a[0]
    expected_values[31] = (copy_b[1] << 64) | copy_a[0]
    expected_values[2] = copy_b[1] << 64
    expected_values[3] = (game_rt[1] << 64) | game_rs[1]
    expected_values[28] = (
        (broadcast_halfword(copy_a[0]) << 64) |
        broadcast_halfword(copy_b[0])
    )
    for register, expected in expected_values.items():
        expected_lo, expected_hi = split_u128(expected)
        actual = state.gpr[register]
        require(actual.lo == expected_lo and actual.hi == expected_hi,
                prefix + f"GPR{register}: got ({actual.hi:016x}:{actual.lo:016x}), "
                f"expected ({expected_hi:016x}:{expected_lo:016x})")

    require(state.gpr[0].lo == 0 and state.gpr[0].hi == 0,
            prefix + "PCPYUD must discard writes to GPR zero")
    # This lane set checks INT_MIN saturation in both ABS variants, word
    # unsigned-add carry, unsigned-subtract underflow, and repeated equal lanes.
    require(state.gpr[6].lo >> 32 == 0x7FFFFFFF,
            prefix + "PABSW failed to clamp INT_MIN to INT_MAX")
    require((state.gpr[7].lo >> 16) & 0xFFFF == 0x7FFF,
            prefix + "PABSH failed to clamp INT_MIN to INT_MAX")
    return engine, module, target_machine


def main():
    require(len(sys.argv) == 2, "usage: test_mmi1_ir_runtime.py path/to/ee_mmi1_ir_fixture")
    fixture_exe = pathlib.Path(sys.argv[1]).resolve()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    register_ee_diagnostic_stubs()

    engines = []
    modules = []
    target_machines = []
    with tempfile.TemporaryDirectory(prefix="ee_mmi1_ir_") as directory:
        temp_dir = pathlib.Path(directory)
        for opt_level in range(4):
            for byte_shift, half_shift in ((0, 0), (8, 16), (40, 32), (120, 112)):
                engine, module, target_machine = run_opt_level(
                    fixture_exe, temp_dir, opt_level, byte_shift, half_shift
                )
                engines.append(engine)
                modules.append(module)
                target_machines.append(target_machine)

    print("mmi1_ir_runtime: PASS (O0-O3; packed MMI1; PCPYH/PCPYLD/PCPYUD; SDBZ opcode; 128-bit source halves; aliases; QFSRV)")



if __name__ == "__main__":
    main()
