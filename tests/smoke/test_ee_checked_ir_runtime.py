#!/usr/bin/env python3
"""Execute signed EX_Ov and conditional EX_Tr with the real EE COP0 helper."""

import ctypes
import pathlib
import re
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs
from test_vu0_transfer_ir_runtime import EEState, U128, aligned_instance, require


ENTRY_PC = 0x00150000
MASK64 = (1 << 64) - 1
EXCEPTION = 1
RETURN = 0
INITIAL_RD = 0x0123456789ABCDEF
INITIAL_HI = 0xFEDCBA9876543210
INITIAL_EPC = 0x00ABCDEF
INITIAL_CAUSE = 0x00004000
INITIAL_BADVADDR = 0x11223344

# (mnemonic, normal operands, normal result, overflowing operands)
ARITHMETIC = [
    ("add", (2, 3), 5, (0x7FFFFFFF, 1)),
    ("sub", (2, 3), MASK64, (0x80000000, 1)),
    ("dadd", (MASK64, 2), 1, (0x7FFFFFFFFFFFFFFF, 1)),
    ("dsub", (0x100000000, 1), 0xFFFFFFFF, (0x8000000000000000, 1)),
    ("addi", (41, 0), 42, (0x7FFFFFFF, 0)),
    ("daddi", (0x100000000, 0), 0x100000001, (0x7FFFFFFFFFFFFFFF, 0)),
]

# (mnemonic, false-condition operands, true-condition operands).
# The immediate forms compare against sign-extended -1, even when unsigned.
TRAPS = [
    ("tge", (MASK64, 0), (1, 0)),
    ("tgeu", (0, 1), (MASK64, 1)),
    ("tlt", (1, 0), (MASK64, 0)),
    ("tltu", (MASK64, 1), (0, 1)),
    ("teq", (0x100000001, 1), (0x100000001, 0x100000001)),
    ("tne", (7, 7), (7, 8)),
    ("tgei", (MASK64 - 1, 0), (0, 0)),
    ("tgeiu", (0, 0), (MASK64, 0)),
    ("tlti", (0, 0), (MASK64 - 1, 0)),
    ("tltiu", (MASK64, 0), (0, 0)),
    ("teqi", (0, 0), (MASK64, 0)),
    ("tnei", (MASK64, 0), (0, 0)),
]


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
from verify_ir import optimize_guest_ir


def compile_guest(fixture, directory, target, mode, opt_level, pipeline=False):
    path = directory / f"{mode}_o{opt_level}.ll"
    subprocess.run([str(fixture), str(path), mode, str(opt_level)], check=True)
    ir_text = path.read_text(encoding="utf-8")
    if not mode.endswith("_delay"):
        guest_ir = ir_text.split("define i32 @guest_checked_exception_test(", 1)[1]
        fault = re.search(
            r"(?ms)^checked_fault_\d+:\n(.*?)^checked_continue_\d+:",
            guest_ir,
        )
        require(fault is not None, f"{mode}: missing checked exception edge")
        require("store i64 " not in guest_ir[:fault.start()],
                f"{mode}: successful checked path prematurely spills GPR state")
        require("store i64 " in fault.group(1) and
                "@ee_raise_exception" in fault.group(1),
                f"{mode}: fault edge must commit prior GPR state")
    module = llvm.parse_assembly(ir_text)
    module.verify()
    machine = target.create_target_machine(opt=2 if pipeline else 0)
    module.triple = llvm.get_default_triple()
    module.data_layout = str(machine.target_data)
    module.verify()
    if pipeline:
        optimize_guest_ir(module, machine)
    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), machine)
    engine.add_module(module)
    engine.finalize_object()
    address = engine.get_function_address("guest_checked_exception_test")
    require(address != 0, f"{mode}: missing native guest entry")
    function_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    return function_type(address), engine, module, machine


def execute(function, operands, delayed=False, exl=False, bev=False):
    cpu_backing, cpu = aligned_instance(EEState)
    runtime_backing = ctypes.create_string_buffer(0x10000)
    cpu.gpr[4] = U128(operands[0] & MASK64, 0xAAAAAAAABBBBBBBB)
    cpu.gpr[5] = U128(operands[1] & MASK64, 0xCCCCCCCCDDDDDDDD)
    cpu.gpr[6] = U128(INITIAL_RD, INITIAL_HI)
    cpu.gpr[10] = U128(0, 0x1234123412341234)
    cpu.gpr[31] = U128(0x00300000, 0)
    cpu.hi = 0xDEADBEEFDEADBEEF
    cpu.lo = 0xA5A5A5A5A5A5A5A5
    cpu.cop0[8] = INITIAL_BADVADDR
    cpu.cop0[12] = (2 if exl else 0) | (0x400000 if bev else 0)
    cpu.cop0[13] = INITIAL_CAUSE
    cpu.cop0[14] = INITIAL_EPC
    result = function(
        ctypes.pointer(cpu),
        ctypes.cast(runtime_backing, ctypes.c_void_p),
    )
    _ = cpu_backing, runtime_backing
    return result, cpu


