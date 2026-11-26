#!/usr/bin/env python3
"""Exact scalar region matching and MCJIT direct/fallback equivalence."""
import ctypes
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import llvmlite.binding as llvm
from audit_ee_scalar_memory_pilot import outline, select, winners, stable_receipt, expansion_candidates
from build_ee_shared_scalar_experiment import share_helpers, shared_name
from build_ee_quadword_experiment import body_of
from audit_ee_helper_inventory import sha


def fixture(width, write):
    bits = width * 8
    prefix = "fastmem_word_" if width == 4 else "fastmem_scalar_"
    check, direct, slow, done = [prefix + s + "_1" for s in ("check", "direct", "slow", "done")]
    ty = f"i{bits}"
    if write:
        direct_op = f"  store {ty} %t20, ptr %t12, align 1"
        slow_op = f"  call void @ee_mem_write{bits}(ptr %rt, i32 %t0, {ty} %t20)"
        final = "  ret i64 0"
        external = f"define void @ee_mem_write{bits}(ptr %rt, i32 %address, {ty} %value) {{\nentry:\n  store i32 %address, ptr @fallback\n  ret void\n}}\n"
    else:
        direct_op = f"  %t13 = load {ty}, ptr %t12, align 1"
        slow_op = f"  %t14 = call {ty} @ee_mem_read{bits}(ptr %rt, i32 %t0)"
        final = f"  %t15 = phi {ty} [ %t13, %{direct} ], [ %t14, %{slow} ]\n"
        final += ("  ret i64 %t15" if width == 8 else f"  %t16 = zext {ty} %t15 to i64\n  ret i64 %t16")
        external = f"define {ty} @ee_mem_read{bits}(ptr %rt, i32 %address) {{\nentry:\n  store i32 %address, ptr @fallback\n  ret {ty} 17\n}}\n"
    value = ("  %t20 = add i64 %value, 0" if width == 8 else f"  %t20 = trunc i64 %value to {ty}")
    return (external + "@fallback = global i32 0\n" + f"""define i64 @guest(ptr %rt, i32 %address, i64 %value) {{
entry:
  %t0 = add i32 %address, 0
{value}
  %t1 = icmp eq ptr %rt, null
  br i1 %t1, label %{slow}, label %{check}
{check}:
  %t2 = getelementptr i8, ptr %rt, i64 0
  %t3 = load i32, ptr %t2
  %t4 = getelementptr i8, ptr %rt, i64 8
  %t5 = load ptr, ptr %t4
  %t6 = zext i32 %t0 to i64
  %t7 = add i64 %t6, {width}
  %t8 = zext i32 %t3 to i64
  %t9 = icmp ule i64 %t7, %t8
  %t10 = icmp ne ptr %t5, null
  %t11 = and i1 %t9, %t10
  br i1 %t11, label %{direct}, label %{slow}
{direct}:
  %t12 = getelementptr i8, ptr %t5, i64 %t6
{direct_op}
  br label %{done}
{slow}:
{slow_op}
  br label %{done}
{done}:
{final}
}}
""").encode()


class Runtime(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("padding", ctypes.c_uint32), ("base", ctypes.c_void_p)]


