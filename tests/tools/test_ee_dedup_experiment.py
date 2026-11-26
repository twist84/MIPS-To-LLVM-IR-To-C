#!/usr/bin/env python3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from build_ee_dedup_experiment import coalesce, dispatch_map, select_routes


def fn(pc, blocks):
    return {"vaddr": hex(pc), "ir_path": f"{pc:x}.ll",
            "block_pcs": [hex(p) for p in blocks]}


def candidate(owner, pcs):
    return {"owner": owner, "status": "matching_emitted_ir",
            "all_source_dispatch_pcs_match": True,
            "source_dispatch_checks": [{"pc": hex(pc), "status": "matching_emitted_ir"}
                                       for pc in pcs]}


def row(pc, candidates):
    return {"pc": hex(pc), "source": f"{pc:x}.ll", "candidates": candidates}


def run():
    metadata = {"entry_point": "0x1000", "discovered_entries": ["0x1000"],
                "functions": [fn(0x1000, [0x1000]), fn(0x1004, [0x1004, 0x1008]),
                              fn(0x100c, [0x100c]), fn(0x1010, [0x1010])],
                "function_count": 4}
    report = {"entries": [row(0x1004, [candidate("1000.ll", [0x1004, 0x1008])]),
                          row(0x100c, [candidate("1004.ll", [0x100c])]),
                          row(0x1010, [candidate("1000.ll", [0x1010])]),
                          row(0x1000, [candidate("1010.ll", [0x1000])])]}
    seen = []
    routes = select_routes(metadata, report, lambda *args: seen.append(args[:2]))
    assert routes == {"1004.ll": "1000.ll", "1010.ll": "1000.ll"}
    assert len(seen) == 3
    merged = coalesce(metadata, routes)
    assert merged["function_count"] == 2
    assert dispatch_map(merged["functions"]) == {
        0x1000: "1000.ll", 0x1004: "1000.ll", 0x1008: "1000.ll",
        0x100c: "100c.ll", 0x1010: "1000.ll"}
    assert metadata["function_count"] == 4
    assert metadata["functions"][0]["block_pcs"] == ["0x1000"]
    # No transitive or circular owner routes.
    for bad in ({"100c.ll": "1004.ll", "1004.ll": "1000.ll"},
                {"1004.ll": "1004.ll"}):
        try:
            coalesce(metadata, bad)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid route accepted")
    report["entries"][0]["candidates"][0]["source_dispatch_checks"].pop()
    try:
        select_routes(metadata, report, lambda *args: None)
    except ValueError:
        pass
    else:
        raise AssertionError("incomplete dispatch evidence accepted")


if __name__ == "__main__":
    run()
    print("PASS: isolated EE dedup route selection and exact dispatch inventory")
