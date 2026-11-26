#!/usr/bin/env python3
"""Verify exact-body sharing, rejection and cross-module execution."""
from pathlib import Path
import ctypes
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import llvmlite.binding as llvm
from audit_ee_helper_inventory import ir_inventory
from build_ee_helper_experiment import FAMILY, SHARED, rewrite

RAW = b"""define internal void @ee_observe_timing_event(ptr %rt, i32 %pc, i32 %opcode, i32 %kind, i64 %attempt, i64 %slot) {
entry:
  store i32 %pc, ptr %rt
  ret void
}
define internal void @ee_trace_instruction_observation(ptr %rt, i32 %pc, i32 %opcode, i32 %kind, i64 %attempt, i64 %slot) {
entry:
  %old = load i32, ptr %rt
  %new = add i32 %old, %opcode
  store i32 %new, ptr %rt
  ret void
}
define internal void @ee_publish_instruction_observation(ptr %rt, i32 %pc, i32 %opcode, i32 %kind, i64 %attempt, i64 %slot) noinline cold {
entry:
  call void @ee_observe_timing_event(ptr %rt, i32 %pc, i32 %opcode, i32 %kind, i64 %attempt, i64 %slot)
  call void @ee_trace_instruction_observation(ptr %rt, i32 %pc, i32 %opcode, i32 %kind, i64 %attempt, i64 %slot)
  ret void
}
define i32 @guest_a(ptr %cpu, ptr %rt) {
entry:
  call void @ee_publish_instruction_observation(ptr %rt, i32 17, i32 23, i32 0, i64 1, i64 1)
  ret i32 1
}
"""


def main():
    expected = {n: x["body_sha256"] for n, x in ir_inventory(RAW, "guest_a").items() if n in FAMILY}
    owner = rewrite(RAW, "guest_a", expected, True)
    other = rewrite(RAW.replace(b"guest_a", b"guest_b"), "guest_b", expected, False)
    assert b"define void @" + SHARED.encode() in owner
    assert b"declare void @" + SHARED.encode() in other
    for bad in (RAW.replace(b"store i32 %pc", b"store i32 %opcode"),
                RAW + b"\n@escape = global ptr @ee_observe_timing_event\n"):
        try:
            rewrite(bad, "guest_a", expected, False)
        except ValueError:
            pass
        else:
            raise AssertionError("unsafe helper rewrite accepted")
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    machine = llvm.Target.from_default_triple().create_target_machine()
    with llvm.create_context() as ctx:
        a = llvm.parse_assembly(owner.decode(), context=ctx)
        b = llvm.parse_assembly(other.decode(), context=ctx)
        a.verify()
        b.verify()
        llvm.link_modules(a, b)
        a.verify()
        assert sum(fn.name == SHARED and not fn.is_declaration for fn in a.functions) == 1
        with llvm.create_mcjit_compiler(a, machine) as engine:
            engine.finalize_object()
            for name in ("guest_a", "guest_b"):
                fn = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p, ctypes.c_void_p)(
                    engine.get_function_address(name))
                result = ctypes.c_int32(-1)
                assert fn(None, ctypes.byref(result)) == 1
                assert result.value == 40
    print("PASS: shared observation")


if __name__ == "__main__":
    main()
