#!/usr/bin/env python3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from compare_ee_source_lowering import (inventory, ownership_changes, ir_metrics, separate,
    validate_resume, RESUME_IDENTITY_FIELDS)


def main():
    base = {"function_count": 1, "functions": [{
        "ir_path": "old/sub_0x1000_0x1000.ll",
        "host_symbol": "ee_fn_0x1000", "vaddr": "0x1000",
        "block_pcs": ["0x1000", "0x1008"],
    }]}
    changed = {"function_count": 1, "functions": [dict(
        base["functions"][0], ir_path="sub_0x1000_0x1000.ll",
        block_pcs=["0x1008", "0x1000"])]}
    assert not any(ownership_changes(inventory(base), inventory(changed)).values())
    changed["functions"][0]["block_pcs"] = ["0x1000"]
    assert ownership_changes(inventory(base), inventory(changed))["changed"] == [
        "sub_0x1000_0x1000.ll"]
    assert separate(Path("/build/old"), Path("/build/new"))
    assert not separate(Path("/build/old"), Path("/build/old/child"))
    raw = b"""define internal i64 @helper(i64 %x) {
entry:
  %r = add i64 %x, 1
  ret i64 %r
}
define i32 @ee_fn_0x1000(ptr %cpu, ptr %rt) {
entry:
  ; 0x1000: addu
  %r = call i64 @helper(i64 1)
  ret i32 3
}
"""
    counts, opcodes, pcs = ir_metrics(raw, "ee_fn_0x1000")
    assert counts["llvm_instructions"] == 4
    assert counts["llvm_blocks"] == 2
    assert counts["guest_llvm_instructions"] == 2
    assert counts["guest_llvm_blocks"] == 1
    assert counts["guest_pcs"] == 1
    assert pcs == {0x1000}
    assert opcodes["addu"]["ir_instructions"] == 2
    # LLVM inserts an unnamed block after a terminator even though
    # there is no textual label. Parsed CFG counts remain authoritative.
    implicit = raw.replace(b"  ret i32 3\n", b"  ret i32 3\n  ret i32 4\n")
    counts, _, pcs = ir_metrics(implicit, "ee_fn_0x1000")
    assert counts["guest_llvm_blocks"] == 2
    assert counts["lexical_guest_blocks"] == 1
    assert counts["guest_llvm_instructions"] == 3
    assert counts["lexical_count_disagreement"] == 1
    assert pcs == {0x1000}

    identity = {key: key for key in RESUME_IDENTITY_FIELDS}
    comparison = {"missing": [], "extra": [], "matching": 0}
    previous = dict(identity, retail_replay_performed=False,
                    candidate_metadata_sha256="metadata",
                    reconstruction=comparison)
    hashes = {"module": "digest"}
    assert not validate_resume(previous, identity, comparison, "metadata", hashes)
    previous["candidate_ir_hashes"] = hashes
    assert validate_resume(previous, identity, comparison, "metadata", hashes)
    for updated, metadata, candidate in (
            (dict(identity, recompiler_sha256="changed"), "metadata", hashes),
            (identity, "changed", hashes),
            (identity, "metadata", {"module": "changed"})):
        try:
            validate_resume(previous, updated, comparison, metadata, candidate)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid resume accepted")
    print("source-lowering comparison: PASS")


if __name__ == "__main__":
    main()
