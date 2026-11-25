#!/usr/bin/env python3
"""Synthetic R5900 EE load/store alignment and precise COP0 exceptions."""

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
MEMORY = {}
READS = []
WRITES = []
TRACES = []
PAIRS = []
EVENTS = []
ALLOW_FUSED_STORE = False


def read_word(address, size):
    READS.append((size, int(address)))
    return sum(MEMORY.get(int(address) + i, 0) << (8 * i)
               for i in range(size))


def write_word(address, size, value):
    WRITES.append((size, int(address), int(value)))
    EVENTS.append(("write", int(address)))
    for i in range(size):
        MEMORY[int(address) + i] = (int(value) >> (8 * i)) & 0xFF


@ctypes.CFUNCTYPE(ctypes.c_uint16, ctypes.c_void_p, ctypes.c_uint32)
def read16(_rt, addr):
    return read_word(addr, 2)


@ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32)
def read32(_rt, addr):
    return read_word(addr, 4)


@ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint32)
def read64(_rt, addr):
    return read_word(addr, 8)


@ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint16)
def write16(_rt, addr, value):
    write_word(addr, 2, value)


@ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32)
def write32(_rt, addr, value):
    write_word(addr, 4, value)


@ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64)
def write64(_rt, addr, value):
    write_word(addr, 8, value)


@ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(U128))
def read_pair(rt, addr, out):
    PAIRS.append(("read", int(addr)))
    lo = read64(rt, addr)
    hi = read64(rt, addr + 8)
    out[0] = U128(lo, hi)


@ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                  ctypes.c_uint64, ctypes.c_uint64)
def try_write_pair(_rt, addr, lo, hi):
    PAIRS.append(("try_write", int(addr)))
    if not ALLOW_FUSED_STORE:
        return 0
    for offset, value in ((0, lo), (8, hi)):
        for i in range(8):
            MEMORY[int(addr) + offset + i] = (int(value) >> (8 * i)) & 0xFF
    return 1


@ctypes.CFUNCTYPE(
    None, ctypes.c_void_p,
    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64,
    ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64,
    ctypes.c_uint64, ctypes.c_uint64
)
def trace_store(_rt, pc, opcode, address, aligned, size,
                value, slot, old0, old1, old2, old3):
    TRACES.append((int(address), int(aligned), int(size), int(slot)))
    EVENTS.append(("trace", int(aligned)))


def install_symbols(path):
    library = ctypes.CDLL(str(path))
    for name, callback in (
        ("ee_mem_read16", read16),
        ("ee_mem_read32", read32),
        ("ee_mem_read64", read64),
        ("ee_mem_write16", write16),
        ("ee_mem_write32", write32),
        ("ee_mem_write64", write64),
        ("ee_mem_read128_pair", read_pair),
        ("ee_mem_try_write128_ram", try_write_pair),
    ):
        llvm.add_symbol(name, ctypes.cast(callback, ctypes.c_void_p).value)
    register_trace_symbols(trace_store)
    helper = library.ee_raise_exception
    llvm.add_symbol(
        "ee_raise_exception",
        ctypes.cast(helper, ctypes.c_void_p).value
    )
    return library


def execute(fixture, directory, target, mode, address, trace_enabled=True,
            fused_store=False):
    global ALLOW_FUSED_STORE
    ALLOW_FUSED_STORE = fused_store and not trace_enabled
    PAIRS.clear()
    EVENTS.clear()
    MEMORY.clear()
    READS.clear()
    WRITES.clear()
    TRACES.clear()
    for i in range(0x40, 0x60):
        MEMORY[i] = (i * 7 + 3) & 0xFF
    before = dict(MEMORY)

    guest, engine, module, machine, offset = compile_jit(
        fixture, pathlib.Path(directory), mode, target
    )
    require(offset + 8 < 0x4000, "JIT counter outside runtime backing")
    cpu_backing, cpu = aligned_instance(EEState)
    rt_backing = ctypes.create_string_buffer(RUNTIME_BACKING_BYTES)
    set_store_trace_enabled(rt_backing, module, trace_enabled)
    UNEXPECTED_TRACES.clear()
    cpu.gpr[9] = U128(address, 0)
    cpu.gpr[8] = U128(0x0123456789abcdef, 0xfedcba9876543210)
    cpu.fpr[8] = 0xaabbccdd11223344
    cpu.gpr[31] = U128(0x00140000, 0)
    cpu.cop0[12] = 0
    result = guest(
        ctypes.pointer(cpu),
        ctypes.cast(rt_backing, ctypes.c_void_p)
    )
    require(not UNEXPECTED_TRACES, f"disabled trace called: {UNEXPECTED_TRACES}")
    _ = (cpu_backing, rt_backing, engine, module, machine)
    return result, cpu, before


