#!/usr/bin/env python3
"""Execute native LLVM LQC2/SQC2 with real EE exception entry semantics."""

import ctypes
import pathlib
import sys
import tempfile

import llvmlite.binding as llvm

from test_vu0_transfer_ir_runtime import (
    EEState, U128, ENTRY_PC, compile_jit, aligned_instance, require,
    register_trace_symbols, set_store_trace_enabled, RUNTIME_BACKING_BYTES,
    UNEXPECTED_TRACES,
)

EE_EXIT_EXCEPTION = 1
EE_EXIT_DISPATCH = 3

# The memory bus is a deterministic address-backed callback for this MCJIT
# fixture. Exception entry itself uses the REAL C++ ee_raise_exception
# helper from the compiled shared library.
MEMORY = {}
READS = []
WRITES = []
TRACES = []

READ64 = ctypes.CFUNCTYPE(
    ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint32
)
WRITE64 = ctypes.CFUNCTYPE(
    None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64
)
TRACE_STORE = ctypes.CFUNCTYPE(
    None, ctypes.c_void_p,
    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64,
    ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64,
    ctypes.c_uint64, ctypes.c_uint64
)


@READ64
def memory_read64(_rt, addr):
    READS.append(int(addr))
    return MEMORY.get(int(addr), 0)


@WRITE64
def memory_write64(_rt, addr, value):
    WRITES.append((int(addr), int(value)))
    MEMORY[int(addr)] = int(value)


@TRACE_STORE
def trace_guest_store(_rt, pc, word, address, aligned, size,
                      value, slot, old0, old1, old2, old3):
    TRACES.append((int(pc), int(address), int(size), int(slot)))


def register_callbacks(library_path):
    library = ctypes.CDLL(str(library_path))
    exception = library.ee_raise_exception
    llvm.add_symbol(
        "ee_raise_exception",
        ctypes.cast(exception, ctypes.c_void_p).value
    )
    llvm.add_symbol(
        "ee_mem_read64",
        ctypes.cast(memory_read64, ctypes.c_void_p).value
    )
    llvm.add_symbol(
        "ee_mem_write64",
        ctypes.cast(memory_write64, ctypes.c_void_p).value
    )
    register_trace_symbols(trace_guest_store)
    return library


def execute(fixture, directory, target, mode, address, trace_enabled=True):
    MEMORY.clear()
    READS.clear()
    WRITES.clear()
    TRACES.clear()
    MEMORY[0x100] = 0x0123456789abcdef
    MEMORY[0x108] = 0xfedcba9876543210

    guest, engine, module, machine, offset = compile_jit(
        fixture, directory, mode, target
    )
    require(offset + 8 < 0x4000, "runtime counter outside JIT backing")
    cpu_backing, cpu = aligned_instance(EEState)
    runtime_backing = ctypes.create_string_buffer(RUNTIME_BACKING_BYTES)
    set_store_trace_enabled(runtime_backing, module, trace_enabled)
    UNEXPECTED_TRACES.clear()
    cpu.gpr[9] = U128(address, 0)
    cpu.gpr[31] = U128(0x00140000, 0)
    cpu.vu0[0] = U128(0xaabbccddeeff0011, 0x1122334455667788)
    cpu.vu0[6] = U128(0x9999999988888888, 0x7777777766666666)
    cpu.cop0[12] = 0

    ret = guest(
        ctypes.pointer(cpu),
        ctypes.cast(runtime_backing, ctypes.c_void_p)
    )
    require(not UNEXPECTED_TRACES, f"disabled trace called: {UNEXPECTED_TRACES}")
    # Hold all JIT-managed buffers and callback objects through execution.
    _ = cpu_backing, runtime_backing, engine, module, machine
    return ret, cpu