class TestScalarPilot(unittest.TestCase):
    def test_promotion_selects_only_measured_wins(self):
        good = {"module": "good.ll", "parent_executable_bytes": 10,
                "parent_object_bytes": 20, "candidate": {"executable_bytes": 8,
                "object_bytes": 21, "seconds": 1}, "executable_saving": 2,
                "object_saving": -1, "ir_bytes_saved": 5}
        bad = dict(good, module="bad.ll", executable_saving=-1,
                   candidate=dict(good["candidate"], executable_bytes=11))
        pilot = {"modules": [good, bad], "selected_modules": 2, "rewritten_modules": 2,
                 "executable_saving": 1, "object_saving": -2, "ir_bytes_saved": 10}
        self.assertEqual(set(winners(pilot, ["good.ll", "bad.ll"])), {"good.ll"})
        self.assertEqual(stable_receipt(good), stable_receipt(dict(good,
                         candidate=dict(good["candidate"], seconds=2))))
        with self.assertRaises(ValueError):
            winners(dict(pilot, executable_saving=3), ["good.ll", "bad.ll"])

    def test_expansion_excludes_accepted_and_non_memory(self):
        rows = [{"module": "accepted.ll", "memory_helper_calls": 1},
                {"module": "new.ll", "memory_helper_calls": 2},
                {"module": "zero.ll", "memory_helper_calls": 0},
                {"module": "missing.ll"}]
        self.assertEqual(expansion_candidates(rows, {"accepted.ll"}), ["new.ll"])

    def test_sparse_audit_selection(self):
        rows = [
            {"module": "missing.ll", "guest_pcs": 4, "current_executable_bytes": 1000},
            {"module": "zero.ll", "guest_pcs": 4, "current_executable_bytes": 900,
             "memory_helper_calls": 0},
            {"module": "small.ll", "guest_pcs": 4, "current_executable_bytes": 100,
             "memory_helper_calls": 2},
            {"module": "large.ll", "guest_pcs": 32, "current_executable_bytes": 200,
             "memory_helper_calls": 5},
        ]
        self.assertEqual(select(rows, 0, 1), ["large.ll"])
        self.assertEqual(select(rows, 0, 8), ["large.ll", "small.ll"])
        self.assertEqual(select(rows, 1, 0), ["large.ll", "small.ll"])
        self.assertEqual(select(rows[:2], 8, 8), [])

    def test_share_exact_scalar_helpers(self):
        for width in (1, 2, 4, 8):
            for write in (False, True):
                op = "write" if write else "read"
                bits = width * 8
                ty = f"i{bits}"
                fallback = f"ee_mem_{op}{bits}"
                declaration = (f"declare void @{fallback}(ptr, i32, {ty})" if write else
                               f"declare {ty} @{fallback}(ptr, i32)").encode()
                raw = fixture(width, write)
                raw = raw.replace(body_of(raw, fallback), declaration, 1)
                raw = raw.replace(b"@fallback = global i32 0\n", b"", 1)
                local, counts = outline(raw, "guest")
                name = f"ee_pilot_scalar_{op}{bits}"
                owners = {name: "a"}
                expected = {name: sha(body_of(local, name))}
                owner = share_helpers(local, "guest", counts, expected, owners, "a")
                other = share_helpers(local, "guest", counts, expected, owners, "b")
                self.assertIn(b"define " + (b"void" if write else ty.encode()) +
                              b" @" + shared_name(name).encode(), owner)
                self.assertIn(b"declare " + (b"void" if write else ty.encode()) +
                              b" @" + shared_name(name).encode(), other)
                with llvm.create_context() as ctx:
                    a = llvm.parse_assembly(owner.decode(), context=ctx)
                    b = llvm.parse_assembly(other.replace(b"@guest(", b"@guest_other(").decode(), context=ctx)
                    a.verify()
                    b.verify()
                    a.link_in(b)
                    a.verify()
                    self.assertFalse(a.get_function(shared_name(name)).is_declaration)
                with self.assertRaises(ValueError):
                    share_helpers(local, "guest", counts, {name: "wrong"}, owners, "a")
                with self.assertRaises(ValueError):
                    share_helpers(local + b"\n@escape = global ptr @" + name.encode() + b"\n",
                                  "guest", counts, expected, owners, "a")

    def test_exact_rewrite(self):
        for width in (1, 2, 4, 8):
            for write in (False, True):
                raw = fixture(width, write)
                changed, counts = outline(raw, "guest")
                self.assertNotEqual(raw, changed)
                self.assertEqual(sum(counts.values()), 1)
                bad = raw.replace(b"icmp ule i64", b"icmp ult i64")
                self.assertEqual(outline(bad, "guest")[0], bad)

    def test_semantics(self):
        llvm.initialize_native_target()
        llvm.initialize_native_asmprinter()
        sig = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64)
        for width in (1, 2, 4, 8):
            for write in (False, True):
                raw = fixture(width, write)
                changed, _ = outline(raw, "guest")
                with llvm.create_context() as ctx:
                    original = llvm.parse_assembly(raw.decode(), context=ctx)
                    outlined = llvm.parse_assembly(changed.decode(), context=ctx)
                    original.verify()
                    outlined.verify()
                    # MCJIT takes ownership of its target machine. Never
                    # reuse one across engines or across fixture iterations.
                    target = llvm.Target.from_default_triple()
                    with llvm.create_mcjit_compiler(original, target.create_target_machine()) as a, \
                            llvm.create_mcjit_compiler(outlined, target.create_target_machine()) as b:
                        a.finalize_object()
                        b.finalize_object()
                        for size, present, null_rt, addr in (
                            (16, True, False, 0), (16, True, False, 16-width),
                            (16, True, False, 17-width), (16, True, False, 0xffffffff),
                            (0, True, False, 0), (16, False, False, 0),
                            (16, True, True, 5), (16, True, False, 1)):
                            outcomes = []
                            for engine in (a, b):
                                memory = (ctypes.c_uint8 * 16)(*range(16))
                                rt = Runtime(size, 0, ctypes.addressof(memory) if present else None)
                                marker = ctypes.c_uint32.from_address(engine.get_global_value_address("fallback"))
                                marker.value = 0xdeadbeef
                                f = sig(engine.get_function_address("guest"))
                                ret = f(None if null_rt else ctypes.byref(rt), addr, 0x1122334455667788)
                                outcomes.append((ret, marker.value, bytes(memory)))
                            self.assertEqual(*outcomes)


if __name__ == "__main__":
    unittest.main()
