#!/usr/bin/env python3
"""Verify exact-body sharing, rejection and cross-module execution."""
from pathlib import Path
import ctypes
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import llvmlite.binding as llvm
from audit_ee_helper_inventory import ir_inventory
from build_ee_helper_experiment import FAMILY, SHARED, COUNTER, SHARED_COUNTER, rewrite, rewrite_counter

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



COUNTER_RAW = b"""define internal void @ee_count_guest_instruction(ptr %rt, i32 %pc, i32 %opcode) {
entry:
  %valid = icmp ne ptr %rt, null
  br i1 %valid, label %count, label %done
count:
  %attempt_ptr = getelementptr i8, ptr %rt, i64 8
  %slot_ptr = getelementptr i8, ptr %rt, i64 16
  %old = load i64, ptr %attempt_ptr
  %next = add i64 %old, 1
  store i64 %next, ptr %attempt_ptr
  %slot = load i64, ptr %slot_ptr
  %slot_next = add i64 %slot, 1
  store i64 %slot_next, ptr %slot_ptr
  %enabled = load i32, ptr %rt
  %observe = icmp ne i32 %enabled, 0
  br i1 %observe, label %publish, label %done
publish:
  call void @ee_publish_instruction_observation(ptr %rt, i32 %pc, i32 %opcode, i32 0, i64 %next, i64 %slot_next)
  br label %done
done:
  ret void
}
"""


def counter_test(machine):
    original = RAW.replace(b"define i32 @guest_a", COUNTER_RAW + b"\ndefine i32 @guest_a")
    original = original.replace(
        b"call void @ee_publish_instruction_observation(ptr %rt, i32 17, i32 23, i32 0, i64 1, i64 1)",
        b"call void @ee_count_guest_instruction(ptr %rt, i32 17, i32 23)")
    info = ir_inventory(original, "guest_a")
    expected = {n: info[n]["body_sha256"] for n in FAMILY}
    counter = info[COUNTER]["body_sha256"]
    a = rewrite(original, "guest_a", expected, True)
    b_raw = original.replace(b"guest_a", b"guest_b")
    b = rewrite(b_raw, "guest_b", expected, False)
    a = rewrite_counter(original, a, "guest_a", counter, True)
    changed_b = rewrite_counter(b_raw, b, "guest_b", counter, False)
    assert b"define void @" + SHARED_COUNTER.encode() in a
    assert b"declare void @" + SHARED_COUNTER.encode() in changed_b
    for bad in (b.replace(b"add i64 %old, 1", b"add i64 %old, 2"),
                b + b"\n@escape = global ptr @ee_count_guest_instruction\n"):
        try:
            rewrite_counter(b_raw, bad, "guest_b", counter, False)
        except ValueError:
            pass
        else:
            raise AssertionError("unsafe counter rewrite accepted")
    with llvm.create_context() as ctx:
        first = llvm.parse_assembly(a.decode(), context=ctx)
        second = llvm.parse_assembly(changed_b.decode(), context=ctx)
        first.verify()
        second.verify()
        llvm.link_modules(first, second)
        first.verify()
        assert sum(fn.name == SHARED_COUNTER and not fn.is_declaration for fn in first.functions) == 1
        with llvm.create_mcjit_compiler(first, machine) as engine:
            engine.finalize_object()
            for name in ("guest_a", "guest_b"):
                fn = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p, ctypes.c_void_p)(
                    engine.get_function_address(name))
                assert fn(None, None) == 1
                for enabled in (0, 1):
                    state = (ctypes.c_uint64 * 3)(enabled, 7, 11)
                    assert fn(None, state) == 1
                    assert list(state) == [40 if enabled else 0, 8, 12]


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
    counter_test(llvm.Target.from_default_triple().create_target_machine())
    print("PASS: shared observation and counter")


if __name__ == "__main__":
    main()
