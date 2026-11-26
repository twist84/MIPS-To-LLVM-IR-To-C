#!/usr/bin/env python3
"""Shared quadword path: exact dependency bodies, FIFO/scalar order, cross-module calls."""
import ctypes
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import llvmlite.binding as llvm
from audit_ee_helper_inventory import ir_inventory
from build_ee_quadword_experiment import QUAD, GUARD, SHARED_QUAD, rewrite_quadword

# Model writes and trace calls as an ordered event stream. The runtime
# record contains a FIFO flag, event count and eight event slots.
RAW = b"""declare i32 @ee_mem_is_quadword_fifo(ptr, i32)
declare void @ee_mem_write128_words(ptr, i32, i64, i64)
declare void @ee_mem_write64(ptr, i32, i64)
declare void @ee_trace_guest_store(ptr, i32, i32, i32, i32, i32, i64, i32, i64, i64, i64, i64)
define internal void @ee_guard_guest_store_trace(ptr %rt, i32 %pc, i32 %op, i32 %effective, i32 %affected, i32 %width, i64 %value, i32 %lane, i64 %base, i64 %sp, i64 %fp, i64 %ra) alwaysinline {
entry:
  call void @ee_trace_guest_store(ptr %rt, i32 %pc, i32 %op, i32 %effective, i32 %affected, i32 %width, i64 %value, i32 %lane, i64 %base, i64 %sp, i64 %fp, i64 %ra)
  ret void
}
define internal void @ee_store_quadword(ptr %rt, i32 %pc, i32 %op, i32 %effective, i32 %affected, i64 %lo, i64 %hi, i64 %base, i64 %sp, i64 %fp, i64 %ra) alwaysinline {
entry:
  %flag = call i32 @ee_mem_is_quadword_fifo(ptr %rt, i32 %affected)
  %fifo = icmp ne i32 %flag, 0
  br i1 %fifo, label %fifo_write, label %scalar_write
fifo_write:
  call void @ee_guard_guest_store_trace(ptr %rt, i32 %pc, i32 %op, i32 %effective, i32 %affected, i32 8, i64 %lo, i32 0, i64 %base, i64 %sp, i64 %fp, i64 %ra)
  %f8 = add i32 %affected, 8
  call void @ee_guard_guest_store_trace(ptr %rt, i32 %pc, i32 %op, i32 %f8, i32 %f8, i32 8, i64 %hi, i32 1, i64 %base, i64 %sp, i64 %fp, i64 %ra)
  call void @ee_mem_write128_words(ptr %rt, i32 %affected, i64 %lo, i64 %hi)
  ret void
scalar_write:
  call void @ee_guard_guest_store_trace(ptr %rt, i32 %pc, i32 %op, i32 %effective, i32 %affected, i32 8, i64 %lo, i32 0, i64 %base, i64 %sp, i64 %fp, i64 %ra)
  call void @ee_mem_write64(ptr %rt, i32 %affected, i64 %lo)
  %s8 = add i32 %affected, 8
  call void @ee_guard_guest_store_trace(ptr %rt, i32 %pc, i32 %op, i32 %s8, i32 %s8, i32 8, i64 %hi, i32 1, i64 %base, i64 %sp, i64 %fp, i64 %ra)
  call void @ee_mem_write64(ptr %rt, i32 %s8, i64 %hi)
  ret void
}
define i32 @guest_a(ptr %cpu, ptr %rt) {
entry:
  call void @ee_store_quadword(ptr %rt, i32 1, i32 2, i32 64, i32 64, i64 7, i64 11, i64 0, i64 0, i64 0, i64 0)
  ret i32 1
}
"""


def main():
    info = ir_inventory(RAW, "guest_a")
    expected = {n: info[n]["body_sha256"] for n in (QUAD, GUARD)}
    a = rewrite_quadword(RAW, RAW, "guest_a", expected, True)
    other = RAW.replace(b"guest_a", b"guest_b")
    b = rewrite_quadword(other, other, "guest_b", expected, False)
    assert b"define void @" + SHARED_QUAD.encode() in a
    assert b"declare void @" + SHARED_QUAD.encode() in b
    for bad in (other.replace(b"i64 %lo, i64 %hi)", b"i64 %hi, i64 %lo)"),
                other + b"\n@escape = global ptr @ee_store_quadword\n",
                other.replace(b"declare void @ee_mem_write64(ptr, i32, i64)", b"declare void @ee_mem_write64(ptr, i32, i32)")):
        try:
            rewrite_quadword(bad, bad, "guest_b", expected, False)
        except ValueError:
            pass
        else:
            raise AssertionError("unsafe quadword rewrite accepted")
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    machine = llvm.Target.from_default_triple().create_target_machine()
    events = []
    fifo = [0]
    p, u, q = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64
    callbacks = {
        "ee_mem_is_quadword_fifo": ctypes.CFUNCTYPE(u, p, u)(lambda rt, addr: fifo[0]),
        "ee_mem_write128_words": ctypes.CFUNCTYPE(None, p, u, q, q)(
            lambda rt, addr, lo, hi: events.append(("fifo", addr, lo, hi))),
        "ee_mem_write64": ctypes.CFUNCTYPE(None, p, u, q)(
            lambda rt, addr, value: events.append(("scalar", addr, value))),
        "ee_trace_guest_store": ctypes.CFUNCTYPE(None, p, u, u, u, u, u, q, u, q, q, q, q)(
            lambda rt, pc, op, effective, affected, width, value, lane, base, sp, fp, ra:
                events.append(("trace", affected, value, lane))),
    }
    for name, cb in callbacks.items():
        llvm.add_symbol(name, ctypes.cast(cb, p).value)
    with llvm.create_context() as ctx:
        first = llvm.parse_assembly(a.decode(), context=ctx)
        second = llvm.parse_assembly(b.decode(), context=ctx)
        first.verify()
        second.verify()
        llvm.link_modules(first, second)
        first.verify()
        assert sum(fn.name == SHARED_QUAD and not fn.is_declaration for fn in first.functions) == 1
        with llvm.create_mcjit_compiler(first, machine) as engine:
            engine.finalize_object()
            for name in ("guest_a", "guest_b"):
                fn = ctypes.CFUNCTYPE(ctypes.c_int32, p, p)(engine.get_function_address(name))
                for flag in (0, 1):
                    fifo[0] = flag
                    events.clear()
                    assert fn(None, None) == 1
                    assert events == ([("trace", 64, 7, 0), ("trace", 72, 11, 1), ("fifo", 64, 7, 11)]
                                      if flag else [("trace", 64, 7, 0), ("scalar", 64, 7),
                                                    ("trace", 72, 11, 1), ("scalar", 72, 11)])
    print("PASS: shared quadword FIFO/scalar order")


if __name__ == "__main__":
    main()
