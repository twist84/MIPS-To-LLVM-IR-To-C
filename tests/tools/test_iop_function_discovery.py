#!/usr/bin/env python3
import json
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
from audit_iop_function_discovery import fnv1a, inventory
from build_iop_aot import (prepare_split_pilot, cached_split_object, split_cache_key,
                           relocation_receipt_roots, select_split_stems,
                           split_compile_schedule, compile_split_modules)
from audit_iop_irx_metadata import (parse_irx, compare_capture, capture_mismatches,
                                    discover_relocation_roots)


from audit_iop_indirect_targets import constant_indirect_targets
from audit_iop_jump_tables import indexed_load_base, table_candidates, discover_table_edges


BASE = 0x10000


def jump(op, target):
    return (op << 26) | (target >> 2)


def capture(words, entry=BASE, exports=()):
    data = struct.pack("<" + "I" * len(words), *words)
    return {"name": "synthetic", "base": BASE, "text_size": len(data),
            "hash": f"{fnv1a(data):016x}", "entry": entry,
            "exports": list(exports)}, data


class DiscoveryTest(unittest.TestCase):
    def test_calls_delay_and_unreachable_data(self):
        meta, data = capture([
            jump(3, BASE + 24), 0, 0x03e00008, 0,
            jump(3, BASE + 32), 0, 0x03e00008, 0,
            0x03e00008, 0,
        ])
        report = inventory(meta, data)
        self.assertEqual([r["entry"] for r in report["regions"]],
                         [BASE, BASE + 24])
        self.assertEqual(report["ordinary_count"], 3)
        self.assertEqual(report["delay_count"], 3)
        self.assertEqual(report["unvisited_aligned_words"], 4)
        self.assertEqual(report["regions"][0]["ordinary_pcs"], [BASE, BASE + 8])
        self.assertEqual(report["ordinary_delay_overlap"], [])
        self.assertEqual(inventory(meta, data), report)

    def test_shared_tail_and_delay_entry(self):
        meta, data = capture([
            jump(2, BASE + 16), 0, jump(2, BASE + 16), 0,
            0x03e00008, 0,
        ], exports=(BASE + 8, BASE + 20, BASE + 1, BASE + 48))
        report = inventory(meta, data)
        self.assertEqual(report["shared_ordinary_pcs"],
                         [{"pc": BASE + 16, "entries": [BASE, BASE + 8]}])
        self.assertEqual(report["ordinary_delay_overlap"], [BASE + 20])
        self.assertEqual(len(report["rejected_seeds"]), 2)

    def test_unsupported_delay_and_indirect(self):
        meta, data = capture([
            jump(3, BASE + 16), jump(2, BASE + 16),
            0x0320f809, 0, 0x03e00008, 0,
        ], exports=(BASE + 8,))
        report = inventory(meta, data)
        self.assertEqual(report["candidate_count"], 2)
        self.assertIn("unsupported_delay", {x["kind"] for x in report["unresolved"]})
        self.assertIn("indirect_call", {x["kind"] for x in report["unresolved"]})
        self.assertIn("return", {x["kind"] for x in report["unresolved"]})

    def test_split_plan_is_identity_bound_and_excludes_unsupported(self):
        meta, data = capture([0, 0x0000000c, 0x03e00008, 0],
                             exports=(BASE + 8,))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "SIO2MAN.json").write_text(json.dumps(meta))
            (root / "SIO2MAN.bin").write_bytes(data)
            plan = prepare_split_pilot(root, "sio2man", root / "pilot")
            self.assertEqual(plan.read_text().splitlines(),
                             [f"IOPREG1 {BASE} 16 {fnv1a(data)} 2",
                              f"1 {BASE}", f"1 {BASE + 8}"])
            self.assertEqual((root / "pilot" / "SIO2MAN.bin").read_bytes(), data)
            with self.assertRaises(ValueError):
                prepare_split_pilot(root, "unknown", root / "bad")
            meta["exports"].append(BASE + 12)
            (root / "SIO2MAN.json").write_text(json.dumps(meta))
            with self.assertRaises(ValueError):
                prepare_split_pilot(root, "SIO2MAN", root / "ambiguous")

    def test_extended_frames_and_interpreter_continuations(self):
        meta, data = capture([
            0x88820000,  # LWL: native audit stops, interpreter can continue
            0x03e00008, 0,
            0x27bdfff0, 0xafb00008, 0xafbf000c,  # separate stack frame
            0x03e00008, 0x27bd0010,
            0x27bdfff0, 0xafbf0014,  # RA outside frame: reject
            0x0000000c,  # syscall: no invented continuation
        ])
        baseline = inventory(meta, data)
        self.assertEqual(baseline["candidate_count"], 1)
        self.assertEqual(baseline["ordinary_count"], 1)
        report = inventory(meta, data, extended=True)
        extra = report["extended"]
        self.assertEqual(report["candidate_count"], 2)
        self.assertEqual(extra["heuristic_frame_seeds"],
                         [{"entry": BASE + 12, "ra_save_pc": BASE + 20}])
        self.assertEqual(extra["interpreter_linear_pcs"], [BASE])
        self.assertEqual(extra["newly_reached_pcs"],
                         list(range(BASE + 4, BASE + 32, 4)))
        self.assertEqual(inventory(meta, data), baseline)

    def test_extended_interpreter_delay_preserves_control_flow(self):
        meta, data = capture([jump(2, BASE + 12), 0x88820000, 0,
                              0x03e00008, 0])
        baseline = inventory(meta, data)
        self.assertEqual(baseline["ordinary_count"], 1)
        report = inventory(meta, data, extended=True)
        self.assertEqual(report["ordinary_count"], 2)
        self.assertEqual(report["extended"]["interpreter_linear_pcs"], [BASE + 4])
        self.assertIn("unsupported_delay", {x["kind"] for x in report["unresolved"]})
        self.assertNotIn(BASE + 8, report["extended"]["newly_reached_pcs"])

    def test_residual_plan_preserves_legacy_dispatch_domain(self):
        # Unsupported ordinary word, inline delay word and an unvisited
        # tail are all residual entries, not inferred function ownership.
        meta, data = capture([0, 0x0000000c, 0x03e00008, 0, 0, 0],
                             exports=(BASE + 8,))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "MCMAN.json").write_text(json.dumps(meta))
            (root / "MCMAN.bin").write_bytes(data)
            plan = prepare_split_pilot(root, "MCMAN", root / "pilot", residual=True)
            lines = plan.read_text().splitlines()
            self.assertEqual(lines[0], f"IOPREG1 {BASE} 24 {fnv1a(data)} 3")
            self.assertEqual(lines[1:], [f"1 {BASE}", f"1 {BASE + 8}",
                f"4 {BASE + 4} {BASE + 12} {BASE + 16} {BASE + 20}"])
            pcs = [int(pc) for line in lines[1:] for pc in line.split()[1:]]
            self.assertEqual(sorted(pcs), list(range(BASE, BASE + len(data), 4)))
            self.assertEqual(len(pcs), len(set(pcs)))
            meta["exports"].append(BASE + 12)
            (root / "MCMAN.json").write_text(json.dumps(meta))
            with self.assertRaises(ValueError):
                prepare_split_pilot(root, "MCMAN", root / "ambiguous", residual=True)

    def test_extended_plan_retains_full_dispatch_domain(self):
        meta, data = capture([
            0x88820000, 0x03e00008, 0,
            0x27bdfff0, 0xafbf000c, 0x03e00008, 0x27bd0010,
            0,
        ])
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "MCMAN.json").write_text(json.dumps(meta))
            (root / "MCMAN.bin").write_bytes(data)
            with self.assertRaises(ValueError):
                prepare_split_pilot(root, "MCMAN", root / "invalid", extended=True)
            plan = prepare_split_pilot(root, "MCMAN", root / "pilot",
                                       residual=True, extended=True)
            lines = plan.read_text().splitlines()
            self.assertEqual(lines[0], f"IOPREG1 {BASE} 32 {fnv1a(data)} 3")
            self.assertEqual(lines[1:], [
                f"1 {BASE + 4}",
                f"3 {BASE + 12} {BASE + 16} {BASE + 20}",
                f"4 {BASE} {BASE + 8} {BASE + 24} {BASE + 28}",
            ])
            pcs = [int(pc) for line in lines[1:] for pc in line.split()[1:]]
            self.assertEqual(sorted(pcs), list(range(BASE, BASE + len(data), 4)))
            self.assertEqual(len(pcs), len(set(pcs)))

    def test_original_irx_metadata_and_verified_relocated_call(self):
        original_words = [jump(3, 8), 0, 0x03e00008, 0]
        original = struct.pack("<4I", *original_words)
        # ELF32 + two program headers + one REL section (loader offsets).
        raw = bytearray(0x200)
        raw[:6] = b"\x7fELF\x01\x01"
        struct.pack_into("<HHIIIIIHHHHHH", raw, 16, 0xff80, 8, 1, 0,
                         52, 0x140, 0, 52, 32, 2, 40, 2, 0)
        struct.pack_into("<8I", raw, 52, 1, 0x100, 0, 0, 16, 16, 5, 4)
        struct.pack_into("<8I", raw, 84, 0x70000080, 0xc0, 0, 0, 48, 48, 0, 4)
        struct.pack_into("<6I", raw, 0xc0, 0, 0, 0x80, 16, 0, 0)
        raw[0xda:0xda + 10] = b"synthetic\0"
        raw[0x100:0x110] = original
        struct.pack_into("<10I", raw, 0x168, 0, 9, 0, 0, 0x1c0, 8, 0, 0, 4, 8)
        struct.pack_into("<II", raw, 0x1c0, 0, 4)
        parsed, text = parse_irx(bytes(raw))
        self.assertEqual(text, original)
        self.assertEqual(parsed["relocations"], [{"offset": 0, "type": 4,
                                                  "symbol_index": 0}])
        meta, linked = capture([jump(3, BASE + 8), 0, 0x03e00008, 0])
        meta["gp"] = BASE + 0x80
        match = compare_capture(meta, linked, parsed, text, inventory(meta, linked))
        self.assertEqual(match["verified_relocated_calls"],
                         [{"site": BASE, "target": BASE + 8, "site_reached": True}])
        self.assertEqual(match["nonrelocation_changed_word_count"], 0)
        self.assertTrue(match["metadata_match_only"])
        self.assertIsNone(compare_capture(dict(meta, gp=0), linked, parsed, text,
                                          inventory(meta, linked)))
        with self.assertRaises(ValueError):
            parse_irx(bytes(raw[:100]))

    def test_relocation_shadow_discovers_unvisited_leaf(self):
        meta, linked = capture([0x03e00008, 0, jump(3, BASE + 16), 0,
                                0x03e00008, 0])
        meta["gp"] = BASE + 0x80
        original = struct.pack("<6I", 0x03e00008, 0, jump(3, 16), 0,
                               0x03e00008, 0)
        parsed = {"name": "synthetic", "entry_offset": 0, "gp_offset": 0x80,
                  "text_size": len(linked), "function_symbols": [],
                  "sections": [{"flags": 4, "address": 0, "size": len(linked)}],
                  "relocations": [{"offset": 8, "type": 4}]}
        baseline = inventory(meta, linked, extended=True)
        match = compare_capture(meta, linked, parsed, original, baseline)
        shadow = discover_relocation_roots(meta, linked, parsed, match, baseline)
        self.assertEqual(shadow["new_roots"], [{"entry": BASE + 16,
                                               "call_sites": [BASE + 8]}])
        self.assertEqual(shadow["newly_reached_pcs"], [BASE + 16, BASE + 20])
        self.assertEqual(shadow["inventory"]["regions"][1]["origins"],
                         ["verified_relocated_jal"])
        self.assertEqual(shadow["inventory"]["unvisited_aligned_words"], 2)
        self.assertEqual(inventory(meta, linked, extended=True), baseline)
        self.assertEqual(capture_mismatches(dict(meta, gp=0), linked, parsed),
                         {"gp": {"captured": 0, "elf": BASE + 0x80}})
        receipt = {"schema": 1, "kind": "iop_original_irx_metadata",
                   "matches": {"synthetic": [{"metadata": parsed, "comparison": match,
                                              "relocation_discovery": shadow}]}}
        self.assertEqual(relocation_receipt_roots(receipt, meta, linked, baseline),
                         [BASE + 16])
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "MCMAN.json").write_text(json.dumps(meta))
            (root / "MCMAN.bin").write_bytes(linked)
            plan = prepare_split_pilot(root, "MCMAN", root / "pilot", residual=True,
                                       extended=True, relocation_receipt=receipt)
            self.assertEqual(plan.read_text().splitlines()[1:], [
                f"1 {BASE}", f"1 {BASE + 16}",
                f"4 {BASE + 4} {BASE + 8} {BASE + 12} {BASE + 20}"])
        changed = bytearray(linked)
        struct.pack_into("<I", changed, 8, jump(3, BASE + 12))
        with self.assertRaises(ValueError):
            relocation_receipt_roots(receipt, meta, changed, baseline)
        with self.assertRaises(ValueError):
            relocation_receipt_roots(receipt, dict(meta, hash="0"), linked, baseline)
        self.assertEqual(capture_mismatches(
            dict(meta, name="ARIKA_IOP_Control_Driver"), linked,
            dict(parsed, name="ARIKA IOP-Control-Driver")), {})
        parsed["sections"][0]["flags"] = 0
        with self.assertRaises(ValueError):
            relocation_receipt_roots(receipt, meta, linked, baseline)
        self.assertEqual(discover_relocation_roots(meta, linked, parsed, match,
                                                   baseline)["new_roots"], [])
        with self.assertRaises(ValueError):
            inventory(meta, linked, relocated_call_entries=[BASE + 16])

    def test_indirect_target_shadow_and_delay_slot(self):
        # LUI/ADDIU builds t9; JALR captures it before the delay overwrites t9.
        meta, data = capture([0x3c190001, 0x27390018, 0x0320f809,
                              0x24190000, 0x03e00008, 0, 0x03e00008, 0])
        report = inventory(meta, data, extended=True)
        result = constant_indirect_targets(meta, data, report)
        self.assertEqual(result["new_call_candidates"], [BASE + 24])
        self.assertEqual(result["observations"][0]["producer_pcs"], [BASE, BASE + 4])
        self.assertEqual(result["unresolved"], [])
        # An alternative entry invalidates the unique predecessor chain.
        meta["exports"] = [BASE + 4]
        result = constant_indirect_targets(meta, data, inventory(meta, data, extended=True))
        self.assertEqual(result["observations"], [])
        self.assertEqual(len(result["unresolved"]), 1)

    def test_cross_block_indirect_target_in_predecessor_delay_slot(self):
        # LUI t9,1; j BASE+16; addiu t9,t9,24 (delay); unreachable;
        # jr t9; nop; jr ra; nop.
        meta, data = capture([0x3c190001, jump(2, BASE + 16), 0x27390018,
                              0, 0x03200008, 0, 0x03e00008, 0])
        report = inventory(meta, data, extended=True)
        local = constant_indirect_targets(meta, data, report, audit_producers=True)
        self.assertEqual(local["new_jump_targets"], [])
        cross = constant_indirect_targets(meta, data, report, audit_producers=True,
                                          cross_block=True)
        self.assertEqual(cross["new_jump_targets"], [BASE + 24])
        self.assertEqual(cross["observations"][0]["producer_pcs"], [BASE, BASE + 8])
        self.assertEqual(cross["observations"][0]["crossed_edges"], [
            {"pc": BASE + 4, "target": BASE + 16, "kind": "jump",
             "delay_pc": BASE + 8}])
        self.assertEqual(inventory(meta, data, extended=True), report)

    def test_cross_block_stops_at_call_and_ambiguous_predecessor(self):
        # JAL does not imply a unique register state at the resume PC.
        meta, data = capture([0x3c190001, jump(3, BASE + 24), 0x27390018,
                              0x03200008, 0, 0, 0x03e00008, 0])
        result = constant_indirect_targets(meta, data, inventory(meta, data, extended=True),
                                           cross_block=True)
        self.assertEqual(result["observations"], [])
        self.assertEqual(result["unresolved"][0]["crossed_edges"], [])
        # Both conditional branch outcomes reach the same successor.
        meta, data = capture([0x3c190001, 0x10800001, 0x27390018,
                              0x03200008, 0, 0, 0x03e00008, 0])
        result = constant_indirect_targets(meta, data, inventory(meta, data, extended=True),
                                           cross_block=True)
        self.assertEqual(result["observations"], [])
        self.assertEqual(result["unresolved"][0]["crossed_edges"], [])

    def test_indirect_target_shadow_does_not_assume_load_values(self):
        meta, data = capture([0x3c190001, 0x37390018, 0x8f390000,
                              0x0320f809, 0, 0x03e00008, 0])
        report = inventory(meta, data, extended=True)
        result = constant_indirect_targets(meta, data, report)
        self.assertEqual(result["observations"], [])
        self.assertEqual(len(result["unresolved"]), 1)

    def test_indirect_producer_audit_classifies_memory_without_reading(self):
        # lui t0,1; lw v0,0x20(t0); sw v0,0(sp); jalr v0
        meta, data = capture([0x3c080001, 0x8d020020, 0xafa20000,
                              0x0040f809, 0, 0x03e00008, 0])
        result = constant_indirect_targets(meta, data, inventory(meta, data, extended=True),
                                           audit_producers=True)
        self.assertEqual(result["observations"], [])
        producer = result["unresolved"][0]["producer"]
        self.assertEqual(producer["classification"], "constant_address_load")
        self.assertEqual(producer["definitions"][-1]["constant_address"], BASE + 32)
        self.assertEqual([x["pc"] for x in producer["definitions"]],
                         [BASE, BASE + 4])

    def test_indirect_producer_audit_scaled_index_and_gp(self):
        # sll v0,a0,2; lui v1,1; addu v0,v0,v1; lw v0,16(v0);
        # move t9,v0; jr t9; nop
        meta, data = capture([0x00041080, 0x3c030001, 0x00431021,
                              0x8c420010, 0x0040c821, 0x03200008, 0])
        result = constant_indirect_targets(meta, data, inventory(meta, data, extended=True),
                                           audit_producers=True)
        producer = result["unresolved"][0]["producer"]
        self.assertEqual(producer["classification"], "scaled_index_load")
        self.assertEqual(len(producer["definitions"]), 5)
        meta, data = capture([0x8f828004, 0x0040f809, 0, 0x03e00008, 0])
        result = constant_indirect_targets(meta, data, inventory(meta, data, extended=True),
                                           audit_producers=True)
        producer = result["unresolved"][0]["producer"]
        self.assertEqual(producer["classification"], "gp_relative_load")
        self.assertEqual(producer["definitions"][0]["offset"], -32764)

    def test_indirect_jump_is_not_function_root(self):
        meta, data = capture([0x3c080001, 0x35080010, 0x01000008, 0,
                              0x03e00008, 0])
        result = constant_indirect_targets(meta, data, inventory(meta, data, extended=True))
        self.assertEqual(result["new_jump_targets"], [BASE + 16])
        self.assertEqual(result["new_call_candidates"], [])

    def test_jump_table_shadow_outside_linked_text(self):
        # index=a0, table=BASE+32, JR v0; two unvisited return targets.
        meta, linked = capture([0x00041080, 0x3c030001, 0x00431021,
                                 0x8c420020, 0x00400008, 0,
                                 0x03e00008, 0])
        discovery = inventory(meta, linked, extended=True)
        indirect = constant_indirect_targets(meta, linked, discovery,
                                             audit_producers=True)
        self.assertEqual(indexed_load_base(indirect["unresolved"][0]["producer"]),
                         BASE + 32)
        original = linked + struct.pack("<3I", 24, 24, 24)
        parsed = {"sections": [
            {"name": ".text", "type": 1, "flags": 6, "address": 0, "size": 32},
            {"name": ".data", "type": 1, "flags": 3, "address": 32, "size": 12}],
            "relocations": [{"offset": 32, "type": 2}, {"offset": 36, "type": 2}]}
        tables = table_candidates(meta, linked, parsed, original, discovery, indirect)
        self.assertEqual(len(tables), 1)
        table = tables[0]
        self.assertEqual([x["target"] for x in table["entries"]], [BASE + 24] * 2)
        self.assertEqual(table["stop"], "missing_or_ambiguous_r_mips_32")
        self.assertFalse(table["bound_proven"])
        self.assertFalse(table["entries"][0]["linked_word_verified"])
        self.assertFalse(table["entries"][0]["already_ordinary"])
        shadow = discover_table_edges(meta, linked, discovery, [], tables)
        self.assertEqual(shadow["candidate_edges"], [
            {"site": BASE + 16, "target": BASE + 24}])
        self.assertEqual(shadow["newly_reached_pcs"], [BASE + 24, BASE + 28])
        self.assertEqual(shadow["new_function_entries"], [])
        self.assertIn(BASE + 24, shadow["inventory"]["regions"][0]["ordinary_pcs"])
        self.assertIn("indirect_jump", {x["kind"] for x in shadow["inventory"]["unresolved"]})
        self.assertFalse(shadow["bounds_proven"])
        self.assertEqual(inventory(meta, linked, extended=True), discovery)
        with self.assertRaises(ValueError):
            inventory(meta, linked, extended=True, candidate_jump_edges=[(BASE, BASE + 24)])
        parsed["relocations"].append({"offset": 36, "type": 2})
        self.assertEqual(len(table_candidates(meta, linked, parsed, original,
                                              discovery, indirect)[0]["entries"]), 1)
        parsed["sections"][1]["flags"] = 6
        self.assertEqual(table_candidates(meta, linked, parsed, original,
                                          discovery, indirect)[0]["entries"], [])

    def test_table_backed_plan_recomputes_edges_and_preserves_residual(self):
        meta, linked = capture([0x00041080, 0x3c030001, 0x00431021,
                                0x8c420020, 0x00400008, 0,
                                0x03e00008, 0])
        meta["gp"] = BASE + 0x80
        original = linked + struct.pack("<2I", 24, 24)
        parsed = {"name": "synthetic", "entry_offset": 0, "gp_offset": 0x80,
                  "text_size": len(linked), "function_symbols": [],
                  "sections": [
                      {"name": ".text", "type": 1, "flags": 6,
                       "address": 0, "size": 32},
                      {"name": ".rodata", "type": 1, "flags": 2,
                       "address": 32, "size": 8}],
                  "relocations": [{"offset": 32, "type": 2},
                                  {"offset": 36, "type": 2}]}
        baseline = inventory(meta, linked, extended=True)
        comparison = compare_capture(meta, linked, parsed, original, baseline)
        shadow = discover_relocation_roots(meta, linked, parsed, comparison, baseline)
        receipt = {"schema": 1, "kind": "iop_original_irx_metadata",
                   "matches": {"synthetic": [{
                       "path": "module.irx", "elf_sha256": hashlib.sha256(original).hexdigest(),
                       "metadata": parsed, "comparison": comparison,
                       "relocation_discovery": shadow}]}}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            disc = root / "disc"
            disc.mkdir()
            elf = disc / "module.irx"
            elf.write_bytes(original)
            # Staging changes descriptor name but not linked identity.
            (root / "CANONICAL.json").write_text(json.dumps(dict(meta, name="CANONICAL")))
            (root / "CANONICAL.bin").write_bytes(linked)
            old_plan = prepare_split_pilot(root, "CANONICAL", root / "old",
                                           residual=True, extended=True,
                                           relocation_receipt=receipt)
            with patch("audit_iop_irx_metadata.parse_irx", return_value=(parsed, original)):
                new_plan = prepare_split_pilot(root, "CANONICAL", root / "new",
                                               residual=True, extended=True,
                                               relocation_receipt=receipt,
                                               table_disc_root=disc)
                old_rows = [list(map(int, line.split()))[1:]
                            for line in old_plan.read_text().splitlines()[1:]]
                new_rows = [list(map(int, line.split()))[1:]
                            for line in new_plan.read_text().splitlines()[1:]]
                self.assertIn(BASE + 24, old_rows[-1])
                self.assertIn(BASE + 24, new_rows[0])
                self.assertNotIn(BASE + 24, new_rows[-1])
                self.assertEqual(sorted(pc for row in new_rows for pc in row),
                                 list(range(BASE, BASE + len(linked), 4)))
                elf.write_bytes(original + b"changed")
                with self.assertRaisesRegex(ValueError, "original ELF changed"):
                    prepare_split_pilot(root, "CANONICAL", root / "bad",
                                        residual=True, extended=True,
                                        relocation_receipt=receipt, table_disc_root=disc)
            with self.assertRaisesRegex(ValueError, "requires relocation evidence"):
                prepare_split_pilot(root, "CANONICAL", root / "missing",
                                    residual=True, extended=True, table_disc_root=disc)

    def test_table_shadow_reports_shared_ownership(self):
        # Two independently seeded JR paths reach the same return block.
        meta, data = capture([0x00400008, 0, 0x00600008, 0, 0x03e00008, 0],
                             exports=(BASE + 8,))
        baseline = inventory(meta, data, extended=True)
        tables = [{"site": site, "entries": [{"target": BASE + 16}]}
                  for site in (BASE, BASE + 8)]
        shadow = discover_table_edges(meta, data, baseline, [], tables)
        self.assertEqual(shadow["inventory"]["shared_ordinary_pcs"],
                         [{"pc": BASE + 16, "entries": [BASE, BASE + 8]}])
        self.assertEqual(shadow["new_function_entries"], [])

    def test_region_cache_hits_invalidation_and_corruption(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cache = root / "cache"
            cache.mkdir()
            source = root / "region.ll"
            source.write_text("first IR")
            obj = root / "region.o"
            calls = []

            def compile_object(src, dst):
                calls.append(src.read_bytes())
                dst.write_bytes(b"object:" + src.read_bytes())

            context = {"target": "test", "options": ["-O3"], "plan": "a"}
            self.assertFalse(cached_split_object(source, obj, cache, context, compile_object))
            obj.unlink()
            self.assertTrue(cached_split_object(source, obj, cache, context, compile_object))
            self.assertEqual(obj.read_bytes(), b"object:first IR")
            self.assertEqual(len(calls), 1)
            changed = dict(context, options=["-O2"])
            self.assertFalse(cached_split_object(source, obj, cache, changed, compile_object))
            source.write_text("second IR")
            self.assertFalse(cached_split_object(source, obj, cache, context, compile_object))
            self.assertEqual(len(calls), 3)
            entry = cache / split_cache_key(source, context)
            (entry / "object.o").write_bytes(b"corrupt")
            with self.assertRaises(ValueError):
                cached_split_object(source, obj, cache, context, compile_object)
            self.assertEqual(len(calls), 3)

    def test_multi_split_selection_and_global_schedule(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("Alpha", "Beta"):
                (root / f"{name}.json").touch()
            self.assertEqual(select_split_stems(root, ["beta", "ALPHA"]),
                             ["Alpha", "Beta"])
            self.assertEqual(select_split_stems(root, None, True), ["Alpha", "Beta"])
            for selection in (["Alpha", "alpha"], ["missing"]):
                with self.assertRaises(ValueError):
                    select_split_stems(root, selection)
            a, b, c = (root / name for name in ("a.ll", "b.ll", "c.ll"))
            for source, size in ((a, 2), (b, 9), (c, 2)):
                source.write_bytes(b"x" * size)
            self.assertEqual(split_compile_schedule({"Alpha": [a, b], "Beta": [c]}),
                             [("Alpha", b), ("Alpha", a), ("Beta", c)])

    def test_multi_split_global_queue_link_order_and_warm_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ir = root / "ir"
            cache = root / "cache"
            ir.mkdir()
            cache.mkdir()
            stems = ["Alpha", "Beta"]
            sizes = {"Alpha.region.0.ll": 2, "Alpha.region.1.ll": 9,
                     "Alpha.ll": 3, "Alpha.table.ll": 1,
                     "Beta.region.0.ll": 12, "Beta.ll": 4, "Beta.table.ll": 1}
            for name, size in sizes.items():
                (ir / name).write_bytes(b"x" * size)
            args = SimpleNamespace(opt="3", jobs=1, region_cache=cache)
            provenance = {stem: {"module": stem} for stem in stems}
            compiled, linked = [], []

            def fake_run(command, check):
                self.assertTrue(check)
                output = Path(command[command.index("-o") + 1])
                if "-c" in command:
                    source = Path(command[command.index("-o") - 1])
                    compiled.append(source.name)
                    output.write_bytes(b"object:" + source.name.encode())
                else:
                    objects = [Path(arg) for arg in command if arg.endswith(".o")]
                    linked.append([obj.name for obj in objects])
                    output.write_bytes(b"".join(obj.read_bytes() for obj in objects))

            def build(name):
                out = root / name
                obj, lib = out / "obj", out / "modules"
                obj.mkdir(parents=True)
                lib.mkdir()
                with patch("build_iop_aot.subprocess.run", side_effect=fake_run):
                    libraries = compile_split_modules(
                        args, "clang", ir, obj, lib, stems, ".so", provenance)
                aggregate = json.loads((out / "split-build-receipt.json").read_text())
                return aggregate, [path.read_bytes() for path in libraries]

            cold, cold_bytes = build("cold")
            self.assertEqual(compiled[:2], ["Beta.region.0.ll", "Alpha.region.1.ll"])
            self.assertEqual(len(compiled), len(sizes))
            self.assertEqual(linked, [
                ["Alpha.region.0.o", "Alpha.region.1.o", "Alpha.o", "Alpha.table.o"],
                ["Beta.region.0.o", "Beta.o", "Beta.table.o"]])
            self.assertEqual(cold["object_count"], len(sizes))
            self.assertEqual(cold["cache_hits"], 0)
            for row in cold["modules"]:
                receipt = json.loads((root / "cold" / row["receipt"]).read_text())
                self.assertEqual(receipt["library_sha256"], row["library_sha256"])
            compiled.clear()
            warm, warm_bytes = build("warm")
            self.assertEqual(compiled, [])
            self.assertEqual(warm["cache_hits"], len(sizes))
            self.assertEqual(cold_bytes, warm_bytes)

    def test_capture_identity_and_exclusive_receipt(self):
        meta, data = capture([0x03e00008, 0])
        with self.assertRaises(ValueError):
            inventory(meta, data + b"\0")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "module.json").write_text(json.dumps(meta))
            (root / "module.bin").write_bytes(data)
            out = root / "receipt.txt"
            cmd = [sys.executable, str(TOOLS / "audit_iop_function_discovery.py"),
                   "--capture", str(root), "--out", str(out)]
            subprocess.run(cmd, check=True, capture_output=True)
            receipt = out.read_bytes()
            self.assertNotEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
            self.assertEqual(out.read_bytes(), receipt)
            self.assertEqual((root / "module.bin").read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
