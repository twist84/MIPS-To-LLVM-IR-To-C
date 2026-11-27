#!/usr/bin/env python3
"""Compare two isolated EE lowering images without rebuilding or executing them.

Object digests, executable byte counts and optional raw disassembly are
static evidence, not dynamic instruction counts or register-residency proof.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess

from audit_ee_helper_inventory import read_manifest, verified_object
from ee_ownership_binary import native_executable_sections


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rows(receipt):
    result = {row["module"]: row for row in receipt["modules"]}
    if (receipt["schema"] != "ee-source-lowering-comparison-v1" or
            receipt["stage"] != "linked_o5_candidate" or
            receipt["profile"] != 5 or
            len(result) != len(receipt["modules"]) or
            len(result) != receipt["functions"] or
            set(result) != set(receipt["candidate_ir_hashes"]) or
            any(receipt["ownership_changes"].values()) or
            receipt["guest_pc_changes"]):
        raise ValueError("expected complete ownership-preserving O5 receipt")
    for name, row in result.items():
        if (Path(name).name != name or
                row["after_sha256"] != receipt["candidate_ir_hashes"][name]):
            raise ValueError("invalid module identity: " + name)
    return result


def validate_pair(control, candidate):
    a, b = rows(control), rows(candidate)
    for key in ("elf_sha256", "baseline_metadata_sha256",
                "baseline_translation_sha256", "baseline_object_manifest_sha256",
                "baseline_library", "native_target", "candidate_metadata_sha256"):
        if control[key] != candidate[key]:
            raise ValueError("receipts differ: " + key)
    if set(a) != set(b):
        raise ValueError("module inventory differs")
    for name in a:
        if (a[name]["symbol"] != b[name]["symbol"] or
                a[name]["before_sha256"] != b[name]["before_sha256"] or
                a[name]["before"]["object_sha256"] != b[name]["before"]["object_sha256"] or
                a[name]["after"]["guest_pcs"] != b[name]["after"]["guest_pcs"]):
            raise ValueError("baseline or guest coverage differs: " + name)
    return a, b


def open_image(directory, receipt, modules):
    ir = directory / "ir"
    objects = directory / "objects"
    raw, manifest, records = read_manifest(objects, set(modules))
    if (sha(raw) != receipt["candidate_object_manifest_sha256"] or
            sha((ir / "ee_functions.json").read_bytes()) !=
            receipt["candidate_metadata_sha256"] or
            sha((directory / "game-translated.dll").read_bytes()) !=
            receipt["candidate_library"]["sha256"] or
            any(manifest.get(k) != v for k, v in receipt["native_target"].items())):
        raise ValueError("image identity differs: " + str(directory))
    return ir, objects, records


def packing(raw):
    # Lexical counts of the canonical packed GPR shape, including helpers.
    # These are emitted IR operations, not optimized native instructions.
    return {
        "insert_2xi64": len(re.findall(rb"= insertelement <2 x i64>", raw)),
        "extract_2xi64": len(re.findall(rb"= extractelement <2 x i64>", raw)),
    }


def audit(control_path, candidate_path, control_dir, candidate_dir,
          samples=4, dumpbin=None):
    raw_receipts = [p.read_bytes() for p in (control_path, candidate_path)]
    receipts = [json.loads(raw) for raw in raw_receipts]
    modules = validate_pair(*receipts)
    images = [open_image(d, r, m) for d, r, m in
              zip((control_dir, candidate_dir), receipts, modules)]
    totals = Counter()
    changed = []
    for name in sorted(modules[0]):
        a, b = (m[name] for m in modules)
        ir_changed = a["after_sha256"] != b["after_sha256"]
        object_changed = a["after"]["object_sha256"] != b["after"]["object_sha256"]
        ir_bytes, object_bytes = [], []
        for row, (ir, objects, records) in zip((a, b), images):
            record = records[name]
            if record["sha256"] != row["after"]["object_sha256"]:
                raise ValueError("receipt/manifest mismatch: " + name)
            raw_ir = (ir / name).read_bytes()
            if sha(raw_ir) != row["after_sha256"]:
                raise ValueError("IR digest differs: " + name)
            raw_object = verified_object(objects, record, row["after_sha256"])
            if (len(raw_ir) != row["after"]["ir_bytes"] or
                    len(raw_object) != row["after"]["object_bytes"]):
                raise ValueError("receipt byte count differs: " + name)
            ir_bytes.append(raw_ir)
            object_bytes.append(raw_object)
        totals["modules"] += 1
        totals["ir_changed"] += ir_changed
        totals["object_changed"] += object_changed
        totals["ir_changed_object_identical"] += ir_changed and not object_changed
        totals["ir_identical_object_changed"] += not ir_changed and object_changed
        for arm, raw in zip(("control", "candidate"), object_bytes):
            totals[arm + "_executable_bytes"] += native_executable_sections(raw)[0]
        if ir_changed or object_changed:
            p, q = (packing(raw) for raw in ir_bytes)
            changed.append({
                "module": name, "symbol": a["symbol"],
                "guest_pcs": a["after"]["guest_pcs"],
                "ir_changed": ir_changed, "object_changed": object_changed,
                "control_packing": p, "candidate_packing": q,
                "removed_inserts": p["insert_2xi64"] - q["insert_2xi64"],
                "removed_extracts": p["extract_2xi64"] - q["extract_2xi64"],
                "control_executable_bytes": native_executable_sections(object_bytes[0])[0],
                "candidate_executable_bytes": native_executable_sections(object_bytes[1])[0],
            })
    totals["removed_inserts"] = sum(x["removed_inserts"] for x in changed)
    totals["removed_extracts"] = sum(x["removed_extracts"] for x in changed)
    # Prefer compact modules with an actual packed-IR reduction; sample
    # both changed and identical native objects when each class exists.
    selected = []
    for object_changed in (True, False):
        candidates = [x for x in changed if x["object_changed"] == object_changed
                      and x["removed_inserts"] > 0]
        candidates.sort(key=lambda x: (x["guest_pcs"] > 128,
                        -x["removed_inserts"], x["guest_pcs"], x["module"]))
        selected.extend(candidates[:samples])
    assembly = []
    if dumpbin:
        for row in selected:
            entry = {"module": row["module"], "symbol": row["symbol"]}
            for arm, (_, objects, records) in zip(("control", "candidate"), images):
                path = objects / records[row["module"]]["object"]
                proc = subprocess.run([dumpbin, "/DISASM", "/NOBYTES", str(path)],
                                      check=True, capture_output=True, text=True,
                                      errors="replace")
                entry[arm] = proc.stdout
            assembly.append(entry)
    return {
        "schema": "ee-packed-native-audit-v1",
        "control_receipt_sha256": sha(raw_receipts[0]),
        "candidate_receipt_sha256": sha(raw_receipts[1]),
        "totals": dict(totals), "changed_modules": changed,
        "sample_modules": selected, "disassembly": assembly,
        "disassembly_tool": dumpbin, "runtime_performed": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control-receipt", type=Path, required=True)
    parser.add_argument("--candidate-receipt", type=Path, required=True)
    parser.add_argument("--control-dir", type=Path, required=True)
    parser.add_argument("--candidate-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=2)
    parser.add_argument("--dumpbin", help="optional path to MSVC dumpbin.exe")
    args = parser.parse_args()
    if args.samples < 0 or args.samples > 8:
        parser.error("samples must be in 0..8 per object class")
    # Never write into either experiment cache or replace any existing file.
    output = args.output.resolve()
    for directory in (args.control_dir, args.candidate_dir):
        if output == directory.resolve() or directory.resolve() in output.parents:
            parser.error("output must be outside experiment caches")
    if output.exists():
        parser.error("output already exists")
    result = audit(args.control_receipt, args.candidate_receipt,
                   args.control_dir, args.candidate_dir, args.samples, args.dumpbin)
    with output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps(result["totals"], indent=2))


if __name__ == "__main__":
    main()
