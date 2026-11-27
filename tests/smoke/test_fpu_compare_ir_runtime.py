#!/usr/bin/env python3
"""JIT-test the four supported R5900 quiet ordered single-precision predicates."""

import ctypes
import math
import pathlib
import random
import re
import struct
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm


FCC0_MASK = 1 << 23
ENTRY_PC = 0x00130000


class EEState(ctypes.Structure):
    _fields_ = [
        ("gpr", (ctypes.c_uint64 * 2) * 32),
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
        ("vu0", (ctypes.c_uint64 * 2) * 32),
        ("fcsr", ctypes.c_uint32),
        ("fpu_acc", ctypes.c_uint32),
        ("fpu_acc_overflow", ctypes.c_uint32),
    ]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def aligned_state():
    backing = ctypes.create_string_buffer(ctypes.sizeof(EEState) + 15)
    address = ctypes.addressof(backing)
    aligned = (address + 15) & ~15
    return backing, ctypes.cast(aligned, ctypes.POINTER(EEState)).contents


def f32_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def compile_jit(fixture, temp_dir, mode, target, opt=0):
    ir_path = temp_dir / f"{mode}.ll"
    subprocess.run([str(fixture), str(ir_path), mode], check=True)

    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    target_machine = target.create_target_machine(opt=opt)
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)
    module.verify()

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()
    address = engine.get_function_address("guest_fpu_compare_test")
    require(address != 0, f"{mode}: JIT function was not emitted")

    function_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    return function_type(address), engine, module, target_machine


def run_case(fixture, temp_dir, target, mode, lhs, rhs, expected_fcc, opt):
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, mode, target, opt
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)

    # Begin with FCC0 set so false predicates must actively clear it. All other
    # FCSR bits must be preserved by the compare's read/modify/write sequence.
    initial_fcsr = 0xA5923456
    state.fcsr = initial_fcsr
    state.fpr[1] = lhs
    state.fpr[2] = rhs

    result = guest_fn(ctypes.pointer(state), None)
    prefix = f"{mode}(0x{lhs:08x}, 0x{rhs:08x}, O{opt}): "
    require(result == 3, prefix + f"expected EE_EXIT_DISPATCH (3), got {result}")
    require(state.exit_reason == 3,
            prefix + f"state exit reason was {state.exit_reason}")
    require(state.pc == ENTRY_PC + 4,
            prefix + f"wrong sequential PC 0x{state.pc:08x}")

    expected_fcsr = (initial_fcsr & ~FCC0_MASK) | (
        FCC0_MASK if expected_fcc else 0
    )
    require(state.fcsr == expected_fcsr,
            prefix + f"FCSR got 0x{state.fcsr:08x}, expected 0x{expected_fcsr:08x}")


def run_cvt_w_case(fixture, temp_dir, target, source_bits, expected_bits, label):
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "cvt_w", target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)

    state.fpr[1] = source_bits
    state.fpr[6] = 0xAABBCCDD11223344
    initial_fcsr = 0xA5923456
    state.fcsr = initial_fcsr

    result = guest_fn(ctypes.pointer(state), None)
    prefix = f"cvt_w({label}, 0x{source_bits:08x}): "
    require(result == 3, prefix + f"expected EE_EXIT_DISPATCH (3), got {result}")
    require(state.exit_reason == 3,
            prefix + f"state exit reason was {state.exit_reason}")
    require(state.pc == ENTRY_PC + 4,
            prefix + f"wrong sequential PC 0x{state.pc:08x}")
    require(state.fpr[6] == expected_bits,
            prefix + f"FPR[6] got 0x{state.fpr[6]:016x}, "
                     f"expected 0x{expected_bits:016x}")
    require(state.fpr[1] == source_bits,
            prefix + "source FPR was modified")
    require(state.fcsr == initial_fcsr,
            prefix + f"CVT.W.S unexpectedly modified FCSR: 0x{state.fcsr:08x}")


def run_cvt_s_w_case(fixture, temp_dir, target, source_value, expected_bits, label, opt):
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "cvt_s_w", target, opt
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)

    source_bits = source_value & 0xFFFFFFFF
    state.fpr[1] = (0xDEADBEEF << 32) | source_bits
    state.fpr[6] = 0xAABBCCDD11223344
    initial_fcsr = 0xA5923456
    state.fcsr = initial_fcsr

    result = guest_fn(ctypes.pointer(state), None)
    prefix = f"cvt_s_w({label}, {source_value}, O{opt}): "
    require(result == 3, prefix + f"expected EE_EXIT_DISPATCH (3), got {result}")
    require(state.exit_reason == 3,
            prefix + f"state exit reason was {state.exit_reason}")
    require(state.pc == ENTRY_PC + 4,
            prefix + f"wrong sequential PC 0x{state.pc:08x}")
    require(state.fpr[6] == expected_bits,
            prefix + f"FPR[6] got 0x{state.fpr[6]:016x}, "
                     f"expected 0x{expected_bits:08x}")
    require(state.fpr[1] == ((0xDEADBEEF << 32) | source_bits),
            prefix + "source FPR was modified")
    require(state.fcsr == initial_fcsr,
            prefix + f"CVT.S.W unexpectedly modified FCSR: 0x{state.fcsr:08x}")


def fp_reference_extreme(lhs_bits, rhs_bits, is_max):
    lhs_signed = ctypes.c_int32(lhs_bits).value
    rhs_signed = ctypes.c_int32(rhs_bits).value
    both_negative = lhs_signed < 0 and rhs_signed < 0

    if is_max:
        value = min(lhs_signed, rhs_signed) if both_negative else max(lhs_signed, rhs_signed)
    else:
        value = max(lhs_signed, rhs_signed) if both_negative else min(lhs_signed, rhs_signed)

    return value & 0xFFFFFFFF


def run_max_min_cases(fixture, temp_dir, target, mode, pairs):
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, mode, target
    )
    retained = (engine, module, target_machine)
    is_max = mode == "max_s"
    initial_fcsr = 0xA592F456
    expected_fcsr = initial_fcsr & ~(0x00008000 | 0x00004000)

    for lhs_bits, rhs_bits, label in pairs:
        backing, state = aligned_state()
        _ = (backing, retained)
        state.fpr[1] = lhs_bits
        state.fpr[2] = rhs_bits
        state.fpr[6] = 0xAABBCCDD11223344
        state.fcsr = initial_fcsr

        result = guest_fn(ctypes.pointer(state), None)
        prefix = f"{mode}({label}, 0x{lhs_bits:08x}, 0x{rhs_bits:08x}): "
        require(result == 3, prefix + f"expected EE_EXIT_DISPATCH (3), got {result}")
        require(state.exit_reason == 3,
                prefix + f"state exit reason was {state.exit_reason}")
        require(state.pc == ENTRY_PC + 4,
                prefix + f"wrong sequential PC 0x{state.pc:08x}")

        expected = fp_reference_extreme(lhs_bits, rhs_bits, is_max)
        require(state.fpr[6] == expected,
                prefix + f"FPR[6] got 0x{state.fpr[6]:016x}, "
                         f"expected 0x{expected:08x}")
        require(state.fpr[1] == lhs_bits and state.fpr[2] == rhs_bits,
                prefix + "source FPR was modified")
        require(state.fcsr == expected_fcsr,
                prefix + f"FCSR got 0x{state.fcsr:08x}, "
                         f"expected 0x{expected_fcsr:08x}")


def run_sign_operation_cases(fixture, temp_dir, target, mode, source_words):
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, mode, target
    )
    retained = (engine, module, target_machine)
    initial_fcsr = 0xA592F456
    expected_fcsr = initial_fcsr & ~(0x00008000 | 0x00004000)

    for source_bits, label in source_words:
        backing, state = aligned_state()
        _ = (backing, retained)
        state.fpr[1] = source_bits
        state.fpr[6] = 0xAABBCCDD11223344
        state.fcsr = initial_fcsr

        result = guest_fn(ctypes.pointer(state), None)
        prefix = f"{mode}({label}, 0x{source_bits:08x}): "
        require(result == 3, prefix + f"expected EE_EXIT_DISPATCH (3), got {result}")
        require(state.exit_reason == 3,
                prefix + f"state exit reason was {state.exit_reason}")
        require(state.pc == ENTRY_PC + 4,
                prefix + f"wrong sequential PC 0x{state.pc:08x}")

        expected = (
            source_bits & 0x7FFFFFFF
            if mode == "abs_s"
            else source_bits ^ 0x80000000
        )
        require(state.fpr[6] == expected,
                prefix + f"FPR[6] got 0x{state.fpr[6]:016x}, "
                         f"expected 0x{expected:08x}")
        require(state.fpr[1] == source_bits,
                prefix + "source FPR was modified")
        require(state.fcsr == expected_fcsr,
                prefix + f"FCSR got 0x{state.fcsr:08x}, "
                         f"expected 0x{expected_fcsr:08x}")


def run_arithmetic_cases(fixture, temp_dir, target, mode, cases, opt=0):
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, mode, target, opt
    )
    retained = (engine, module, target_machine)

    for (lhs_bits, rhs_bits, expected_bits,
         initial_fcsr, expected_fcsr, label) in cases:
        backing, state = aligned_state()
        _ = (backing, retained)
        state.fpr[1] = lhs_bits
        state.fpr[2] = rhs_bits
        state.fpr[6] = 0xAABBCCDD11223344
        state.fcsr = initial_fcsr

        result = guest_fn(ctypes.pointer(state), None)
        prefix = (
            f"{mode}({label}, 0x{lhs_bits:08x}, 0x{rhs_bits:08x}): "
        )
        require(result == 3, prefix + f"expected EE_EXIT_DISPATCH (3), got {result}")
        require(state.exit_reason == 3,
                prefix + f"state exit reason was {state.exit_reason}")
        require(state.pc == ENTRY_PC + 4,
                prefix + f"wrong sequential PC 0x{state.pc:08x}")
        require(state.fpr[6] == expected_bits,
                prefix + f"FPR[6] got 0x{state.fpr[6]:016x}, "
                         f"expected 0x{expected_bits:08x}")
        require(state.fpr[1] == lhs_bits and state.fpr[2] == rhs_bits,
                prefix + "source FPR was modified")
        require(state.fcsr == expected_fcsr,
                prefix + f"FCSR got 0x{state.fcsr:08x}, "
                         f"expected 0x{expected_fcsr:08x}")


