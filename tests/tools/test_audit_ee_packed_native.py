#!/usr/bin/env python3
import copy
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from audit_ee_packed_native import packing, validate_pair


def receipt():
    name = "sub_0x1000_0x1000.ll"
    result = {key: key for key in (
        "elf_sha256", "baseline_metadata_sha256",
        "baseline_translation_sha256", "baseline_object_manifest_sha256",
        "baseline_library", "native_target", "candidate_metadata_sha256")}
    result.update({
        "schema": "ee-source-lowering-comparison-v1",
        "stage": "linked_o5_candidate", "profile": 5, "functions": 1,
        "candidate_ir_hashes": {name: "ir-a"},
        "ownership_changes": {"missing": [], "extra": [], "changed": []},
        "guest_pc_changes": [],
        "modules": [{
            "module": name, "symbol": "ee_fn_00001000",
            "before_sha256": "original-ir", "after_sha256": "ir-a",
            "before": {"object_sha256": "original-object"},
            "after": {"guest_pcs": 4, "object_sha256": "native-a"},
        }],
    })
    return result


def main():
    raw = b"""  %a = insertelement <2 x i64> undef, i64 1, i32 0
  %b = insertelement <2 x i64> %a, i64 2, i32 1
  %c = extractelement <2 x i64> %b, i32 0
  %d = insertelement <4 x i32> undef, i32 1, i32 0
"""
    assert packing(raw) == {"insert_2xi64": 2, "extract_2xi64": 1}
    assert packing(raw.replace(b"\n", b"\r\n")) == packing(raw)
    a = receipt()
    b = copy.deepcopy(a)
    name = b["modules"][0]["module"]
    b["modules"][0]["after_sha256"] = "ir-b"
    b["candidate_ir_hashes"][name] = "ir-b"
    left, right = validate_pair(a, b)
    assert left[name]["after_sha256"] != right[name]["after_sha256"]
    assert left[name]["after"]["object_sha256"] == right[name]["after"]["object_sha256"]
    for mutate in (
        lambda r: r.update(native_target="other"),
        lambda r: r.update(stage="static_ir_complete"),
        lambda r: r["modules"][0]["after"].update(guest_pcs=5),
        lambda r: r["modules"][0].update(before_sha256="other"),
        lambda r: r["modules"][0]["before"].update(object_sha256="other"),
        lambda r: r["candidate_ir_hashes"].update({name: "stale"}),
        lambda r: r["ownership_changes"]["changed"].append(name),
        lambda r: r["modules"].append(copy.deepcopy(r["modules"][0])),
    ):
        bad = copy.deepcopy(b)
        mutate(bad)
        try:
            validate_pair(a, bad)
        except ValueError:
            pass
        else:
            raise AssertionError("incompatible receipt accepted")
    print("packed native audit: PASS")


if __name__ == "__main__":
    main()