def check(mode, function, operands, expected_result, exception_code,
          delayed, fault, exl=False, bev=False):
    result, cpu = execute(function, operands, delayed=delayed, exl=exl, bev=bev)
    where = f"{mode} {'delay' if delayed else 'plain'} {'fault' if fault else 'pass'}"
    if exl:
        where += " nested"
    if bev:
        where += " bev"

    require(cpu.gpr[10].lo == 0x1357 and
            cpu.gpr[10].hi == 0x1234123412341234,
            where + ": preceding ADDIU was not architecturally committed")
    require(cpu.gpr[4].lo == (operands[0] & MASK64) and
            cpu.gpr[5].lo == (operands[1] & MASK64) and
            cpu.gpr[4].hi == 0xAAAAAAAABBBBBBBB and
            cpu.gpr[5].hi == 0xCCCCCCCCDDDDDDDD,
            where + ": instruction changed source GPRs")
    require(cpu.gpr[6].hi == INITIAL_HI and
            cpu.hi == 0xDEADBEEFDEADBEEF and
            cpu.lo == 0xA5A5A5A5A5A5A5A5,
            where + ": upper GPR half or HI/LO were clobbered")
    require(cpu.cop0[8] == INITIAL_BADVADDR,
            where + ": non-address exception must preserve BadVAddr")
    if fault:
        trapping_pc = ENTRY_PC + (8 if delayed else 4)
        branch_pc = ENTRY_PC + 4
        require(result == EXCEPTION and cpu.exit_reason == EXCEPTION,
                where + ": expected native exception exit")
        require(cpu.pc == (0xBFC00380 if bev else 0x80000180),
                where + f": wrong exception vector 0x{cpu.pc:08x}")
        require(cpu.pad == trapping_pc,
                where + ": actual fault PC lost")
        require(cpu.cop0[14] == (INITIAL_EPC if exl else
                                branch_pc if delayed else trapping_pc),
                where + f": wrong EPC 0x{cpu.cop0[14]:08x}")
        require((cpu.cop0[13] & 0x80000000) ==
                (0x80000000 if delayed else 0),
                where + ": Cause.BD mismatch")
        require(((cpu.cop0[13] >> 2) & 31) == exception_code and
                (cpu.cop0[13] & 0xFF00) == INITIAL_CAUSE and
                (cpu.cop0[12] & 2) != 0,
                where + ": wrong EX_Ov/EX_Tr, IP mask or EXL")
        require(cpu.gpr[6].lo == INITIAL_RD,
                where + ": trapping operation wrote destination register")
    else:
        require(result == RETURN and cpu.exit_reason == RETURN and
                cpu.pc == 0x00300000,
                where + ": false condition must execute JR ra")
        require(cpu.cop0[13] == INITIAL_CAUSE and
                cpu.cop0[14] == INITIAL_EPC and
                cpu.cop0[12] == ((2 if exl else 0) |
                                 (0x400000 if bev else 0)),
                where + ": non-trap modified COP0 exception state")
        require(cpu.gpr[6].lo == expected_result,
                where + f": wrong arithmetic/untouched value 0x{cpu.gpr[6].lo:x}")


def main():
    require(len(sys.argv) == 3,
            "usage: test_ee_checked_ir_runtime.py fixture exception_library")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    helper = ctypes.CDLL(str(pathlib.Path(sys.argv[2]).resolve()))
    llvm.add_symbol(
        "ee_raise_exception",
        ctypes.cast(helper.ee_raise_exception, ctypes.c_void_p).value,
    )
    register_ee_diagnostic_stubs()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()

    executed = 0
    with tempfile.TemporaryDirectory(prefix="ee-checked-exception-") as tmp:
        directory = pathlib.Path(tmp)
        for opt_level, pipeline in ((0, False), (3, False), (0, True)):
            for delayed in (False, True):
                suffix = "_delay" if delayed else ""
                for mode, normal, wanted, overflow in ARITHMETIC:
                    function, engine, module, machine = compile_guest(
                        fixture, directory, target, mode + suffix, opt_level, pipeline
                    )
                    check(mode, function, normal, wanted, 12, delayed, False)
                    check(mode, function, overflow, INITIAL_RD, 12, delayed, True)
                    executed += 2
                    _ = engine, module, machine

                for mode, false_inputs, true_inputs in TRAPS:
                    function, engine, module, machine = compile_guest(
                        fixture, directory, target, mode + suffix, opt_level, pipeline
                    )
                    check(mode, function, false_inputs, INITIAL_RD, 13, delayed, False)
                    check(mode, function, true_inputs, INITIAL_RD, 13, delayed, True)
                    executed += 2
                    if mode in ("teq", "teqi"):
                        check(mode, function, true_inputs, INITIAL_RD, 13,
                              delayed, True, exl=True)
                        check(mode, function, true_inputs, INITIAL_RD, 13,
                              delayed, True, bev=True)
                        executed += 2
                    _ = engine, module, machine

    _ = helper
    print(f"ee_checked_ir_runtime: PASS ({executed} JIT executions; "
          "O0/O3/O2-pipeline, EX_Ov/EX_Tr, register/immediate, BD/EPC, EXL/BEV)")


if __name__ == "__main__":
    main()
