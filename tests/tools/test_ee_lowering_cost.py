#!/usr/bin/env python3
"""Test lexical attribution, block boundaries, cloned PCs and strict counts."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from audit_ee_lowering_cost import scan_guest, band

IR = b"""define internal void @ee_guard_guest_store_trace(ptr %rt) {
entry:
  ret void
}
define i32 @ee_fn_1000(ptr %cpu, ptr %rt) {
entry:
  %p = getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 6
  %pc = load i32, ptr %p
  switch i32 %pc, label %dispatch_unknown [
    i32 4096, label %bb_0x1000
  ]
dispatch_unknown:
  ret i32 3
bb_0x1000:
  ; 0x1000: ADDIU
  call void @ee_count_guest_instruction(ptr %rt, i32 4096, i32 0)
  %g = getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 0, i32 2, i32 0
  %v = load i64, ptr %g
  %n = add i64 %v, 1
  store i64 %n, ptr %g
  br label %alu_count_fast_1
alu_count_fast_1:
  ; 0x1000: ADDIU
  %q = add i64 %n, 1
  br label %alu_count_merge_1
alu_count_merge_1:
  call void @ee_count_guest_instruction(ptr %rt, i32 4100, i32 0)
  br label %bb_0x1008
bb_0x1008:
  ; 0x1008: LW
  %x = call i32 @ee_mem_read32(ptr %rt, i32 0)
  br label %fastmem_word_done_3
fastmem_word_done_3:
  ret i32 0
}
"""


class TestLoweringCost(unittest.TestCase):
    def test_counts(self):
        t, u, m, p, blocks, pcs = scan_guest(IR, "ee_fn_1000")
        self.assertEqual(pcs, {4096, 4100, 4104})
        self.assertEqual(t["guest_pcs"], 3)
        self.assertEqual(t["count_only_pcs"], 1)
        self.assertEqual(t["annotation_occurrences"], 3)
        self.assertEqual(t["annotation_excess"], 1)
        self.assertEqual(t["llvm_blocks"], 7)
        self.assertEqual(t["ir_instructions"], 17)
        self.assertEqual(t["switch_case_lines"], 1)
        self.assertEqual(t["accounting_calls"], 2)
        self.assertEqual(t["gpr_geps"], 1)
        self.assertEqual(t["gpr_loads"], 1)
        self.assertEqual(t["gpr_stores"], 1)
        self.assertEqual(t["fastmem_region_instructions"], 1)
        self.assertEqual(t["entry_dispatch_instructions"], 4)
        self.assertEqual(m["ADDIU"]["annotation_occurrences"], 2)
        self.assertEqual(m["ADDIU"]["function_scoped_annotated_pcs"], 1)
        self.assertEqual(m["LW"]["memory_helper_calls"], 1)
        self.assertEqual(p[4096]["ir_instructions"], 8)
        self.assertEqual(blocks["alu_count_merge_1"]["ir_instructions"], 2)
        self.assertEqual(sum(v["ir_instructions"] for v in m.values()) +
                         u["ir_instructions"], t["ir_instructions"])

    def test_refuses_unknown_instruction(self):
        with self.assertRaises(ValueError):
            scan_guest(IR.replace(b"%q = add", b"%q = not_an_opcode"), "ee_fn_1000")

    def test_bands(self):
        self.assertEqual([band(n) for n in (0, 4, 5, 32, 128, 512, 513)],
                         ["no-observed-pcs", "1-4", "5-16", "17-32", "33-128", "129-512", "513+"])


if __name__ == "__main__":
    unittest.main()
