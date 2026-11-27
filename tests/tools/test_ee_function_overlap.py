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
        assert report["unique_annotated_guest_pcs"] == 3
        assert report["unique_block_starts"] == 3
        assert report["module_scoped_block_starts"] == 5
        assert report["blocks_in_multiple_modules"] == 1
        assert report["within_module_excess_annotations"] == 1
        assert report["wholly_contained_modules"] == 1
        assert report["strict_containment_pairs"] == 2
        assert report["partial_overlap_pairs"] == 1
        assert report["shared_terminal_block_candidates"] == 1
        assert report["guest_modules_with_entry_switch"] == 3


def test_generated_ir(exe, discovery_tool=None):
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
        if discovery_tool:
            import subprocess
            result = subprocess.run([discovery_tool, str(elf), str(out / "ee_discovery_cache.bin")],
                                    capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            sources = json.loads(result.stdout)["entry_sources"]
            assert set(sources) == set(metadata["discovered_entries"])
            assert "elf_entry" in sources["0x100008"]
            assert "direct_call" in sources["0x100040"]
        for function in metadata["functions"]:
            path = out / Path(function["ir_path"]).name
            pcs, blocks, _, _, _, has_switch = scan_details(path)
            assert has_switch
            assert blocks == {int(pc, 0) for pc in function["block_pcs"]}
            assert {0x00100008, 0x0010000c, 0x00100010, 0x00100014} <= pcs, pcs
            assert 0x00100040 not in blocks


def test_binary_accounting():
    import struct
    from ee_ownership_binary import executable_segments, native_executable_sections
    from test_recomp_discovery_cache import make_elf
    with TemporaryDirectory() as tmp:
        path = Path(tmp) / "guest.elf"
        make_elf(path)
        assert executable_segments(path.read_bytes()) == 0x100
    data = bytearray(128)
    struct.pack_into("<HHIIIHH", data, 0, 0x8664, 2, 0, 0, 0, 0, 0)
    struct.pack_into("<4I", data, 28, 0, 0, 12, 100)
    struct.pack_into("<I", data, 56, 0x60000020)
    struct.pack_into("<4I", data, 68, 0, 0, 8, 112)
    struct.pack_into("<I", data, 96, 0x40000040)
    assert native_executable_sections(data) == (12, None)


def test_provenance_ownership():
    from audit_ee_function_overlap import runtime_evidence
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        a = root / "a.ll"
        b = root / "b.ll"
        a.write_text("bb_0x1000:\n  ; 0x1000: nop\n  ; 0x1004: nop\n")
        b.write_text("bb_0x1004:\n  ; 0x1004: nop\n")
        log = root / "run.log"
        log.write_text("live cache-miss: PC=0x00001004, request=1\n"
                       "auto-expand: compiling table sibling PC 0x00001008\n")
        meta = {"discovered_entries": ["0x1000"], "functions": [
            {"vaddr": "0x1000", "ir_path": "a.ll", "block_pcs": ["0x1000"]},
            {"vaddr": "0x1004", "ir_path": "b.ll", "block_pcs": ["0x1004"]}]}
        discovery = {"entry_sources": {"0x1000": ["elf_entry", "direct_call"]}}
        report = audit([a, b], metadata=meta, discovery=discovery,
                       runtime_sources=runtime_evidence([log]))
        entries = report["discovery"]["entries"]
        assert entries["0x1000"]["sources"] == ["direct_call", "elf_entry"]
        assert entries["0x1004"]["sources"] == ["runtime_cache_miss"]
        assert entries["0x1004"]["earlier_inventory_modules_covering_pc"] == 1
        assert entries["0x1008"]["sources"] == ["runtime_table_sibling"]
        assert report["discovery"]["confirmed_runtime_misses_covered_by_other_modules"] == 1


def test_multi_block_tail():
    from audit_ee_function_overlap import shared_tail_regions
    def block(pcs, successors=(), external=()):
        return frozenset(pcs), frozenset(successors), frozenset(external)
    tail = {0x1008: block([0x1008, 0x100c], [0x1010]),
            0x1010: block([0x1010, 0x1014])}
    left = {0x1000: block([0x1000], [0x1008]), **tail}
    right = {0x2000: block([0x2000], [0x1008]), **tail}
    assert list(shared_tail_regions(left, right)) == [{0x1008, 0x1010}]
    right[0x1010] = block([0x1010, 0x1014], external=[0x3000])
    assert not list(shared_tail_regions(left, right))
    cycle = {0x1008: block([0x1008], [0x1010]),
             0x1010: block([0x1010], [0x1008])}
    assert not list(shared_tail_regions(cycle, cycle))


def test_runtime_evidence_ledger():
    from ee_discovery_evidence import EVIDENCE_NAME, load, record
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        elf = root / "guest.elf"
        elf.write_bytes(b"guest")
        record(root, elf, "live cache-miss: PC=0x00001000, request=1\n")
        record(root, elf, "auto-expand: compiling table sibling PC 0x00001000\n")
        record(root, elf, "live cache-miss: PC=0x00001000, request=2\n")
        assert load(root / EVIDENCE_NAME, elf) == {
            0x1000: {"runtime_cache_miss", "runtime_table_sibling"}}
        elf.write_bytes(b"different guest")
        try:
            load(root / EVIDENCE_NAME, elf)
        except ValueError:
            pass
        else:
            raise AssertionError("wrong ELF accepted")


if __name__ == "__main__":
    test_cross_module_overlap()
    test_ownership()
    test_binary_accounting()
    test_provenance_ownership()
    test_multi_block_tail()
    test_runtime_evidence_ledger()
    if len(sys.argv) > 1:
        test_generated_ir(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    print("PASS: cross-module guest PC overlap audit")
