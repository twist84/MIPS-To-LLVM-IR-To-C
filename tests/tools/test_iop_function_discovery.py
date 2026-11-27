#!/usr/bin/env python3
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
from audit_iop_function_discovery import fnv1a, inventory
from build_iop_aot import prepare_split_pilot, cached_split_object, split_cache_key
from audit_iop_irx_metadata import parse_irx, compare_capture


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