def main():
    require(len(sys.argv) == 3,
            "usage: test_vu0_quadword_ir_runtime.py fixture exception_library")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    exception_lib = register_callbacks(pathlib.Path(sys.argv[2]).resolve())
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()

    with tempfile.TemporaryDirectory(prefix="ee-vu0-quadword-") as tmp:
        directory = pathlib.Path(tmp)

        result, cpu = execute(
            fixture, directory, target, "lqc2_vf6", 0x100
        )
        require(result == EE_EXIT_DISPATCH,
                "LQC2 valid aligned transfer must dispatch")
        require(cpu.vu0[6].lo == 0x0123456789abcdef and
                cpu.vu0[6].hi == 0xfedcba9876543210,
                "LQC2 must read full 128-bit vector")
        require(READS == [0x100, 0x108], "LQC2 read order")

        result, cpu = execute(
            fixture, directory, target, "lqc2_vf0", 0x100
        )
        require(result == EE_EXIT_DISPATCH,
                "LQC2 to VF0 must retire")
        require(cpu.vu0[0].lo == 0xaabbccddeeff0011 and
                cpu.vu0[0].hi == 0x1122334455667788,
                "LQC2 VF0 must discard architectural writes")
        require(READS == [0x100, 0x108], "LQC2 VF0 reads memory")

        result, cpu = execute(
            fixture, directory, target, "sqc2_vf6", 0x100
        )
        require(result == EE_EXIT_DISPATCH,
                "SQC2 aligned source must dispatch")
        require(WRITES == [
            (0x100, 0x9999999988888888),
            (0x108, 0x7777777766666666)
        ], "SQC2 preserves 128-bit vector and order")
        require(len(TRACES) == 2, "SQC2 retains trace instrumentation")
        result, cpu = execute(
            fixture, directory, target, "sqc2_vf6", 0x100, trace_enabled=False
        )
        require(result == EE_EXIT_DISPATCH and WRITES == [
            (0x100, 0x9999999988888888),
            (0x108, 0x7777777766666666)
        ] and not TRACES, "disabled SQC2 trace must preserve both writes")

        result, cpu = execute(
            fixture, directory, target, "sqc2_vf0", 0x100
        )
        require(result == EE_EXIT_DISPATCH,
                "SQC2 VF0 must retire")
        require(WRITES == [
            (0x100, 0),
            (0x108, 0x3f80000000000000)
        ], "SQC2 VF0 must store constant (0,0,0,1)")
        require(cpu.vu0[0].lo == 0xaabbccddeeff0011 and
                cpu.vu0[0].hi == 0x1122334455667788,
                "SQC2 must not touch VF0 backing")

        for mode, excode in (
            ("lqc2_vf6", 4),
            ("sqc2_vf6", 5),
            ("lqc2_vf0", 4),
            ("sqc2_vf0", 5),
            ("lqc2_vf6_delay", 4),
            ("sqc2_vf6_delay", 5),
        ):
            result, cpu = execute(fixture, directory, target, mode, 0x108)
            delay = mode.endswith("_delay")
            fault_pc = ENTRY_PC + (4 if delay else 0)
            expected_epc = ENTRY_PC
            require(result == EE_EXIT_EXCEPTION and
                    cpu.exit_reason == EE_EXIT_EXCEPTION,
                    f"{mode}: misalignment must enter guest exception")
            require(((cpu.cop0[13] >> 2) & 0x1f) == excode,
                    f"{mode}: wrong ExcCode")
            require((cpu.cop0[13] >> 31) == (1 if delay else 0),
                    f"{mode}: wrong Cause.BD")
            require(cpu.cop0[14] == expected_epc,
                    f"{mode}: EPC not pointing to fault/branch")
            require(cpu.cop0[8] == 0x108,
                    f"{mode}: BadVAddr must be guest effective address")
            require((cpu.cop0[12] & 2) == 2,
                    f"{mode}: EXL must be set")
            require(cpu.pad == fault_pc,
                    f"{mode}: diagnostic PC must point to faulting transfer")
            require(READS == [] and WRITES == [] and TRACES == [],
                    f"{mode}: alignment fault touched memory")
            require(cpu.vu0[6].lo == 0x9999999988888888 and
                    cpu.vu0[6].hi == 0x7777777766666666,
                    f"{mode}: alignment fault touched destination")

    _ = exception_lib
    print("vu0_quadword_ir_runtime: PASS (alignment, AdEL/AdES, BD/EPC, VF0)")


if __name__ == "__main__":
    main()