def verify_add_identity_fast_path(fixture, temp_dir, target):
    """Compare the exact identity path against the unshortened integer adder."""
    path = temp_dir / "add_identity.ll"
    subprocess.run([str(fixture), str(path), "add_s"], check=True)
    ir = path.read_text(encoding="utf-8")
    match = re.search(
        r"define internal \{ i32, i1, i1 \} @ee_cop1_add_bits\([^\n]*\) \{\n.*?\n\}",
        ir, re.S)
    require(match is not None, "missing shared addition helper")
    guard = "br i1 %identity, label %identity_result, label %arithmetic"
    reference = match.group(0).replace("@ee_cop1_add_bits(", "@test_add_reference(", 1)
    require(guard in reference, "missing ADD/SUB identity guard")
    reference = reference.replace(guard, "br i1 false, label %identity_result, label %arithmetic")
    ir += "\n" + reference + "\n"
    for name, callee in (("test_add_fast", "ee_cop1_add_bits"),
                         ("test_add_full", "test_add_reference")):
        ir += f"""
define i64 @{name}(i32 %a, i32 %b, i32 %sub) {{
entry:
  %subtract = icmp ne i32 %sub, 0
  %r = call {{ i32, i1, i1 }} @{callee}(i32 %a, i32 %b, i1 %subtract)
  %bits = extractvalue {{ i32, i1, i1 }} %r, 0
  %o = extractvalue {{ i32, i1, i1 }} %r, 1
  %u = extractvalue {{ i32, i1, i1 }} %r, 2
  %bits64 = zext i32 %bits to i64
  %o64 = zext i1 %o to i64
  %u64 = zext i1 %u to i64
  %of = shl i64 %o64, 32
  %uf = shl i64 %u64, 33
  %flags = or i64 %of, %uf
  %out = or i64 %bits64, %flags
  ret i64 %out
}}
"""
    edges = [0, 1, 0x7fffff, 0x80000000, 0x807fffff,
             0x00800000, 0x00800001, 0x3f800000, 0x3fffffff,
             0x7f800000, 0x7fffffff, 0xffffffff]
    pairs = [(a, b) for a in edges for b in edges]
    for ea in range(256):
        for eb in range(256):
            a = (ea << 23) | ((ea * 65537 + eb * 17) & 0x7fffff)
            b = (eb << 23) | ((eb * 65537 + ea * 17) & 0x7fffff)
            for sa in (0, 0x80000000):
                for sb in (0, 0x80000000):
                    pairs.append((a | sa, b | sb))
    rng = random.Random(0xadd)
    pairs.extend((rng.getrandbits(32), rng.getrandbits(32)) for _ in range(16384))
    signature = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_uint32,
                                 ctypes.c_uint32, ctypes.c_uint32)
    for opt in (0, 3):
        module = llvm.parse_assembly(ir)
        module.verify()
        machine = target.create_target_machine(opt=opt)
        module.triple = llvm.get_default_triple()
        module.data_layout = str(machine.target_data)
        engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), machine)
        engine.add_module(module)
        engine.finalize_object()
        addresses = [engine.get_function_address(name)
                     for name in ("test_add_fast", "test_add_full")]
        require(all(addresses), "missing addition comparison entry")
        fast, full = map(signature, addresses)
        for a, b in pairs:
            for sub in (0, 1):
                got, expected = fast(a, b, sub), full(a, b, sub)
                if got != expected:
                    raise AssertionError((opt, hex(a), hex(b), sub, hex(got), hex(expected)))
        print(f"ADD/SUB identity: {len(pairs) * 2} tuples at O{opt} match full adder")


def verify_mul_correction_fast_path(fixture, temp_dir, target):
    """Compare every result bit and O/U flag against unconditional correction."""
    path = temp_dir / "mul_fast_path.ll"
    subprocess.run([str(fixture), str(path), "mul_s"], check=True)
    ir = path.read_text(encoding="utf-8")
    match = re.search(
        r"define internal \{ i32, i1, i1 \} @ee_cop1_mul_bits\([^\n]*\) \{\n.*?\n\}",
        ir, re.S)
    require(match is not None, "missing shared multiplication helper")
    reference = match.group(0).replace("@ee_cop1_mul_bits(", "@test_mul_reference(", 1)
    guard = "br i1 %needs_correction, label %correct, label %normalize"
    require(guard in reference, "missing Booth correction guard")
    # The original behavior always evaluates the unchanged correction tree.
    reference = reference.replace(guard, "br i1 true, label %correct, label %normalize")
    ir += "\n" + reference + "\n"
    for name, callee in (("test_mul_fast", "ee_cop1_mul_bits"),
                         ("test_mul_slow", "test_mul_reference")):
        ir += f"""
define i64 @{name}(i32 %a, i32 %b) {{
entry:
  %r = call {{ i32, i1, i1 }} @{callee}(i32 %a, i32 %b)
  %bits = extractvalue {{ i32, i1, i1 }} %r, 0
  %o = extractvalue {{ i32, i1, i1 }} %r, 1
  %u = extractvalue {{ i32, i1, i1 }} %r, 2
  %bits64 = zext i32 %bits to i64
  %o64 = zext i1 %o to i64
  %u64 = zext i1 %u to i64
  %o_shift = shl i64 %o64, 32
  %u_shift = shl i64 %u64, 33
  %flags = or i64 %o_shift, %u_shift
  %out = or i64 %bits64, %flags
  ret i64 %out
}}
"""
    # Directed residues 0, 32767, 32768 and 8388607, in both orders:
    # (2^23 + 1) * (2^23 + n) has residue n modulo 2^23.
    edges = [0, 0x80000000, 1, 0x807fffff, 0x00800000, 0x00800001,
             0x3f800000, 0x3f800001, 0x3f807fff, 0x3f808000, 0x3fffffff,
             0x7f800000, 0x7fffffff, 0xffffffff]
    pairs = [(a, b) for a in edges for b in edges]
    rng = random.Random(0x5900)
    pairs.extend((rng.getrandbits(32), rng.getrandbits(32)) for _ in range(100000))
    # Exercise thousands of values exactly around the guard threshold.
    for _ in range(2048):
        a = rng.randrange(1, 1 << 23, 2)
        residue = rng.choice((0, 1, 32767, 32768, 32769, 8388607))
        b = (pow(a, -1, 1 << 23) * residue) & 0x7fffff
        lhs, rhs = 0x3f800000 | a, 0x3f800000 | b
        pairs.extend(((lhs, rhs), (rhs, lhs)))

    signature = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32)
    for opt in (0, 3):
        module = llvm.parse_assembly(ir)
        module.verify()
        machine = target.create_target_machine(opt=opt)
        module.triple = llvm.get_default_triple()
        module.data_layout = str(machine.target_data)
        engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), machine)
        engine.add_module(module)
        engine.finalize_object()
        addresses = [engine.get_function_address(name)
                     for name in ("test_mul_fast", "test_mul_slow")]
        require(all(addresses), "missing multiplication comparison entry")
        fast, slow = map(signature, addresses)
        for a, b in pairs:
            got, expected = fast(a, b), slow(a, b)
            if got != expected:
                raise AssertionError((opt, hex(a), hex(b), hex(got), hex(expected)))
        # Physical console asymmetry: a nonzero correction really matters.
        require(fast(0x3f800000, 0x3fffffff) == 0x3ffffffe,
                "Booth correction lost at exact truncation boundary")
        require(fast(0x3fffffff, 0x3f800000) == 0x3fffffff,
                "operand order lost")
    print(f"MUL fast path: {len(pairs)} pairs at O0/O3 match unconditional correction")


def run_accumulator_cases(fixture, temp_dir, target, mode, cases, opt=0):
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, mode, target, opt
    )
    retained = (engine, module, target_machine)
    writes_fpr = mode in ("madd_s", "msub_s")
    fpr_sentinel = 0xAABBCCDD11223344

    for (fs_bits, ft_bits, acc_bits, expected_bits,
         initial_fcsr, expected_fcsr, label, *latch_case) in cases:
        backing, state = aligned_state()
        _ = (backing, retained)
        state.fpr[1] = fs_bits
        state.fpr[2] = ft_bits
        state.fpr[6] = fpr_sentinel
        state.fpu_acc = acc_bits
        state.fcsr = initial_fcsr
        initial_latch = latch_case[0] if latch_case else 0
        state.fpu_acc_overflow = initial_latch

        result = guest_fn(ctypes.pointer(state), None)
        prefix = (
            f"{mode}({label}, fs=0x{fs_bits:08x}, "
            f"ft=0x{ft_bits:08x}, acc=0x{acc_bits:08x}): "
        )
        require(result == 3, prefix + f"expected EE_EXIT_DISPATCH (3), got {result}")
        require(state.exit_reason == 3,
                prefix + f"state exit reason was {state.exit_reason}")
        require(state.pc == ENTRY_PC + 4,
                prefix + f"wrong sequential PC 0x{state.pc:08x}")
        require(state.fpr[1] == fs_bits and state.fpr[2] == ft_bits,
                prefix + "source FPR was modified")

        if writes_fpr:
            require(state.fpr[6] == expected_bits,
                    prefix + f"FPR[6] got 0x{state.fpr[6]:016x}, "
                             f"expected 0x{expected_bits:08x}")
            require(state.fpu_acc == acc_bits,
                    prefix + f"ACC changed to 0x{state.fpu_acc:08x}")
        else:
            require(state.fpu_acc == expected_bits,
                    prefix + f"ACC got 0x{state.fpu_acc:08x}, "
                             f"expected 0x{expected_bits:08x}")
            require(state.fpr[6] == fpr_sentinel,
                    prefix + "accumulator-only operation modified FPR[6]")

        if expected_fcsr is not None:
            require(state.fcsr == expected_fcsr,
                    prefix + f"FCSR got 0x{state.fcsr:08x}, "
                             f"expected 0x{expected_fcsr:08x}")
        if writes_fpr:
            require(state.fpu_acc_overflow == initial_latch,
                    prefix + "FPR destination changed ACC overflow latch")
        elif latch_case or expected_fcsr is not None:
            expected_latch = (latch_case[1] if latch_case
                              else int(bool(expected_fcsr & 0x8000)))
            require(state.fpu_acc_overflow == expected_latch,
                    prefix + f"wrong ACC overflow latch {state.fpu_acc_overflow}")


