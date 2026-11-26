#!/usr/bin/env python3
"""Exact scalar region matching and MCJIT direct/fallback equivalence."""
import ctypes
import faulthandler
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import llvmlite.binding as llvm
from audit_ee_scalar_memory_pilot import outline, select, winners, stable_receipt, expansion_candidates
from build_ee_shared_scalar_experiment import share_helpers, shared_name, residual_candidates, merge_alignment_faults, alignment_winners, alignment_remaining, normalize_entry_switch, rotate_pc
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
                # Destroy ModuleRefs before their LLVM context, even on
                # verification/link/assertion failure. link_modules consumes b.
                with llvm.create_context() as ctx:
                    with llvm.parse_assembly(owner.decode(), context=ctx) as a:
                        with llvm.parse_assembly(other.replace(b"@guest(", b"@guest_other(").decode(), context=ctx) as b:
                            a.verify()
                            b.verify()
                            llvm.link_modules(a, b)
                            a.verify()
                            self.assertEqual(sum(fn.name == shared_name(name) and
                                                 not fn.is_declaration for fn in a.functions), 1)
                with self.assertRaises(ValueError):
                    share_helpers(local, "guest", counts, {name: "wrong"}, owners, "a")
                with self.assertRaises(ValueError):
                    share_helpers(local + b"\n@escape = global ptr @" + name.encode() + b"\n",
                                  "guest", counts, expected, owners, "a")

    def test_residual_inventory(self):
        trials = [{"module": "win.ll", "executable_saving": 2},
                  {"module": "loss.ll", "executable_saving": -1},
                  {"module": "unmatched.ll"}]
        self.assertEqual(set(residual_candidates(trials, {"win.ll"}, 2)),
                         {"loss.ll", "unmatched.ll"})
        with self.assertRaises(ValueError):
            residual_candidates(trials, {"loss.ll"}, 2)
        with self.assertRaises(ValueError):
            residual_candidates(trials + trials[:1], {"win.ll"}, 3)

    def test_entry_switch_rotation(self):
        raw = b"""%ee_cpu_state = type { i32, i32, i32, i32, i32, i32, i32, i32 }
define i32 @guest(ptr %cpu, ptr %rt) {
entry:
  %t0 = getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 6
  %t1 = load i32, ptr %t0
  switch i32 %t1, label %dispatch_unknown [
    i32 4096, label %bb_0x1000
    i32 0, label %bb_0x1000
    i32 4100, label %bb_0x1004
    i32 4294967292, label %bb_0xfffffffc
  ]
dispatch_unknown:
  %t2 = getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 7
  store i32 3, ptr %t2
  ret i32 3
bb_0x1000:
  ret i32 10
bb_0x1004:
  ret i32 11
bb_0xfffffffc:
  ret i32 12
}
"""
        changed, count = normalize_entry_switch(raw, "guest")
        self.assertEqual(count, 4)
        self.assertNotEqual(changed, raw)
        self.assertIn(b"i32 0, label %bb_0x1000", changed)
        self.assertIn(b"i32 1073741823, label %bb_0xfffffffc", changed)
        self.assertIn(b"\n  ]\ndispatch_unknown:", changed)
        # Real module_builder output closes with two spaces. Also accept
        # the original fixture's single-space format without changing it.
        alternate = raw.replace(b"\n  ]\n", b"\n ]\n")
        alt_changed, alt_count = normalize_entry_switch(alternate, "guest")
        self.assertEqual(alt_count, count)
        self.assertEqual(alt_changed, changed.replace(b"\n  ]\n", b"\n ]\n"))
        # The emitter may insert the quadword read scratch slot at entry.
        quad = raw.replace(b"entry:\n", b"entry:\n  %quadword_read_slot = alloca { i64, i64 }, align 16\n", 1)
        quad_changed, quad_count = normalize_entry_switch(quad, "guest")
        self.assertEqual(quad_count, count)
        self.assertIn(b"%quadword_read_slot = alloca { i64, i64 }, align 16", quad_changed)
        self.assertIn(b"switch i32 %ee_entry_rot_key", quad_changed)
        self.assertEqual(normalize_entry_switch(raw.replace(b"i32 4100,", b"i32 4101,"), "guest")[0],
                         raw.replace(b"i32 4100,", b"i32 4101,"))
        for pc in (0, 1, 2, 3, 4096, 4100, 0xffffffff, 0xfffffffc):
            key = rotate_pc(pc)
            self.assertEqual(((key << 2) & 0xffffffff) | (key >> 30), pc)
        linked = raw + changed.split(b"\n", 1)[1].replace(b"@guest(", b"@candidate(")
        llvm.initialize_native_target()
        llvm.initialize_native_asmprinter()
        signature = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p)
        with llvm.create_context() as ctx:
            with llvm.parse_assembly(linked.decode(), context=ctx) as module:
                module.verify()
                target = llvm.Target.from_default_triple().create_target_machine()
                with llvm.create_mcjit_compiler(module, target) as engine:
                    engine.finalize_object()
                    funcs = [signature(engine.get_function_address(n)) for n in ("guest", "candidate")]
                    for pc in [*range(4088, 4110), 0, 1, 2, 3, 4, 0xfffffffc, 0xffffffff, 0x80000000]:
                        outcomes = []
                        for fn in funcs:
                            state = (ctypes.c_uint32 * 8)()
                            state[6], state[7] = pc, 99
                            outcomes.append((fn(state, None), list(state)))
                        self.assertEqual(*outcomes)

    def test_alignment_expansion_inventory(self):
        candidates = [(2, 100, "accepted.ll"), (5, 200, "remaining.ll"), (1, 50, "loss.ll")]
        self.assertEqual(alignment_remaining(candidates, {"accepted.ll": {}}),
                         {"remaining.ll", "loss.ll"})
        with self.assertRaises(ValueError):
            alignment_remaining(candidates, {"unknown.ll": {}})
        with self.assertRaises(ValueError):
            alignment_remaining(candidates + candidates[:1], {})

    def test_alignment_promotion_receipt(self):
        item = {"ir_sha256": "original", "transformed_ir_sha256": "parent",
                "sha256": "object", "size_bytes": 100, "experimental_pipeline": "Oz"}
        row = {"module": "a.ll", "mode": "Oz", "original_ir_sha256": "original",
               "parent_ir_sha256": "parent", "parent_object_sha256": "object",
               "parent_object_bytes": 100, "parent_executable_bytes": 80,
               "merged_sites": 3, "remaining_calls": 1, "ir_bytes_saved": -10,
               "candidate": {"executable_bytes": 60, "object_bytes": 90},
               "executable_saving": 20, "object_saving": 10}
        pilot = {"mode": "read_only_ee_alignment_merge_pilot", "metadata_sha256": sha(b"meta"),
                 "parent_manifest_sha256": sha(b"manifest"), "parent_library_sha256": "dll",
                 "native_target": {}, "inventory_modules": 1, "eligible_modules": 1,
                 "eligible_call_reductions": 2, "selected_modules": 1, "rewritten_modules": 1,
                 "executable_saving": 20, "object_saving": 10, "ir_bytes_saved": -10,
                 "modules": [row]}
        args = ({"a.ll"}, [(2, 100, "a.ll")], b"meta", b"manifest", {"native_target": {}},
                {"a.ll": item}, {"linked_library_sha256": "dll"})
        self.assertEqual(alignment_winners(pilot, *args), {"a.ll": row})
        with self.assertRaises(ValueError):
            alignment_winners(dict(pilot, modules=[dict(row, merged_sites=4)]), *args)
        with self.assertRaises(ValueError):
            alignment_winners(dict(pilot, parent_library_sha256="stale"), *args)

    def test_merge_alignment_faults(self):
        # Same AdEL/BD group may merge; different AdES/BD stays independent.
        raw = b"""declare i32 @ee_raise_exception(ptr, ptr, i32, i32, i32, i32)
define i32 @guest(ptr %cpu, ptr %rt, i32 %t0, i32 %t1, i32 %t2) {
entry:
  %t3 = icmp ne i32 %t0, 0
  br i1 %t3, label %memory_alignment_fault_1, label %memory_alignment_ok_1
memory_alignment_fault_1:
  call i32 @ee_raise_exception(ptr %cpu, ptr %rt, i32 4, i32 100, i32 0, i32 %t0)
  ret i32 1
memory_alignment_ok_1:
  %t4 = icmp ne i32 %t1, 0
  br i1 %t4, label %memory_alignment_fault_2, label %memory_alignment_ok_2
memory_alignment_fault_2:
  call i32 @ee_raise_exception(ptr %cpu, ptr %rt, i32 4, i32 104, i32 0, i32 %t1)
  ret i32 1
memory_alignment_ok_2:
  %t5 = icmp ne i32 %t2, 0
  br i1 %t5, label %memory_alignment_fault_3, label %memory_alignment_ok_3
memory_alignment_fault_3:
  call i32 @ee_raise_exception(ptr %cpu, ptr %rt, i32 5, i32 108, i32 1, i32 %t2)
  ret i32 1
memory_alignment_ok_3:
  ret i32 0
}
"""
        changed, sites, groups = merge_alignment_faults(raw, "guest")
        self.assertEqual((sites, groups), (2, 1))
        self.assertEqual(changed.count(b"call i32 @ee_raise_exception"), 2)
        self.assertIn(b"[ 100, %memory_alignment_fault_1 ], [ 104, %memory_alignment_fault_2 ]", changed)
        self.assertIn(b"[ %t0, %memory_alignment_fault_1 ], [ %t1, %memory_alignment_fault_2 ]", changed)
        self.assertIn(b"i32 5, i32 108, i32 1, i32 %t2", changed)
        self.assertEqual(merge_alignment_faults(raw.replace(b"i32 4, i32 104, i32 0",
                                                           b"i32 4, i32 104, i32 1"), "guest")[1:], (0, 0))
        # Execute both functions with a mock exception publisher that records
        # every argument and call count. No unresolved fallback symbols.
        mock = b"""define i32 @ee_raise_exception(ptr %cpu, ptr %rt, i32 %code, i32 %pc, i32 %bd, i32 %addr) {
entry:
  %p1 = getelementptr i32, ptr %cpu, i32 1
  %p2 = getelementptr i32, ptr %cpu, i32 2
  %p3 = getelementptr i32, ptr %cpu, i32 3
  %p4 = getelementptr i32, ptr %cpu, i32 4
  %old = load i32, ptr %p4
  %next = add i32 %old, 1
  store i32 %code, ptr %cpu
  store i32 %pc, ptr %p1
  store i32 %bd, ptr %p2
  store i32 %addr, ptr %p3
  store i32 %next, ptr %p4
  ret i32 99
}
"""
        linked = mock + raw.split(b"\n", 1)[1] + changed.split(b"\n", 1)[1].replace(b"@guest(", b"@candidate(")
        llvm.initialize_native_target()
        llvm.initialize_native_asmprinter()
        signature = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32)
        with llvm.create_context() as ctx:
            with llvm.parse_assembly(linked.decode(), context=ctx) as module:
                module.verify()
                target = llvm.Target.from_default_triple().create_target_machine()
                with llvm.create_mcjit_compiler(module, target) as engine:
                    engine.finalize_object()
                    functions = [signature(engine.get_function_address(n)) for n in ("guest", "candidate")]
                    for args in ((0, 0, 0), (1, 2, 3), (0, 0xffffffff, 3), (0, 0, 5)):
                        outcomes = []
                        for fn in functions:
                            state = (ctypes.c_uint32 * 5)()
                            ret = fn(state, None, *args)
                            outcomes.append((ret, list(state)))
                        self.assertEqual(*outcomes)

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
    faulthandler.enable()
    unittest.main(verbosity=2)