def main():
    require(len(sys.argv) == 3,
            "usage: test_ee_alignment_ir_runtime.py fixture exception_lib")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    helper = install_symbols(pathlib.Path(sys.argv[2]).resolve())
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()

    with tempfile.TemporaryDirectory(prefix="ee-alignment-") as directory:
        # LQ/SQ round down their effective address, even when unaligned.
        result, cpu, original = execute(
            fixture, directory, target, "ee_lq", 0x43)
        expected_lo = sum(original[0x40 + i] << (8 * i)
                          for i in range(8))
        expected_hi = sum(original[0x48 + i] << (8 * i)
                          for i in range(8))
        require(result == EE_EXIT_DISPATCH and
                READS == [(8, 0x40), (8, 0x48)] and not WRITES,
                "LQ must read two halves at 0x40/0x48")
        require(PAIRS == [("read", 0x40)], "LQ did not use paired helper")
        require(cpu.gpr[8].lo == expected_lo and
                cpu.gpr[8].hi == expected_hi,
                "LQ must replace the complete 128-bit GPR")

        result, cpu, original = execute(
            fixture, directory, target, "ee_sq", 0x43)
        require(result == EE_EXIT_DISPATCH and not READS and
                [(size, addr) for size, addr, value in WRITES] ==
                [(8, 0x40), (8, 0x48)],
                "SQ must write two halves at 0x40/0x48")
        require(TRACES[0][:2] == (0x43, 0x40) and
                TRACES[1][:2] == (0x4b, 0x48),
                "SQ trace must keep original and affected addresses")
        require(all(MEMORY[0x40 + i] ==
                    ((0x0123456789abcdef >> (8 * i)) & 0xFF)
                    for i in range(8)),
                "SQ low half corrupted")
        require(all(MEMORY[0x48 + i] ==
                    ((0xfedcba9876543210 >> (8 * i)) & 0xFF)
                    for i in range(8)),
                "SQ high half corrupted")

        require(EVENTS == [("trace", 0x40), ("write", 0x40),
                           ("trace", 0x48), ("write", 0x48)],
                "SQ trace/write ordering")
        require(PAIRS == [("try_write", 0x40)], "SQ paired helper not attempted")

        result, cpu, original = execute(
            fixture, directory, target, "ee_sq", 0x43,
            trace_enabled=False, fused_store=True)
        require(result == EE_EXIT_DISPATCH and
                PAIRS == [("try_write", 0x40)] and not WRITES and not TRACES,
                "SQ fused path must not repeat scalar writes")
        require(bytes(MEMORY[i] for i in range(0x40, 0x50)) ==
                bytes.fromhex("efcdab89674523011032547698badcfe"),
                "SQ fused path corrupted lane data")

        result, cpu, original = execute(
            fixture, directory, target, "ee_lq_zero", 0x43)
        require(result == EE_EXIT_DISPATCH and
                PAIRS == [("read", 0x40)] and READS == [(8, 0x40), (8, 0x48)] and
                cpu.gpr[0].lo == 0 and cpu.gpr[0].hi == 0,
                "LQ GPR0 must still read both lanes and discard the result")
        result, cpu, original = execute(
            fixture, directory, target, "ee_sq_zero", 0x43)
        require(result == EE_EXIT_DISPATCH and
                WRITES == [(8, 0x40, 0), (8, 0x48, 0)],
                "SQ GPR0 must write both zero halves")

        # Natural alignment is mandatory for scalar and COP1 transfers.
        for mode, width, excode in (
            ("ee_lh", 2, 4), ("ee_lw", 4, 4),
            ("ee_ld", 8, 4), ("ee_lwc1", 4, 4),
            ("ee_sh", 2, 5), ("ee_sw", 4, 5),
            ("ee_sd", 8, 5), ("ee_swc1", 4, 5),
            ("ee_lw_delay", 4, 4), ("ee_sw_delay", 4, 5),
        ):
            result, cpu, original = execute(
                fixture, directory, target, mode, 0x43)
            delayed = mode.endswith("_delay")
            require(result == EE_EXIT_EXCEPTION and
                    cpu.exit_reason == EE_EXIT_EXCEPTION,
                    f"{mode}: unaligned access did not raise exception")
            require(((cpu.cop0[13] >> 2) & 0x1f) == excode,
                    f"{mode}: wrong ExcCode")
            require((cpu.cop0[13] >> 31) == int(delayed),
                    f"{mode}: wrong Cause.BD")
            require(cpu.cop0[14] == ENTRY_PC and
                    cpu.cop0[8] == 0x43 and
                    (cpu.cop0[12] & 2) != 0,
                    f"{mode}: wrong EPC, BadVAddr or EXL")
            require(cpu.pad == ENTRY_PC + (4 if delayed else 0),
                    f"{mode}: wrong diagnostic fault PC")
            require(MEMORY == original and not READS and
                    not WRITES and not TRACES,
                    f"{mode}: alignment exception accessed memory")
            require(cpu.gpr[8].lo == 0x0123456789abcdef and
                    cpu.gpr[8].hi == 0xfedcba9876543210 and
                    cpu.fpr[8] == 0xaabbccdd11223344,
                    f"{mode}: alignment exception modified destination")
        # The positive scalar word path remains operational.
        result, cpu, original = execute(
            fixture, directory, target, "ee_lw", 0x40)
        value = sum(original[0x40 + i] << (8 * i) for i in range(4))
        sign_extended = value | (0xffffffff00000000
                                 if value & 0x80000000 else 0)
        require(result == EE_EXIT_DISPATCH and
                READS == [(4, 0x40)] and not WRITES and
                cpu.gpr[8].lo == sign_extended,
                "aligned LW path regressed")

    _ = helper
    print("ee_alignment_ir_runtime: PASS (LQ/SQ masks, scalar traps, BD/EPC)")


if __name__ == "__main__":
    main()
