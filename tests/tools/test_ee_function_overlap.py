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


def test_ownership():
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        def module(name, entries, body):
            path = root / (name + ".ll")
            switch = "".join(f"    i32 {pc}, label %bb_0x{pc:x}\n" for pc in entries)
            path.write_text("define i32 @ee_fn_00001000(ptr %cpu, ptr %rt) {\n"
                            "  switch i32 %pc, label %dispatch_unknown [\n"
                            + switch + "  ]\n" + body
                            + "bb_0x2000:\n  ret i32 3\nbb_0x0:\n  ret i32 0\n}\n")
            return path
        tail = "bb_0x1008:\n  ; 0x100c: nop\n  call void @ee_count_guest_instruction(ptr %rt, i32 4104, i32 0)\n  ret i32 0\n"
        a = module("a", [4096, 4104], "bb_0x1000:\n  ; 0x1000: nop\n  ; 0x1000: nop\n  br label %bb_0x1008\n" + tail)
        b = module("b", [4104], tail)
        c = module("c", [4104, 4112], tail + "bb_0x1010:\n  ; 0x1010: nop\n  br label %bb_0x1008\n")
        report = audit([a, b, c])
        assert report["unique_guest_pcs"] == 4
        assert report["unique_block_starts"] == 3
        assert report["module_scoped_block_starts"] == 5
        assert report["blocks_in_multiple_modules"] == 1
        assert report["within_module_excess_annotations"] == 1
        assert report["wholly_contained_modules"] == 1
        assert report["strict_containment_pairs"] == 2
        assert report["partial_overlap_pairs"] == 1
        assert report["shared_terminal_block_candidates"] == 1
        assert report["guest_modules_with_entry_switch"] == 3


def test_generated_ir(exe):
    from test_recomp_discovery_cache import make_elf, compile_ir
    from audit_ee_function_overlap import scan_details
    import json
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        elf = root / "game.elf"
        make_elf(elf)
        out = root / "ir"
        result = compile_ir(exe, elf, out, 0x00100008)
        assert result.returncode == 0, result.stdout + result.stderr
        metadata = json.loads((out / "ee_functions.json").read_text())
        for function in metadata["functions"]:
            path = out / Path(function["ir_path"]).name
            pcs, blocks, _, _, _, has_switch = scan_details(path)
            assert has_switch
            assert blocks == {int(pc, 0) for pc in function["block_pcs"]}
            assert {0x00100008, 0x0010000c, 0x00100010, 0x00100014} <= pcs, pcs
            assert 0x00100040 not in blocks


if __name__ == "__main__":
    test_cross_module_overlap()
    test_ownership()
    if len(sys.argv) > 1:
        test_generated_ir(sys.argv[1])
    print("PASS: cross-module guest PC overlap audit")