def run_cop1_transfer_cases(fixture, temp_dir, target):
    gpr_hi_sentinel = 0x8877665544332211
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "mfc1", target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)
    state.fpr[1] = 0xDEADBEEFBF800000
    state.gpr[6][0] = 0x0123456789ABCDEF
    state.gpr[6][1] = gpr_hi_sentinel

    result = guest_fn(ctypes.pointer(state), None)
    require(result == 3, f"MFC1: expected EE_EXIT_DISPATCH (3), got {result}")
    require(state.gpr[6][0] == 0xFFFFFFFFBF800000,
            f"MFC1 failed to sign-extend the FPR word: 0x{state.gpr[6][0]:016x}")
    require(state.gpr[6][1] == gpr_hi_sentinel,
            "MFC1 clobbered the upper 64-bit GPR half")
    require(state.fpr[1] == 0xDEADBEEFBF800000,
            "MFC1 modified its source FPR")

    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "mtc1", target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)
    state.gpr[8][0] = 0x12345678ABCDEF01
    state.gpr[8][1] = gpr_hi_sentinel
    state.fpr[6] = 0xAABBCCDD11223344

    result = guest_fn(ctypes.pointer(state), None)
    require(result == 3, f"MTC1: expected EE_EXIT_DISPATCH (3), got {result}")
    require(state.fpr[6] == 0x00000000ABCDEF01,
            f"MTC1 failed to transfer the low GPR word: 0x{state.fpr[6]:016x}")
    require(state.gpr[8][0] == 0x12345678ABCDEF01 and
            state.gpr[8][1] == gpr_hi_sentinel,
            "MTC1 modified its source GPR")

    initial_fcsr = 0xA592F456
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "cfc1", target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)
    state.fcsr = initial_fcsr
    state.gpr[6][0] = 0x0123456789ABCDEF
    state.gpr[6][1] = gpr_hi_sentinel

    result = guest_fn(ctypes.pointer(state), None)
    expected_gpr = 0x0182C051 # reserved bits masked; hardwired bits set
    require(result == 3, f"CFC1: expected EE_EXIT_DISPATCH (3), got {result}")
    require(state.gpr[6][0] == expected_gpr,
            f"CFC1 failed to sign-extend FCSR: 0x{state.gpr[6][0]:016x}")
    require(state.gpr[6][1] == gpr_hi_sentinel,
            "CFC1 clobbered the upper 64-bit GPR half")
    require(state.fcsr == initial_fcsr, "CFC1 modified FCSR")

    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "ctc1", target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)
    state.fcsr = 0x01234567
    state.gpr[8][0] = 0x12345678ABCDEF01
    state.gpr[8][1] = gpr_hi_sentinel

    result = guest_fn(ctypes.pointer(state), None)
    require(result == 3, f"CTC1: expected EE_EXIT_DISPATCH (3), got {result}")
    require(state.fcsr == 0x0181C001,
            f"CTC1 failed to mask FCSR: 0x{state.fcsr:08x}")
    require(state.gpr[8][0] == 0x12345678ABCDEF01 and
            state.gpr[8][1] == gpr_hi_sentinel,
            "CTC1 modified its source GPR")

    # Hardware observations in ps2autotests ee_fpu/fcr.expected.
    for mode, expected in [
        ("cfc1_fcr0", 0x00002e30),
        ("cfc1_alias15", 0x00002e30),
        ("cfc1_alias16", 0x0183c079),
        ("cfc1", 0x0183c079),
    ]:
        guest_fn, engine, module, target_machine = compile_jit(
            fixture, temp_dir, mode, target
        )
        backing, state = aligned_state()
        _ = (backing, engine, module, target_machine)
        state.fcsr = 0xffffffff
        state.gpr[6][1] = gpr_hi_sentinel
        require(guest_fn(ctypes.pointer(state), None) == 3 and
                state.gpr[6][0] == expected and
                state.gpr[6][1] == gpr_hi_sentinel and
                state.fcsr == 0xffffffff,
                f"{mode}: FCR read alias or read-only state mismatch")

    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "ctc1_fcr0", target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)
    state.fcsr = 0x01000079
    state.gpr[8][0] = 0xffffffff
    require(guest_fn(ctypes.pointer(state), None) == 3 and
            state.fcsr == 0x01000079 and state.gpr[8][0] == 0xffffffff,
            "CTC1 FCR0 must not modify FCR31 or the source GPR")

    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "ctc1", target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)
    state.gpr[8][0] = 0xffffffff
    require(guest_fn(ctypes.pointer(state), None) == 3 and
            state.fcsr == 0x0183c079,
            "CTC1 FCR31 all-one write must preserve only architectural bits")

    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "ctc1_after_branch", target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)
    state.fcsr = 0xDEADBEEF
    state.gpr[8][0] = 0xDEADBEEF
    state.gpr[8][1] = gpr_hi_sentinel

    result = guest_fn(ctypes.pointer(state), None)
    require(result == 3,
            f"CTC1 after branch: expected EE_EXIT_DISPATCH (3), got {result}")
    require(state.pc == ENTRY_PC + 16,
            f"CTC1 after branch: unexpected exit PC 0x{state.pc:08x}")
    require(state.fcsr == 0x01000041,
            f"CTC1 successor read stale GPR state: FCSR=0x{state.fcsr:08x}")
    require(state.gpr[8][0] == 0x2345,
            f"ADDIU predecessor write was lost: GPR8=0x{state.gpr[8][0]:016x}")
    require(state.gpr[8][1] == gpr_hi_sentinel,
            "ADDIU/CTC1 sequence clobbered the upper GPR half")


def run_hardware_fcr_control_cases(fixture, temp_dir, target, opt):
    # Literal physical-console observations from ps2autotests fcr.cpp
    # and fcr.expected at 97469ffbed8631277b94e28d01dabd702aa97ef3.
    # Keep expectations independent of the emitter's bit mask.
    write_cases = [
        (0x00000003, 0x01000001, "Update rounding mode (RM)"),
        (0x0000007C, 0x01000079, "Update flags"),
        (0x00000F80, 0x01000001, "Update enables"),
        (0x0001F000, 0x0101C001, "Update cause"),
        (0x00020000, 0x01020001, "Update unimplemented (E)"),
        (0x01000000, 0x01000001, "Update flushing (FS)"),
        (0x00400000, 0x01000001, "Update flushing (FO)"),
        (0x00200000, 0x01000001, "Update flushing (FN)"),
        (0x00800000, 0x01800001, "Update FCC"),
        (0xFE000000, 0x01000001, "Update FCC1-7"),
        (0x001C0000, 0x01000001, "Update unknown"),
    ]
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "ctc1", target, opt
    )
    retained = (engine, module, target_machine)
    for value, expected, label in write_cases:
        for initial in (0x01000001, 0x0183C079):
            backing, state = aligned_state()
            _ = (backing, retained)
            state.fcsr = initial
            state.gpr[8][0] = 0xA5A5A5A500000000 | value
            state.gpr[8][1] = 0x8877665544332211
            state.fpu_acc = 0xDEADBEEF
            state.fpu_acc_overflow = 1
            require(guest_fn(ctypes.pointer(state), None) == 3 and
                    state.pc == ENTRY_PC + 4 and
                    state.fcsr == expected and
                    state.gpr[8][0] == (0xA5A5A5A500000000 | value) and
                    state.gpr[8][1] == 0x8877665544332211 and
                    state.fpu_acc == 0xDEADBEEF and
                    state.fpu_acc_overflow == 1,
                    f"O{opt} {label}: CTC1 expected {expected:08x}, "
                    f"got {state.fcsr:08x}")

    # The reference prints all 32 aliases in both the initial state and
    # after the flags write. The first sixteen read FCR0, the last
    # sixteen read FCR31. The group fixtures use nonzero destination GPRs.
    for mode, expected_fcr0 in (("fcr_read_low", True),
                               ("fcr_read_high", False)):
        guest_fn, engine, module, target_machine = compile_jit(
            fixture, temp_dir, mode, target, opt
        )
        retained = (engine, module, target_machine)
        for initial in (0x01000001, 0x01000079):
            backing, state = aligned_state()
            _ = (backing, retained)
            state.fcsr = initial
            for i in range(1, 32):
                state.gpr[i][0] = 0x1234567800000000 | i
                state.gpr[i][1] = 0x8877665500000000 | i
            require(guest_fn(ctypes.pointer(state), None) == 3 and
                    state.pc == ENTRY_PC + 64 and state.fcsr == initial,
                    f"O{opt} {mode}: wrong exit or changed FCSR")
            for i in range(1, 32):
                expected = (0x00002E30 if expected_fcr0 else initial) if i <= 16 else (
                    0x1234567800000000 | i)
                require(state.gpr[i][0] == expected and
                        state.gpr[i][1] == (0x8877665500000000 | i),
                        f"O{opt} {mode}: GPR {i} alias or preservation mismatch")


def run_hardware_fcr_arithmetic_cases(fixture, temp_dir, target, opt):
    # Exact FCR31 observations, not flags calculated from the emitter.
    # ps2autotests fcr.cpp / fcr.expected, pinned at
    # 97469ffbed8631277b94e28d01dabd702aa97ef3.
    # The printed decimal/NaN results are not raw result words; the
    # separate existing arithmetic vectors cover those result bits.
    cases = {
        "sqrt_s": [
            (0, 0xBF800000, 0x01020041, "sqrt(-1)"),
        ],
        "div_s": [
            (0, 0, 0x01020041, "Divide zero by zero"),
            (0x3F800000, 0, 0x01010021, "Divide one by zero"),
            (0x00800000, 0x40400000, 0x01000001, "Underflow"),
            (0x3F800000, 0x4040FFFF, 0x01000001, "Inexact"),
        ],
        "add_s": [
            (0x7F800001, 0x7F800001, 0x01008011, "NAN math"),
        ],
        "mul_s": [
            (0x7F7FFFFF, 0x7F7FFFFF, 0x01008011, "Overflow"),
        ],
    }
    for mode, vectors in cases.items():
        guest_fn, engine, module, target_machine = compile_jit(
            fixture, temp_dir, mode, target, opt
        )
        retained = (engine, module, target_machine)
        for lhs, rhs, expected, label in vectors:
            backing, state = aligned_state()
            _ = (backing, retained)
            state.fcsr = 0x01000001  # reference writes CTC1 zero
            state.fpr[1] = lhs
            state.fpr[2] = rhs
            state.fpu_acc = 0xDEADBEEF
            state.fpu_acc_overflow = 1
            require(guest_fn(ctypes.pointer(state), None) == 3 and
                    state.exit_reason == 3 and state.pc == ENTRY_PC + 4 and
                    state.fcsr == expected and state.fpr[1] == lhs and
                    state.fpr[2] == rhs and state.fpu_acc == 0xDEADBEEF and
                    state.fpu_acc_overflow == 1,
                    f"O{opt} {mode} {label}: FCSR expected {expected:08x}, "
                    f"got {state.fcsr:08x}")


def run_bc1_delay_cases(fixture, temp_dir, target):
    # BC1T samples FCC0 before an ordinary delay slot that clears FCSR.
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "bc1_t_delay", target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)
    state.fcsr = 0x00800000
    state.gpr[8][0] = 0
    state.gpr[9][0] = 0xDEADBEEF
    state.gpr[10][0] = 0xCAFEBABE

    result = guest_fn(ctypes.pointer(state), None)
    require(result == 0,
            f"BC1T delay-slot: expected return exit (0), got {result}")
    require(state.fcsr == 0x01000001,
            f"BC1T delay-slot CTC1 did not execute: FCSR=0x{state.fcsr:08x}")
    require(state.gpr[10][0] == 0x2222,
            "BC1T was evaluated after its delay slot instead of before it")
    require(state.gpr[9][0] == 0xDEADBEEF,
            "BC1T unexpectedly executed the not-taken path")

    # BC1FL is not taken while FCC0 is true. Its delay slot must be annulled,
    # even though that slot would clear FCC0 if it were executed.
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "bc1_fl_annul", target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)
    state.fcsr = 0x00800000
    state.gpr[8][0] = 0
    state.gpr[9][0] = 0xDEADBEEF
    state.gpr[10][0] = 0xCAFEBABE

    result = guest_fn(ctypes.pointer(state), None)
    require(result == 3,
            f"BC1FL annulment: expected dispatcher exit (3), got {result}")
    require(state.fcsr == 0x00800000,
            f"BC1FL not-taken path executed its annulled CTC1: FCSR=0x{state.fcsr:08x}")
    require(state.gpr[9][0] == 0x1111,
            "BC1FL did not execute the not-taken path")
    require(state.gpr[10][0] == 0xCAFEBABE,
            "BC1FL unexpectedly executed the taken path")


def f32_value(bits):
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def ieee_f32_bits(value):
    try:
        return f32_bits(value)
    except OverflowError:
        return 0xFF800000 if value < 0.0 else 0x7F800000


def ee_fpu_normalise_bits(bits):
    exponent = bits & 0x7F800000
    sign = bits & 0x80000000
    if exponent == 0:
        return sign
    if exponent == 0x7F800000:
        return sign | 0x7F7FFFFF
    return bits


def ee_fpu_clamp_result_bits(bits):
    absolute = bits & 0x7FFFFFFF
    if absolute == 0x7F800000:
        return (bits & 0x80000000) | 0x7F7FFFFF
    if (bits & 0x7F800000) == 0 and (bits & 0x007FFFFF) != 0:
        return bits & 0x80000000
    return bits


