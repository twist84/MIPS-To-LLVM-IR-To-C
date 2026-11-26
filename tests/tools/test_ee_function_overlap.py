#!/usr/bin/env python3
"""Synthetic cross-module overlap, including repeated delay-slot annotations."""
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from audit_ee_function_overlap import audit


def test_cross_module_overlap():
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        a, b, c = [root / f"{name}.ll" for name in ("a", "b", "c")]
        a.write_text("bb_0x1000:\n  ; 0x1000: addiu\n  ; 0x1004: nop\n"
                     "  ; 0x1004: nop\nbb_0x1008:\n  ; 0x1008: jr\n")
        b.write_text("bb_0x1008:\n  ; 0x1008: jr\n  ; 0x100c: nop\n")
        c.write_text("define void @helper() { ret void }\n")
        report = audit([a, b, c], {"a.o": 123, "b.o": 456})
        assert report["modules_scanned"] == 3
        assert report["guest_modules"] == 2
        assert report["unique_annotated_guest_pcs"] == 4
        assert report["module_scoped_guest_pcs"] == 5
        assert report["cross_module_excess_pc_copies"] == 1
        assert report["within_module_excess_annotations"] == 1
        assert report["cross_module_excess_block_copies"] == 1
        assert report["overlapping_modules"] == 2
        assert report["largest_overlaps"][0]["object_file_bytes"] == 123
        assert report["most_duplicated_pcs"] == [{"pc": "0x1008", "module_count": 2}]


if __name__ == "__main__":
    test_cross_module_overlap()
    print("PASS: cross-module guest PC overlap audit")
