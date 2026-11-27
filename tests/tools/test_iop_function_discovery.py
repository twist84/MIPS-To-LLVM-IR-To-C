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