def run_sqrt_cases(fixture, temp_dir, target, source_words, opt):
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "sqrt_s", target, opt
    )
    retained = (engine, module, target_machine)
    initial_fcsr = 0xA593F43E
    cleared_fcsr = initial_fcsr & ~0x00030000

    for source_bits, label in source_words:
        backing, state = aligned_state()
        _ = (backing, retained)
        state.fpr[0] = 0x41800000  # fs is not an operand (sqrt(16) = 4)
        state.fpr[2] = source_bits
        state.fpr[6] = 0xAABBCCDD11223344
        state.fcsr = initial_fcsr

        result = guest_fn(ctypes.pointer(state), None)
        prefix = f"sqrt_s({label}, 0x{source_bits:08x}): "
        require(result == 3, prefix + f"expected EE_EXIT_DISPATCH (3), got {result}")
        require(state.exit_reason == 3,
                prefix + f"state exit reason was {state.exit_reason}")
        require(state.pc == ENTRY_PC + 4,
                prefix + f"wrong sequential PC 0x{state.pc:08x}")

        # Literal real-console output words, independent of the float oracle.
        hardware = {
            0x00000000: 0x00000000, 0x80000000: 0x00000000,
            0x3F800000: 0x3F800000, 0x40000000: 0x3FB504F3,
            0x40400000: 0x3FDDB3D7, 0x41C80000: 0x40A00000,
            0x7FFFFFFF: 0x5FB504F3, 0xFFFFFFFF: 0x5FB504F3,
            0x7F800000: 0x5F800000, 0xFF800000: 0x5F800000,
            0x3FFFFFFF: 0x3FB504F3, 0xDEADBEEF: 0x4F152108,
            0x3FC00000: 0x3F9CC471, 0x7FC00000: 0x5F9CC471,
        }
        # Additional model controls: not claimed as physical-console captures.
        model = {
            0x40800000: 0x40000000, 0xC0800000: 0x40000000,
            0x00000001: 0x00000000, 0x80000001: 0x00000000,
            0x7FC12345: 0x5F9D3B2D, 0xFFC12345: 0x5F9D3B2D,
            0x0080EFD3: 0x200077B1, 0x3F80EFD3: 0x3F8077B1,
            0x4000EFD3: 0x3FB5AE39, 0x7F80EFD3: 0x5F8077B1,
            0xBF80EFD3: 0x3F8077B1,
        }
        expected = {**hardware, **model}[source_bits]
        expected_fcsr = cleared_fcsr
        if source_bits & 0x80000000:
            expected_fcsr |= 0x00020040  # I|SI
        require(state.fpr[6] == expected,
                prefix + f"FPR[6] got 0x{state.fpr[6]:016x}, "
                         f"expected 0x{expected:08x}")
        require(state.fpr[2] == source_bits and state.fpr[0] == 0x41800000,
                prefix + "source FPR was modified")
        require(state.fcsr == expected_fcsr,
                prefix + f"FCSR got 0x{state.fcsr:08x}, "
                         f"expected 0x{expected_fcsr:08x}")


def run_rsqrt_cases(fixture, temp_dir, target, operand_pairs, opt):
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "rsqrt_s", target, opt
    )
    retained = (engine, module, target_machine)
    initial_fcsr = 0xA593F47E
    cleared_fcsr = initial_fcsr & ~0x00030000

    for numerator_bits, denominator_bits, expected, label in operand_pairs:
        backing, state = aligned_state()
        _ = (backing, retained)
        state.fpr[1] = numerator_bits
        state.fpr[2] = denominator_bits
        state.fpr[6] = 0xAABBCCDD11223344
        state.fcsr = initial_fcsr

        result = guest_fn(ctypes.pointer(state), None)
        prefix = (
            f"rsqrt_s({label}, 0x{numerator_bits:08x}, "
            f"0x{denominator_bits:08x}): "
        )
        require(result == 3, prefix + f"expected EE_EXIT_DISPATCH (3), got {result}")
        require(state.exit_reason == 3,
                prefix + f"state exit reason was {state.exit_reason}")
        require(state.pc == ENTRY_PC + 4,
                prefix + f"wrong sequential PC 0x{state.pc:08x}")

        expected_fcsr = cleared_fcsr
        if denominator_bits & 0x80000000:
            expected_fcsr |= 0x00020040  # I|SI
        if (denominator_bits & 0x7F800000) == 0:
            if (numerator_bits & 0x7F800000) == 0:
                expected_fcsr |= 0x00020040  # zero/zero: I|SI
            else:
                expected_fcsr |= 0x00010020  # nonzero/zero: D|SD

        require(state.fpr[6] == expected,
                prefix + f"FPR[6] got 0x{state.fpr[6]:016x}, "
                         f"expected 0x{expected:08x}")
        require(state.fpr[1] == numerator_bits and state.fpr[2] == denominator_bits,
                prefix + "source FPR was modified")
        require(state.fcsr == expected_fcsr,
                prefix + f"FCSR got 0x{state.fcsr:08x}, "
                         f"expected 0x{expected_fcsr:08x}")


def check_matrix_chain(fixture, temp_dir, target):
    for opt in (0, 3):
        fn, engine, module, machine = compile_jit(
            fixture, temp_dir, "matrix_chain", target, opt)
        raw = (temp_dir / "matrix_chain.ll").read_text(encoding="utf-8")
        guest = raw.split("define i32 @guest_fpu_compare_test(", 1)[1].split("\n}\n", 1)[0]
        # Only the two incoming FPRs require loads, ACC and latch are
        # already written by MULA.S. No float round-trip for MOV.S.
        require(len(re.findall(r" = load i64, ptr ", guest)) == 2,
                "matrix chain reloaded raw FPR inputs")
        require("bitcast i32" not in guest, "MOV.S used host FP")
        backing, state = aligned_state()
        state.fpr[1] = 0xdeadbeef40000000
        state.fpr[2] = 0x1234567840400000
        state.fcsr = 0x0000c008
        require(fn(ctypes.pointer(state), None) == 3, "matrix chain exit")
        require(state.pc == ENTRY_PC + 20, "matrix chain PC")
        require(state.fpr[6] == f32_bits(6) and
                state.fpu_acc == f32_bits(12) and
                state.fpr[7] == state.fpr[8] == f32_bits(24),
                "matrix chain FPR/ACC result")
        require(state.fpu_acc_overflow == 0 and state.fcsr == 8,
                "matrix chain flags")
        require(state.fpr[1] == 0xdeadbeef40000000 and
                state.fpr[2] == 0x1234567840400000, "matrix chain sources")
        _ = backing, engine, module, machine


def main():
    require(
        len(sys.argv) == 2,
        "usage: test_fpu_compare_ir_runtime.py path/to/ee_fpu_compare_ir_fixture",
    )
    fixture = pathlib.Path(sys.argv[1]).resolve()

    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()

    # Raw-word expectations from ps2autotests ee_fpu/compare.expected
    # (97469ffbed8631277b94e28d01dabd702aa97ef3), plus negative ordering
    # and boundary cases. Never route exponent-255 payloads through Python
    # floats, which can quiet signaling NaNs or lose their raw mantissas.
    cases = [
        ("c_f", 0x3f800000, 0x3f800000, False),
        ("c_f", 0x7fffffff, 0x7fffffff, False),
        ("c_eq", 0x3fc00000, 0x3fc00000, True),
        ("c_eq", 0x3fc00000, 0x40200000, False),
        ("c_eq", 0x00000000, 0x80000000, True),
        ("c_eq", 0x7fffffff, 0x7fffffff, True),
        ("c_eq", 0x7f800000, 0x7f800001, False),
        ("c_eq", 0x7f800001, 0x7f800000, False),
        ("c_eq", 0x7f7fffff, 0x7f800000, False),
        ("c_eq", 0x00000001, 0x00000000, True),
        ("c_eq", 0x80000001, 0x00000000, True),
        ("c_eq", 0xffffffff, 0xffffffff, True),
        ("c_eq", 0x7fffffff, 0xffffffff, False),
        ("c_lt", 0xbf800000, 0x00000000, True),
        ("c_lt", 0x80000000, 0x00000000, False),
        ("c_lt", 0x7fffffff, 0xbf800000, False),
        ("c_lt", 0xbf800000, 0x7fffffff, True),
        ("c_lt", 0x7f800000, 0x7f800001, True),
        ("c_lt", 0x7f800001, 0x7f800000, False),
        ("c_lt", 0xff800001, 0xff800000, True),
        ("c_lt", 0xff800000, 0xff800001, False),
        ("c_lt", 0x80000001, 0x00000001, False),
        ("c_le", 0x00000000, 0x00000000, True),
        ("c_le", 0xc0000000, 0xbf800000, True),
        ("c_le", 0x40000000, 0x3f800000, False),
        ("c_le", 0x7fffffff, 0x3f800000, False),
        ("c_le", 0xbf800000, 0x7fffffff, True),
        ("c_le", 0x7f800000, 0x7f800001, True),
        ("c_le", 0x7f800001, 0x7f800000, False),
        ("c_le", 0xff800001, 0xff800000, True),
        ("c_le", 0xff800000, 0xff800001, False),
        ("c_le", 0x80000001, 0x00000001, True),
    ]

    # CVT.W.S uses the EE's truncating conversion and saturates values whose
    # exponent is outside the signed 32-bit range. These expected results match
    # the PCSX2 EE interpreter's CVT_W implementation.
    cvt_cases = [
        (0x40700000, 0x00000003, "positive fraction truncates"),
        (0xC0700000, 0x00000000FFFFFFFD, "negative fraction truncates toward zero"),
        (0x3F000000, 0x00000000, "positive half"),
        (0xBF000000, 0x00000000, "negative half"),
        (0x00000000, 0x00000000, "positive zero"),
        (0x80000000, 0x00000000, "negative zero"),
        (0x00000001, 0x00000000, "positive denormal"),
        (0x80000001, 0x00000000, "negative denormal"),
        (0x4EFFFFFF, 0x000000007FFFFF80, "largest value below 2^31"),
        (0xCEFFFFFF, 0x0000000080000080, "negative value just above -2^31"),
        (0x4F000000, 0x000000007FFFFFFF, "positive 2^31 saturates"),
        (0xCF000000, 0x0000000080000000, "negative 2^31 clamps"),
        (0xCF000001, 0x0000000080000000, "negative overflow saturates"),
        (0x7F800000, 0x000000007FFFFFFF, "positive infinity saturates"),
        (0xFF800000, 0x0000000080000000, "negative infinity saturates"),
        (0x7FC00000, 0x000000007FFFFFFF, "positive quiet NaN saturates"),
        (0xFFC00000, 0x0000000080000000, "negative quiet NaN saturates"),
    ]

    # MAC instructions share arithmetic but differ in destination and latch writes.
    accumulator_normal_flags = 0xA592F45E
    accumulator_no_exception = accumulator_normal_flags & ~0xC000
    adda_cases = [
        (0x3FC00000, 0x40100000, 0x3F800000, 0x40700000,
         accumulator_normal_flags, accumulator_no_exception, "1.5 + 2.25"),
    ]
    suba_cases = [
        (0x3FC00000, 0x40100000, 0x3F800000, 0xBF400000,
         accumulator_normal_flags, accumulator_no_exception, "1.5 - 2.25"),
    ]
    mula_cases = [
        (0x3FC00000, 0x40000000, 0x3F800000, 0x40400000,
         accumulator_normal_flags, accumulator_no_exception, "1.5 * 2.0"),
    ]
    # Literal console results from muldiv.expected. ACC initialization
    # uses ADDA with -0, so exponent-zero payloads are flushed.
    # That fixture does not record FCSR or the internal ACC overflow latch.
    mac_hardware_words = {
        "madd_s": [
            (0x00000000, 0x00000000, 0x00000000, 0x00000000),
            (0x00000000, 0x80000000, 0x00000000, 0x00000000),
            (0x00000000, 0x80000000, 0x80000000, 0x80000000),
            (0x80000000, 0x00000000, 0x00000000, 0x00000000),
            (0x80000000, 0x80000000, 0x00000000, 0x00000000),
            (0x80000000, 0x80000000, 0x80000000, 0x00000000),
            (0x00000000, 0x3f800000, 0x00000000, 0x00000000),
            (0x3f800000, 0x3f800000, 0x00000000, 0x3f800000),
            (0x3f800000, 0x3f800000, 0x3f800000, 0x40000000),
            (0x3f800000, 0x00000000, 0x3f800000, 0x3f800000),
            (0x40000000, 0x40000000, 0x40000000, 0x40c00000),
            (0x40400000, 0x3f800000, 0x40400000, 0x40c00000),
            (0x40400000, 0x40400000, 0x00000000, 0x41100000),
            (0x7fffffff, 0x7fffffff, 0x00000000, 0x7fffffff),
            (0x00000000, 0x00000000, 0x7fffffff, 0x7fffffff),
            (0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff),
            (0x7fffffff, 0x7fffffff, 0xffffffff, 0x7fffffff),
            (0x7fffffff, 0xffffffff, 0x00000000, 0xffffffff),
            (0x7fffffff, 0xffffffff, 0x7fffffff, 0xffffffff),
            (0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff),
            (0xffffffff, 0x7fffffff, 0x00000000, 0xffffffff),
            (0xffffffff, 0xffffffff, 0x00000000, 0x7fffffff),
            (0xffffffff, 0xffffffff, 0xffffffff, 0x7fffffff),
            (0x00000000, 0x00000000, 0xffffffff, 0xffffffff),
            (0x7fffffff, 0x00000000, 0x00000000, 0x00000000),
            (0x00000000, 0x7fffffff, 0x00000000, 0x00000000),
            (0x00000000, 0x7f800000, 0x00000000, 0x00000000),
            (0x7f800000, 0x7f800000, 0x00000000, 0x7fffffff),
            (0x7f800000, 0x7f800000, 0x7f800000, 0x7fffffff),
            (0xff800000, 0x7f800000, 0x00000000, 0xffffffff),
            (0xff800000, 0x7f800000, 0xff800000, 0xffffffff),
            (0x00000000, 0x00000000, 0xff800000, 0xff800000),
            (0x00000000, 0x00000000, 0x80000000, 0x00000000),
            (0x00000001, 0x00000001, 0x00000000, 0x00000000),
            (0x3f800000, 0x3f800000, 0x7f800001, 0x7f800001),
            (0x7f800001, 0x7f800001, 0x7f800001, 0x7fffffff),
            (0x3f800000, 0xbf800000, 0x00000000, 0xbf800000),
            (0x3fffffff, 0x3f800000, 0x00000000, 0x3fffffff),
            (0x7f800001, 0x3f800000, 0x00000000, 0x7f800001),
            (0x00000001, 0x3f800000, 0x00000000, 0x00000000),
            (0x3f800000, 0x3fffffff, 0x00000000, 0x3ffffffe),
            (0x3f800000, 0x7f800001, 0x00000000, 0x7f800001),
            (0x3f800000, 0x00000001, 0x00000000, 0x00000000),
            (0x3fffffff, 0x3fffffff, 0x00000000, 0x407ffffe),
            (0x7f800001, 0x7f800001, 0x00000000, 0x7fffffff),
            (0x7fffffff, 0x3f800000, 0x00000000, 0x7fffffff),
            (0xffffffff, 0x3f800000, 0x00000000, 0xffffffff),
            (0x3f800000, 0x7fffffff, 0x00000000, 0x7ffffffe),
            (0x3f800000, 0xffffffff, 0x00000000, 0xfffffffe),
            (0x00001337, 0xdeadbeef, 0x00000000, 0x00000000),
        ],
        "msub_s": [
            (0x00000000, 0x00000000, 0x00000000, 0x00000000),
            (0x00000000, 0x80000000, 0x00000000, 0x00000000),
            (0x00000000, 0x80000000, 0x80000000, 0x00000000),
            (0x80000000, 0x00000000, 0x00000000, 0x00000000),
            (0x80000000, 0x80000000, 0x00000000, 0x00000000),
            (0x80000000, 0x80000000, 0x80000000, 0x80000000),
            (0x00000000, 0x3f800000, 0x00000000, 0x00000000),
            (0x3f800000, 0x3f800000, 0x00000000, 0xbf800000),
            (0x3f800000, 0x3f800000, 0x3f800000, 0x00000000),
            (0x3f800000, 0x00000000, 0x3f800000, 0x3f800000),
            (0x40000000, 0x40000000, 0x40000000, 0xc0000000),
            (0x40400000, 0x3f800000, 0x40400000, 0x00000000),
            (0x40400000, 0x40400000, 0x00000000, 0xc1100000),
            (0x7fffffff, 0x7fffffff, 0x00000000, 0xffffffff),
            (0x00000000, 0x00000000, 0x7fffffff, 0x7fffffff),
            (0x7fffffff, 0x7fffffff, 0x7fffffff, 0xffffffff),
            (0x7fffffff, 0x7fffffff, 0xffffffff, 0xffffffff),
            (0x7fffffff, 0xffffffff, 0x00000000, 0x7fffffff),
            (0x7fffffff, 0xffffffff, 0x7fffffff, 0x7fffffff),
            (0x7fffffff, 0xffffffff, 0xffffffff, 0x7fffffff),
            (0xffffffff, 0x7fffffff, 0x00000000, 0x7fffffff),
            (0xffffffff, 0xffffffff, 0x00000000, 0xffffffff),
            (0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff),
            (0x00000000, 0x00000000, 0xffffffff, 0xffffffff),
            (0x7fffffff, 0x00000000, 0x00000000, 0x00000000),
            (0x00000000, 0x7fffffff, 0x00000000, 0x00000000),
            (0x00000000, 0x7f800000, 0x00000000, 0x00000000),
            (0x7f800000, 0x7f800000, 0x00000000, 0xffffffff),
            (0x7f800000, 0x7f800000, 0x7f800000, 0xffffffff),
            (0xff800000, 0x7f800000, 0x00000000, 0x7fffffff),
            (0xff800000, 0x7f800000, 0xff800000, 0x7fffffff),
            (0x00000000, 0x00000000, 0xff800000, 0xff800000),
            (0x00000000, 0x00000000, 0x80000000, 0x80000000),
            (0x00000001, 0x00000001, 0x00000000, 0x00000000),
            (0x3f800000, 0x3f800000, 0x7f800001, 0x7f800001),
            (0x7f800001, 0x7f800001, 0x7f800001, 0xffffffff),
            (0x3f800000, 0xbf800000, 0x00000000, 0x3f800000),
            (0x3fffffff, 0x3f800000, 0x00000000, 0xbfffffff),
            (0x7f800001, 0x3f800000, 0x00000000, 0xff800001),
            (0x00000001, 0x3f800000, 0x00000000, 0x00000000),
            (0x3f800000, 0x3fffffff, 0x00000000, 0xbffffffe),
            (0x3f800000, 0x7f800001, 0x00000000, 0xff800001),
            (0x3f800000, 0x00000001, 0x00000000, 0x00000000),
            (0x3fffffff, 0x3fffffff, 0x00000000, 0xc07ffffe),
            (0x7f800001, 0x7f800001, 0x00000000, 0xffffffff),
            (0x7fffffff, 0x3f800000, 0x00000000, 0xffffffff),
            (0xffffffff, 0x3f800000, 0x00000000, 0x7fffffff),
            (0x3f800000, 0x7fffffff, 0x00000000, 0xfffffffe),
            (0x3f800000, 0xffffffff, 0x00000000, 0x7ffffffe),
            (0x00001337, 0xdeadbeef, 0x00000000, 0x00000000),
        ],
        "madda_s": [
            (0x00000000, 0x00000000, 0x00000000, 0x00000000),
            (0x00000000, 0x80000000, 0x00000000, 0x00000000),
            (0x00000000, 0x80000000, 0x80000000, 0x80000000),
            (0x80000000, 0x00000000, 0x00000000, 0x00000000),
            (0x80000000, 0x80000000, 0x00000000, 0x00000000),
            (0x80000000, 0x80000000, 0x80000000, 0x00000000),
            (0x00000000, 0x3f800000, 0x00000000, 0x00000000),
            (0x3f800000, 0x3f800000, 0x00000000, 0x3f800000),
            (0x3f800000, 0x3f800000, 0x3f800000, 0x40000000),
            (0x3f800000, 0x00000000, 0x3f800000, 0x3f800000),
            (0x40000000, 0x40000000, 0x40000000, 0x40c00000),
            (0x40400000, 0x3f800000, 0x40400000, 0x40c00000),
            (0x40400000, 0x40400000, 0x00000000, 0x41100000),
            (0x7fffffff, 0x7fffffff, 0x00000000, 0x7fffffff),
            (0x00000000, 0x00000000, 0x7fffffff, 0x7fffffff),
            (0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff),
            (0x7fffffff, 0x7fffffff, 0xffffffff, 0x7fffffff),
            (0x7fffffff, 0xffffffff, 0x00000000, 0xffffffff),
            (0x7fffffff, 0xffffffff, 0x7fffffff, 0xffffffff),
            (0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff),
            (0xffffffff, 0x7fffffff, 0x00000000, 0xffffffff),
            (0xffffffff, 0xffffffff, 0x00000000, 0x7fffffff),
            (0xffffffff, 0xffffffff, 0xffffffff, 0x7fffffff),
            (0x00000000, 0x00000000, 0xffffffff, 0xffffffff),
            (0x7fffffff, 0x00000000, 0x00000000, 0x00000000),
            (0x00000000, 0x7fffffff, 0x00000000, 0x00000000),
            (0x00000000, 0x7f800000, 0x00000000, 0x00000000),
            (0x7f800000, 0x7f800000, 0x00000000, 0x7fffffff),
            (0x7f800000, 0x7f800000, 0x7f800000, 0x7fffffff),
            (0xff800000, 0x7f800000, 0x00000000, 0xffffffff),
            (0xff800000, 0x7f800000, 0xff800000, 0xffffffff),
            (0x00000000, 0x00000000, 0xff800000, 0xff800000),
            (0x00000000, 0x00000000, 0x80000000, 0x00000000),
            (0x00000001, 0x00000001, 0x00000000, 0x00000000),
            (0x3f800000, 0x3f800000, 0x7f800001, 0x7f800001),
            (0x7f800001, 0x7f800001, 0x7f800001, 0x7fffffff),
            (0x3f800000, 0xbf800000, 0x00000000, 0xbf800000),
            (0x3fffffff, 0x3f800000, 0x00000000, 0x3fffffff),
            (0x7f800001, 0x3f800000, 0x00000000, 0x7f800001),
            (0x00000001, 0x3f800000, 0x00000000, 0x00000000),
            (0x3f800000, 0x3fffffff, 0x00000000, 0x3ffffffe),
            (0x3f800000, 0x7f800001, 0x00000000, 0x7f800001),
            (0x3f800000, 0x00000001, 0x00000000, 0x00000000),
            (0x3fffffff, 0x3fffffff, 0x00000000, 0x407ffffe),
            (0x7f800001, 0x7f800001, 0x00000000, 0x7fffffff),
            (0x7fffffff, 0x3f800000, 0x00000000, 0x7fffffff),
            (0xffffffff, 0x3f800000, 0x00000000, 0xffffffff),
            (0x3f800000, 0x7fffffff, 0x00000000, 0x7ffffffe),
            (0x3f800000, 0xffffffff, 0x00000000, 0xfffffffe),
            (0x00001337, 0xdeadbeef, 0x00000000, 0x00000000),
        ],
        "msuba_s": [
            (0x00000000, 0x00000000, 0x00000000, 0x00000000),
            (0x00000000, 0x80000000, 0x00000000, 0x00000000),
            (0x00000000, 0x80000000, 0x80000000, 0x00000000),
            (0x80000000, 0x00000000, 0x00000000, 0x00000000),
            (0x80000000, 0x80000000, 0x00000000, 0x00000000),
            (0x80000000, 0x80000000, 0x80000000, 0x80000000),
            (0x00000000, 0x3f800000, 0x00000000, 0x00000000),
            (0x3f800000, 0x3f800000, 0x00000000, 0xbf800000),
            (0x3f800000, 0x3f800000, 0x3f800000, 0x00000000),
            (0x3f800000, 0x00000000, 0x3f800000, 0x3f800000),
            (0x40000000, 0x40000000, 0x40000000, 0xc0000000),
            (0x40400000, 0x3f800000, 0x40400000, 0x00000000),
            (0x40400000, 0x40400000, 0x00000000, 0xc1100000),
            (0x7fffffff, 0x7fffffff, 0x00000000, 0xffffffff),
            (0x00000000, 0x00000000, 0x7fffffff, 0x7fffffff),
            (0x7fffffff, 0x7fffffff, 0x7fffffff, 0xffffffff),
            (0x7fffffff, 0x7fffffff, 0xffffffff, 0xffffffff),
            (0x7fffffff, 0xffffffff, 0x00000000, 0x7fffffff),
            (0x7fffffff, 0xffffffff, 0x7fffffff, 0x7fffffff),
            (0x7fffffff, 0xffffffff, 0xffffffff, 0x7fffffff),
            (0xffffffff, 0x7fffffff, 0x00000000, 0x7fffffff),
            (0xffffffff, 0xffffffff, 0x00000000, 0xffffffff),
            (0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff),
            (0x00000000, 0x00000000, 0xffffffff, 0xffffffff),
            (0x7fffffff, 0x00000000, 0x00000000, 0x00000000),
            (0x00000000, 0x7fffffff, 0x00000000, 0x00000000),
            (0x00000000, 0x7f800000, 0x00000000, 0x00000000),
            (0x7f800000, 0x7f800000, 0x00000000, 0xffffffff),
            (0x7f800000, 0x7f800000, 0x7f800000, 0xffffffff),
            (0xff800000, 0x7f800000, 0x00000000, 0x7fffffff),
            (0xff800000, 0x7f800000, 0xff800000, 0x7fffffff),
            (0x00000000, 0x00000000, 0xff800000, 0xff800000),
            (0x00000000, 0x00000000, 0x80000000, 0x80000000),
            (0x00000001, 0x00000001, 0x00000000, 0x00000000),
            (0x3f800000, 0x3f800000, 0x7f800001, 0x7f800001),
            (0x7f800001, 0x7f800001, 0x7f800001, 0xffffffff),
            (0x3f800000, 0xbf800000, 0x00000000, 0x3f800000),
            (0x3fffffff, 0x3f800000, 0x00000000, 0xbfffffff),
            (0x7f800001, 0x3f800000, 0x00000000, 0xff800001),
            (0x00000001, 0x3f800000, 0x00000000, 0x00000000),
            (0x3f800000, 0x3fffffff, 0x00000000, 0xbffffffe),
            (0x3f800000, 0x7f800001, 0x00000000, 0xff800001),
            (0x3f800000, 0x00000001, 0x00000000, 0x00000000),
            (0x3fffffff, 0x3fffffff, 0x00000000, 0xc07ffffe),
            (0x7f800001, 0x7f800001, 0x00000000, 0xffffffff),
            (0x7fffffff, 0x3f800000, 0x00000000, 0xffffffff),
            (0xffffffff, 0x3f800000, 0x00000000, 0x7fffffff),
            (0x3f800000, 0x7fffffff, 0x00000000, 0xfffffffe),
            (0x3f800000, 0xffffffff, 0x00000000, 0x7ffffffe),
            (0x00001337, 0xdeadbeef, 0x00000000, 0x00000000),
        ],
    }
    mac_cases = {}
    for mode, words in mac_hardware_words.items():
        mac_cases[mode] = [
            (*values, initial, None, "hardware result")
            for values in words
            for initial in (0, accumulator_normal_flags)
        ]
        subtract = mode in ("msub_s", "msuba_s")
        writes_acc = mode in ("madda_s", "msuba_s")
        opposite_max = 0x7fffffff if subtract else 0xffffffff
        product_max = 0xffffffff if subtract else 0x7fffffff
        mac_cases[mode].extend([
            (0x7fffffff, 0x3f800000, opposite_max, 0,
             0, 0, "finite exponent-255 cancellation", 0, 0),
            (0x7fffffff, 0x3f800000, opposite_max, opposite_max,
             0, 0x8010, "previous ACC overflow dominates finite product", 1, 1),
            (0x7fffffff, 0x40000000, opposite_max, product_max,
             0, 0x8010, "product overflow dominates opposite ACC", 0, int(writes_acc)),
            (0x7fffffff, 0x40000000, opposite_max, product_max,
             0x4008, 0x8018, "product overflow dominates latched ACC", 1, 1),
            (0x00800000, 0x3f000000, 0x3f800000, 0x3f800000,
             0, 8, "product underflow sets sticky only", 0, 0),
            (0x00800000 if subtract else 0x80800000, 0x3f800000,
             0x00800001, 0, 0, 0x4008, "final sum underflow", 0, 0),
            (0x3fc00000, 0x40000000, 0x3f800000,
             0xc0000000 if subtract else 0x40800000,
             accumulator_normal_flags, accumulator_no_exception,
             "ordinary MAC clears previous O/U cause", 0, 0),
        ])

    # PCSX2 EE reference semantics:
    # - ADD/SUB/MUL normalise zero/denormal and Inf/NaN operands first.
    # - Arithmetic overflow clamps to signed FMAX and sets O|SO.
    # - Multiplier underflow flushes to signed zero; adder cancellation
    #   retains the normalized fraction. Both set U|SU.
    # - DIV checks the raw divisor exponent and replaces the I/D causes.
    fpu_normal_flags = 0xA592F45E
    add_cases = [
        (0x3FC00000, 0x40100000, 0x40700000,
         fpu_normal_flags, fpu_normal_flags & ~0xC000, "normal addition"),
        (0x7F7FFFFF, 0x7F7FFFFF, 0x7FFFFFFF,
         fpu_normal_flags, fpu_normal_flags & ~0xC000, "exact extended maximum"),
        (0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF,
         0xA592744E, 0xA592B45E, "overflow replaces prior underflow cause"),
        (0x3F800000, 0x33800000, 0x3F800000,
         fpu_normal_flags, fpu_normal_flags & ~0xC000, "half-ULP truncation"),
        (0x3F800001, 0x33800000, 0x3F800001,
         fpu_normal_flags, fpu_normal_flags & ~0xC000, "odd significand truncation"),
        (0x00000001, 0x80000001, 0x00000000,
         fpu_normal_flags, fpu_normal_flags & ~0xC000, "denormal operands flush to signed zero"),
    ]
    sub_cases = [
        (0x3FC00000, 0x40100000, 0xBF400000,
         fpu_normal_flags, fpu_normal_flags & ~0xC000, "normal subtraction"),
        (0x3F800000, 0x33000000, 0x3F800000,
         fpu_normal_flags, fpu_normal_flags & ~0xC000, "exponent delta 25 discards operand"),
        (0x3F800000, 0x33800000, 0x3F7FFFFF,
         fpu_normal_flags, fpu_normal_flags & ~0xC000, "exponent delta 24 retains guard"),
        (0x00800001, 0x00800000, 0x00000000,
         0xA592B456, 0xA592745E, "cancellation underflow"),
        (0x00800000, 0x00800001, 0x80000000,
         0xA592B456, 0xA592745E, "negative cancellation underflow"),
    ]
    # Literal console result words; ADD/ADDA and SUB/SUBA have identical
    # corresponding fixtures in ps2autotests ee_fpu/arithmetic.expected.
    add_hardware_words = [
        (0x00000000, 0x00000000, 0x00000000),
        (0x00000000, 0x80000000, 0x00000000),
        (0x80000000, 0x00000000, 0x00000000),
        (0x80000000, 0x80000000, 0x80000000),
        (0x00000000, 0x3F800000, 0x3F800000),
        (0x3F800000, 0x3F800000, 0x40000000),
        (0x3F800000, 0x00000000, 0x3F800000),
        (0x40000000, 0x40000000, 0x40800000),
        (0x40400000, 0x3F800000, 0x40800000),
        (0x40400000, 0x40400000, 0x40C00000),
        (0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF),
        (0x7FFFFFFF, 0xFFFFFFFF, 0x00000000),
        (0xFFFFFFFF, 0x7FFFFFFF, 0x00000000),
        (0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF),
        (0x7FFFFFFF, 0x00000000, 0x7FFFFFFF),
        (0x00000000, 0x7FFFFFFF, 0x7FFFFFFF),
        (0x00000000, 0x7F800000, 0x7F800000),
        (0x7F800000, 0x7F800000, 0x7FFFFFFF),
        (0xFF800000, 0x7F800000, 0x00000000),
        (0x3F800000, 0xBF800000, 0x00000000),
        (0x3FFFFFFF, 0x3F800000, 0x403FFFFF),
        (0x7F800001, 0x3F800000, 0x7F800001),
        (0x00000001, 0x3F800000, 0x3F800000),
        (0x3F800000, 0x3FFFFFFF, 0x403FFFFF),
        (0x3F800000, 0x7F800001, 0x7F800001),
        (0x3F800000, 0x00000001, 0x3F800000),
        (0x7FFFFFFF, 0x3F800000, 0x7FFFFFFF),
        (0xFFFFFFFF, 0x3F800000, 0xFFFFFFFF),
        (0x3F800000, 0x7FFFFFFF, 0x7FFFFFFF),
        (0x3F800000, 0xFFFFFFFF, 0xFFFFFFFF),
        (0x00001337, 0xDEADBEEF, 0xDEADBEEF),
    ]
    sub_hardware_words = [
        (0x00000000, 0x00000000, 0x00000000),
        (0x00000000, 0x80000000, 0x00000000),
        (0x80000000, 0x00000000, 0x80000000),
        (0x80000000, 0x80000000, 0x00000000),
        (0x00000000, 0x3F800000, 0xBF800000),
        (0x3F800000, 0x3F800000, 0x00000000),
        (0x3F800000, 0x00000000, 0x3F800000),
        (0x40000000, 0x40000000, 0x00000000),
        (0x40400000, 0x3F800000, 0x40000000),
        (0x40400000, 0x40400000, 0x00000000),
        (0x7FFFFFFF, 0x7FFFFFFF, 0x00000000),
        (0x7FFFFFFF, 0xFFFFFFFF, 0x7FFFFFFF),
        (0xFFFFFFFF, 0x7FFFFFFF, 0xFFFFFFFF),
        (0xFFFFFFFF, 0xFFFFFFFF, 0x00000000),
        (0x7FFFFFFF, 0x00000000, 0x7FFFFFFF),
        (0x00000000, 0x7FFFFFFF, 0xFFFFFFFF),
        (0x00000000, 0x7F800000, 0xFF800000),
        (0x7F800000, 0x7F800000, 0x00000000),
        (0xFF800000, 0x7F800000, 0xFFFFFFFF),
        (0x3F800000, 0xBF800000, 0x40000000),
        (0x3FFFFFFF, 0x3F800000, 0x3F7FFFFE),
        (0x7F800001, 0x3F800000, 0x7F800001),
        (0x00000001, 0x3F800000, 0xBF800000),
        (0x3F800000, 0x3FFFFFFF, 0xBF7FFFFE),
        (0x3F800000, 0x7F800001, 0xFF800001),
        (0x3F800000, 0x00000001, 0x3F800000),
        (0x7FFFFFFF, 0x3F800000, 0x7FFFFFFF),
        (0xFFFFFFFF, 0x3F800000, 0xFFFFFFFF),
        (0x3F800000, 0x7FFFFFFF, 0xFFFFFFFF),
        (0x3F800000, 0xFFFFFFFF, 0x7FFFFFFF),
        (0x00001337, 0xDEADBEEF, 0x5EADBEEF),
    ]
    for operation, words, scalar_cases, accumulator_cases in (
        ("ADD", add_hardware_words, add_cases, adda_cases),
        ("SUB", sub_hardware_words, sub_cases, suba_cases),
    ):
        for lhs, rhs, expected in words:
            # The recorded overflow cases are same-sign exponent-255
            # sums after applying SUB's sign inversion; exact maximum plus
            # a tiny operand is not an overflow.
            effective_rhs = rhs ^ (0x80000000 if operation == "SUB" else 0)
            overflow = ((lhs & 0x7F800000) == 0x7F800000
                        and (effective_rhs & 0x7F800000) == 0x7F800000
                        and ((lhs ^ effective_rhs) & 0x80000000) == 0)
            for initial in (0xA5903456, fpu_normal_flags):
                flags = (initial & ~0xC000) | (0x8010 if overflow else 0)
                scalar_cases.append((lhs, rhs, expected, initial, flags,
                                     "hardware " + operation))
                accumulator_cases.append((lhs, rhs, 0xAABBCCDD, expected,
                                          initial, flags, "hardware " + operation + "A"))

    # Physical SCPH-900xx cancellation traces from PCSX2 PR #12001.
    # The adder retains its normalized fraction on exponent underflow,
    # unlike the multiplier. The flag trace reports U|SU.
    for scalar_cases, accumulator_cases, words in (
        (add_cases, adda_cases, [
            (0x8080002C, 0x008000EC, 0x00400000),
            (0x80800036, 0x00829F6A, 0x0027CD00),
            (0x80C1AA65, 0x01200000, 0x007CAB36),
        ]),
        (sub_cases, suba_cases, [
            (0x0080008B, 0x008000EA, 0x803E0000),
        ]),
    ):
        for lhs, rhs, expected in words:
            scalar_cases.append((lhs, rhs, expected, 0x01000001,
                                 0x01004009, "hardware cancellation fraction"))
            accumulator_cases.append((lhs, rhs, 0, expected, 0x01000001,
                                      0x01004009, "adder fraction model for ACC"))

    # The same adder is used for the final MAC stage.
    mac_cases["madd_s"].extend([
        (0x00800000, 0x3F000000, 0x01000000, 0x01000000,
         0x01000001, 0x01000009, "hardware product underflow sticky only", 0, 0),
        (0x7FFFFFFF, 0xBF800000, 0x7FFFFFFF, 0x7FFFFFFF,
         0x01000001, 0x01008011, "hardware latched overflow result", 1, 1),
        (0x008000EC, 0x3F800000, 0x8080002C, 0x00400000,
         0x01000001, 0x01004009, "MAC cancellation fraction model", 0, 0),
    ])

    mul_cases = [
        (0x3FC00000, 0x40000000, 0x40400000,
         fpu_normal_flags, fpu_normal_flags & ~0xC000, "normal multiplication"),
        (0x00800000, 0x3F000000, 0x00000000,
         0xA592B456, 0xA592745E, "positive subnormal result flushes and sets U|SU"),
        (0x80800000, 0x3F000000, 0x80000000,
         0xA592B456, 0xA592745E, "negative subnormal result flushes and sets U|SU"),
        (0x7F7FFFFF, 0x40000000, 0x7FFFFFFF,
         fpu_normal_flags, fpu_normal_flags & ~0xC000, "exact extended maximum"),
        (0x7FFFFFFF, 0x40000000, 0x7FFFFFFF,
         0xA592744E, 0xA592B45E, "multiplication overflow replaces prior underflow cause"),
        (0x3FFFFFFF, 0x3FFFFFFF, 0x407FFFFE,
         fpu_normal_flags, fpu_normal_flags & ~0xC000, "truncated full significand product"),
    ]
    # Distinct physical-console MUL/MULA result words from muldiv.expected.
    mul_hardware_words = [
        (0x00000000, 0x00000000, 0x00000000),
        (0x00000000, 0x80000000, 0x80000000),
        (0x80000000, 0x00000000, 0x80000000),
        (0x80000000, 0x80000000, 0x00000000),
        (0x00000000, 0x3F800000, 0x00000000),
        (0x3F800000, 0x3F800000, 0x3F800000),
        (0x3F800000, 0x00000000, 0x00000000),
        (0x40000000, 0x40000000, 0x40800000),
        (0x40400000, 0x3F800000, 0x40400000),
        (0x40400000, 0x40400000, 0x41100000),
        (0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF),
        (0x7FFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF),
        (0xFFFFFFFF, 0x7FFFFFFF, 0xFFFFFFFF),
        (0xFFFFFFFF, 0xFFFFFFFF, 0x7FFFFFFF),
        (0x7FFFFFFF, 0x00000000, 0x00000000),
        (0x00000000, 0x7FFFFFFF, 0x00000000),
        (0x00000000, 0x7F800000, 0x00000000),
        (0x7F800000, 0x7F800000, 0x7FFFFFFF),
        (0xFF800000, 0x7F800000, 0xFFFFFFFF),
        (0x3F800000, 0xBF800000, 0xBF800000),
        (0x3FFFFFFF, 0x3F800000, 0x3FFFFFFF),
        (0x7F800001, 0x3F800000, 0x7F800001),
        (0x00000001, 0x3F800000, 0x00000000),
        (0x3F800000, 0x3FFFFFFF, 0x3FFFFFFE),
        (0x3F800000, 0x7F800001, 0x7F800001),
        (0x3F800000, 0x00000001, 0x00000000),
        (0x7FFFFFFF, 0x3F800000, 0x7FFFFFFF),
        (0xFFFFFFFF, 0x3F800000, 0xFFFFFFFF),
        (0x3F800000, 0x7FFFFFFF, 0x7FFFFFFE),
        (0x3F800000, 0xFFFFFFFF, 0xFFFFFFFE),
        (0x00001337, 0xDEADBEEF, 0x80000000),
    ]
    for lhs, rhs, expected in mul_hardware_words:
        overflow = ((lhs & 0x7F800000) == 0x7F800000
                    and (rhs & 0x7F800000) == 0x7F800000)
        for initial in (0xA5903456, fpu_normal_flags):
            flags = (initial & ~0xC000) | (0x8010 if overflow else 0)
            mul_cases.append((lhs, rhs, expected, initial, flags, "hardware MUL"))
            mula_cases.append((lhs, rhs, 0xAABBCCDD, expected, initial, flags,
                               "hardware MULA"))
    for lhs, rhs, expected, initial, flags, label in mul_cases[:6]:
        mula_cases.append((lhs, rhs, 0xAABBCCDD, expected, initial, flags,
                           "accumulator " + label))
    div_cases = [
        (0x40400000, 0x40000000, 0x3FC00000,
         0xA592F45E, 0xA590F45E, "normal division clears I/D causes"),
        (0x3F800000, 0x00000000, 0x7FFFFFFF,
         0xA5903456, 0xA5913476, "positive divide by zero sets D|SD"),
        (0x00000000, 0x00000000, 0x7FFFFFFF,
         0xA5903456, 0xA5923456, "zero divided by zero sets I|SI"),
        (0xBF800000, 0x00000000, 0xFFFFFFFF,
         0xA5903456, 0xA5913476, "negative divide by zero returns negative FMAX"),
        (0x3F800000, 0x00000001, 0x7FFFFFFF,
         0xA5903456, 0xA5913476, "denormal divisor counts as zero"),
        (0x80000000, 0x00000000, 0xFFFFFFFF,
         0xA5903456, 0xA5923456, "negative zero over positive zero retains sign"),
        (0x00000000, 0x80000000, 0xFFFFFFFF,
         0xA5903456, 0xA5923456, "positive zero over negative zero retains sign"),
        (0x00800000, 0x40000000, 0x00000000,
         0xA592F45E, 0xA590F45E, "division underflow clears I/D, preserves O/U"),
        (0x7F7FFFFF, 0x00800000, 0x7FFFFFFF,
         0xA592F45E, 0xA590F45E, "division overflow clears I/D, preserves O/U"),
        (0x7F800000, 0x40000000, 0x7F000000,
         0xA592F45E, 0xA590F45E, "extended operand divided by two"),
    ]

    # SCPH-900xx console flags: range errors from the divide unit
    # saturate/flush the result without setting the FMAC O/U flags.
    div_cases.extend([
        (0x3F800000, 0, 0x7FFFFFFF,
         0x01020041, 0x01010061, "D replaces I while SI survives"),
        (0, 0, 0x7FFFFFFF,
         0x01010021, 0x01020061, "I replaces D while SD survives"),
        (0x3F800000, 0x3F800000, 0x3F800000,
         0x01030061, 0x01000061, "successful divide clears both causes"),
        (0x00800000, 0x40000000, 0x00000000,
         0x01000001, 0x01000001, "hardware division underflow flag"),
        (0x5AE20690, 0x00800044, 0x7FFFFFFF,
         0x01000001, 0x01000001, "hardware division overflow flag"),
    ])

    # Unique physical-console results from ps2autotests
    # tests/cpu/ee_fpu/muldiv.expected (expanded CF_* operands).
    div_hardware_words = [
        (0x00000000, 0x00000000, 0x7FFFFFFF),
        (0x00000000, 0x80000000, 0xFFFFFFFF),
        (0x80000000, 0x00000000, 0xFFFFFFFF),
        (0x80000000, 0x80000000, 0x7FFFFFFF),
        (0x00000000, 0x3F800000, 0x00000000),
        (0x3F800000, 0x3F800000, 0x3F800000),
        (0x3F800000, 0x00000000, 0x7FFFFFFF),
        (0x40000000, 0x40000000, 0x3F800000),
        (0x40400000, 0x3F800000, 0x40400000),
        (0x40400000, 0x40400000, 0x3F800000),
        (0x7FFFFFFF, 0x7FFFFFFF, 0x3F800000),
        (0x7FFFFFFF, 0xFFFFFFFF, 0xBF800000),
        (0xFFFFFFFF, 0x7FFFFFFF, 0xBF800000),
        (0xFFFFFFFF, 0xFFFFFFFF, 0x3F800000),
        (0x7FFFFFFF, 0x00000000, 0x7FFFFFFF),
        (0x00000000, 0x7FFFFFFF, 0x00000000),
        (0x00000000, 0x7F800000, 0x00000000),
        (0x7F800000, 0x7F800000, 0x3F800000),
        (0xFF800000, 0x7F800000, 0xBF800000),
        (0x3F800000, 0xBF800000, 0xBF800000),
        (0x3FFFFFFF, 0x3F800000, 0x3FFFFFFF),
        (0x7F800001, 0x3F800000, 0x7F800001),
        (0x00000001, 0x3F800000, 0x00000000),
        (0x3F800000, 0x3FFFFFFF, 0x3F000001),
        (0x3F800000, 0x7F800001, 0x00000000),
        (0x3F800000, 0x00000001, 0x7FFFFFFF),
        (0x7FFFFFFF, 0x3F800000, 0x7FFFFFFF),
        (0xFFFFFFFF, 0x3F800000, 0xFFFFFFFF),
        (0x3F800000, 0x7FFFFFFF, 0x00000000),
        (0x3F800000, 0xFFFFFFFF, 0x80000000),
        (0x00001337, 0xDEADBEEF, 0x80000000),
    ]
    # Check both clear and previously populated causes/stickies. The
    # result words are from hardware; I/D clearing follows the EE manual.
    for lhs, rhs, expected in div_hardware_words:
        for initial in (0xA5903456, 0xA593F47E):
            flags = initial & ~0x30000
            if (rhs & 0x7F800000) == 0:
                flags |= (0x00020040 if (lhs & 0x7F800000) == 0
                          else 0x00010020)
            div_cases.append((lhs, rhs, expected, initial, flags,
                              "hardware DIV.S result"))

    sqrt_words = [
        (0x40800000, "positive four"),
        (0xC0800000, "negative four"),
        (0x80000000, "negative zero"),
        (0x00000001, "positive denormal"),
        (0x80000001, "negative denormal"),
        (0x7F800000, "positive extended exponent"),
        (0xFF800000, "negative infinity is invalid"),
        (0x7FC12345, "positive extended mantissa"),
        (0xFFC12345, "negative NaN is invalid"),
        (0x0080EFD3, "minimum exponent root model"),
        (0x3F80EFD3, "non-nearest root model"),
        (0x4000EFD3, "even exponent root model"),
        (0x7F80EFD3, "extended exponent non-nearest root model"),
        (0xBF80EFD3, "negative non-nearest root model"),
    ]
    # Literal result words from the physical-console ps2autotests
    # tests/cpu/ee_fpu/sqrt.expected, including expanded CF_* constants.
    rsqrt_pairs = [
        (0x00000000, 0x00000000, 0x7FFFFFFF, "positive zero / zero"),
        (0x00000000, 0x80000000, 0x7FFFFFFF, "positive zero / negative zero"),
        (0x80000000, 0x00000000, 0xFFFFFFFF, "negative zero / zero"),
        (0x80000000, 0x80000000, 0xFFFFFFFF, "negative zero / negative zero"),
        (0x00000000, 0x3F800000, 0x00000000, "zero / one"),
        (0x3F800000, 0x3F800000, 0x3F800000, "one / one"),
        (0x3F800000, 0x00000000, 0x7FFFFFFF, "one / zero"),
        (0x40000000, 0x40000000, 0x3FB504F3, "two / sqrt two"),
        (0x40400000, 0x3F800000, 0x40400000, "three / one"),
        (0x40400000, 0x40400000, 0x3FDDB3D7, "three / sqrt three"),
        (0x7FFFFFFF, 0x7FFFFFFF, 0x5FB504F2, "maximum / sqrt maximum"),
        (0x7FFFFFFF, 0xFFFFFFFF, 0x5FB504F2, "maximum / negative maximum"),
        (0xFFFFFFFF, 0x7FFFFFFF, 0xDFB504F2, "negative maximum / maximum"),
        (0xFFFFFFFF, 0xFFFFFFFF, 0xDFB504F2, "negative maximum / negative maximum"),
        (0x7FFFFFFF, 0x00000000, 0x7FFFFFFF, "maximum / zero"),
        (0x00000000, 0x7FFFFFFF, 0x00000000, "zero / maximum"),
        (0x00000000, 0x7F800000, 0x00000000, "zero / extended"),
        (0x7F800000, 0x7F800000, 0x5F800000, "extended / extended"),
        (0xFF800000, 0x7F800000, 0xDF800000, "negative extended / extended"),
        (0x3F800000, 0x41C80000, 0x3E4CCCCD, "one / sqrt twenty-five"),
        (0x40A00000, 0x41C80000, 0x3F800000, "five / sqrt twenty-five"),
        (0x41C80000, 0x41C80000, 0x40A00000, "twenty-five / sqrt twenty-five"),
        (0x3F800000, 0xBF800000, 0x3F800000, "one / negative one"),
        (0x3FFFFFFF, 0x3F800000, 0x3FFFFFFF, "max mantissa / one"),
        (0x7F800001, 0x3F800000, 0x7F800001, "extended mantissa / one"),
        (0x00000001, 0x3F800000, 0x00000000, "denormal / one"),
        (0x3F800000, 0x3FFFFFFF, 0x3F3504F3, "one / max mantissa"),
        (0x3F800000, 0x7F800001, 0x1F800000, "one / extended mantissa"),
        (0x3F800000, 0x00000001, 0x7FFFFFFF, "one / denormal"),
        (0x7FFFFFFF, 0x3F800000, 0x7FFFFFFF, "maximum / one"),
        (0xFFFFFFFF, 0x3F800000, 0xFFFFFFFF, "negative maximum / one"),
        (0x3F800000, 0x7FFFFFFF, 0x1F3504F3, "one / maximum"),
        (0x3F800000, 0xFFFFFFFF, 0x1F3504F3, "one / negative maximum"),
        (0x00001337, 0xDEADBEEF, 0x00000000, "garbage words"),
        # Additional architectural controls; no new hardware trace claimed.
        (0x40C00000, 0x40800000, 0x40400000, "six / sqrt four"),
        (0x40C00000, 0xC0800000, 0x40400000, "negative radicand"),
        (0x40C00000, 0x00000000, 0x7FFFFFFF, "positive zero divisor"),
        (0x40C00000, 0x80000000, 0x7FFFFFFF, "negative zero divisor"),
        (0xC0C00000, 0x00000000, 0xFFFFFFFF, "negative numerator / zero"),
        (0xC0C00000, 0x80000000, 0xFFFFFFFF, "negative numerator / negative zero"),
        (0x80000001, 0x00000001, 0xFFFFFFFF, "denormal / denormal"),
        (0x40C00000, 0x00000001, 0x7FFFFFFF, "positive denormal divisor"),
        (0x40C00000, 0x80000001, 0x7FFFFFFF, "negative denormal divisor"),
        (0x7F7FFFFF, 0x00800000, 0x7FFFFFFF, "overflow saturates to R5900 max"),
        (0x00800000, 0x7F7FFFFF, 0x00000000, "underflow flushes"),
        (0xC0C00000, 0x40800000, 0xC0400000, "negative numerator"),
        (0x40C00000, 0xFF800000, 0x20C00000, "negative extended radicand"),
    ]

    sign_operation_words = [
        (0xBF800000, "negative one"),
        (0x80000000, "negative zero"),
        (0xFF800000, "negative infinity"),
        (0xFFC00000, "negative quiet NaN"),
        (0x7FC12345, "positive NaN payload"),
        (0xFFA54321, "negative NaN payload"),
        (0x80000001, "negative denormal"),
        (0x00000000, "positive zero"),
    ]

    extreme_pairs = [
        (0x3F800000, 0x40000000, "positive finite"),
        (0xBF800000, 0xC0000000, "both negative"),
        (0x00000000, 0x80000000, "positive and negative zero"),
        (0x7F800000, 0x7F7FFFFF, "positive infinity and FMAX"),
        (0xFF800000, 0xFF7FFFFF, "negative infinity and negative FMAX"),
        (0x7FC00000, 0x3F800000, "positive NaN and positive finite"),
        (0xFFC00000, 0xBF800000, "negative NaN and negative finite"),
        (0x7FC00000, 0xFFC00000, "opposite-sign NaNs"),
        (0x80000001, 0x00000001, "opposite-sign denormals"),
        (0x7F800000, 0xFF800000, "opposite infinities"),
        (0xFFFFFFFF, 0x80000000, "negative NaN and negative zero"),
        (0x00000000, 0x7FC00000, "positive zero and positive NaN"),
    ]

    with tempfile.TemporaryDirectory(prefix="ee-fpu-compare-") as directory:
        temp_dir = pathlib.Path(directory)
        check_matrix_chain(fixture, temp_dir, target)
        for opt in (0, 3):
            for mode, lhs, rhs, expected in cases:
                run_case(fixture, temp_dir, target, mode, lhs, rhs, expected, opt)
        for source_bits, expected_bits, label in cvt_cases:
            run_cvt_w_case(
                fixture, temp_dir, target, source_bits, expected_bits, label
            )
        # Explicit raw results rather than a host float cast oracle.
        # Signed maximum and the positive/negative non-exact conversions
        # distinguish truncation from nearest-even.
        for opt in (0, 3):
            for source_value, expected_bits, label in [
                (0, 0x00000000, "zero"),
                (1, 0x3f800000, "positive one"),
                (-1, 0xbf800000, "negative one"),
                (16777217, 0x4b800000, "positive truncation tie"),
                (16777219, 0x4b800001, "positive round-up tie"),
                (-16777219, 0xcb800001, "negative round-away tie"),
                (2147483647, 0x4effffff, "signed maximum"),
                (-2147483647, 0xceffffff, "near signed minimum"),
                (-2147483648, 0xcf000000, "signed minimum"),
                (0x3fbeb852, 0x4e7efae1, "hardware conversion vector"),
            ]:
                run_cvt_s_w_case(
                    fixture, temp_dir, target, source_value, expected_bits, label, opt
                )
        for opt in (0, 3):
            run_arithmetic_cases(fixture, temp_dir, target, "add_s", add_cases, opt)
            run_arithmetic_cases(fixture, temp_dir, target, "sub_s", sub_cases, opt)
        verify_add_identity_fast_path(fixture, temp_dir, target)
        verify_mul_correction_fast_path(fixture, temp_dir, target)
        for opt in (0, 3):
            run_arithmetic_cases(fixture, temp_dir, target, "mul_s", mul_cases, opt)
        for opt in (0, 3):
            run_arithmetic_cases(fixture, temp_dir, target, "div_s", div_cases, opt)
        run_cop1_transfer_cases(fixture, temp_dir, target)
        for opt in (0, 3):
            run_hardware_fcr_control_cases(fixture, temp_dir, target, opt)
            run_hardware_fcr_arithmetic_cases(fixture, temp_dir, target, opt)
        run_bc1_delay_cases(fixture, temp_dir, target)
        for opt in (0, 3):
            run_accumulator_cases(fixture, temp_dir, target, "adda_s", adda_cases, opt)
            run_accumulator_cases(fixture, temp_dir, target, "suba_s", suba_cases, opt)
        for opt in (0, 3):
            run_accumulator_cases(fixture, temp_dir, target, "mula_s", mula_cases, opt)
        for opt in (0, 3):
            for mode, cases in mac_cases.items():
                run_accumulator_cases(fixture, temp_dir, target, mode, cases, opt)
        sqrt_hardware_words = [
            (word, "hardware SQRT.S vector")
            for word in (0x00000000, 0x80000000, 0x3F800000, 0x40000000,
                         0x40400000, 0x41C80000, 0x7FFFFFFF, 0xFFFFFFFF,
                         0x7F800000, 0xFF800000, 0x3FFFFFFF, 0xDEADBEEF,
                         0x3FC00000, 0x7FC00000)
        ]
        for opt in (0, 3):
            run_sqrt_cases(fixture, temp_dir, target,
                           sqrt_words + sqrt_hardware_words, opt)
        for opt in (0, 3):
            run_rsqrt_cases(fixture, temp_dir, target, rsqrt_pairs, opt)
        run_max_min_cases(fixture, temp_dir, target, "max_s", extreme_pairs)
        run_max_min_cases(fixture, temp_dir, target, "min_s", extreme_pairs)
        run_sign_operation_cases(
            fixture, temp_dir, target, "abs_s", sign_operation_words
        )
        run_sign_operation_cases(
            fixture, temp_dir, target, "neg_s", sign_operation_words
        )

    print(
        "fpu_compare_ir_runtime: PASS (C.F/C.EQ/C.LT/C.LE; CVT.W.S/CVT.S.W; "
        "MFC1/MTC1/CFC1/CTC1 transfer/liveness; BC1 condition timing and "
        "branch-likely annulment; "
        "ADD.S/SUB.S/MUL.S/DIV.S, SQRT.S/RSQRT.S and accumulator arithmetic; "
        "FCSR cause/sticky semantics; "
        "MAX.S/MIN.S bitwise ordering; ABS.S/NEG.S raw-bit sign operations; "
        "EE NaN/Inf normalisation, integer "
        "saturation, truncation, FCSR cause-bit clearing and sticky-bit "
        "preservation; O0 JIT, comparisons/CVT.S.W also O3 codegen)"
    )


if __name__ == "__main__":
    main()
