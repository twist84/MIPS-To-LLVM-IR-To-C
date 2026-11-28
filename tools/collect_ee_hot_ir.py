#!/usr/bin/env python3
"""Collect profile-ranked EE IR without mutating the active translation cache."""

import argparse
from collections import defaultdict
import csv
import hashlib
import io
import json
from pathlib import Path
import sys
import zipfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def collect(replay, ir, out, limit):
    if limit < 1 or out.exists() or out.resolve().is_relative_to(ir.resolve()):
        raise ValueError("use a positive limit and a fresh output outside the IR cache")
    with zipfile.ZipFile(replay) as archive:
        profile = archive.read("first-boot.json.pc-profile.csv")
        cache = json.loads(archive.read("translation-cache.json"))
        provenance = json.loads(archive.read("replay-provenance.json"))
    metadata_bytes = (ir / "ee_functions.json").read_bytes()
    if sha(metadata_bytes) != cache["metadata_sha256"]:
        raise ValueError("local metadata does not match captured translation cache")
    metadata = json.loads(metadata_bytes)
    owners = defaultdict(set)
    entries = defaultdict(set)
    for function in metadata["functions"]:
        name = function["ir_path"].replace("\\", "/").rsplit("/", 1)[-1]
        if not name.endswith(".ll") or name in ("", ".", ".."):
            raise ValueError("invalid IR filename")
        pc = int(function["vaddr"], 0)
        entries[pc].add(name)
        for block in function.get("block_pcs", []):
            owners[int(block, 0)].add(name)
    rows = []
    seen = set()
    for row in csv.DictReader(io.StringIO(profile.decode("utf-8"))):
        pc = int(row["pc"], 0)
        values = {key: int(row[key]) for key in (
            "calls", "dispatches", "instructions", "idle_charged",
            "native_boundary_ns", "nested_ns")}
        if (pc in seen or pc < 0 or pc > 0xffffffff or values["calls"] <= 0
                or min(values.values()) < 0
                or values["idle_charged"] > values["instructions"]
                or values["nested_ns"] > values["native_boundary_ns"]):
            raise ValueError("invalid or duplicate profile row")
        seen.add(pc)
        values["pc"] = pc
        values["exclusive_ns"] = values["native_boundary_ns"] - values["nested_ns"]
        rows.append(values)
    if not rows:
        raise ValueError("empty EE profile")
    hot = sorted(rows, key=lambda r: (-r["exclusive_ns"], r["pc"]))[:limit]
    frequent = sorted(rows, key=lambda r: (-r["calls"], r["pc"]))[:limit]
    chosen = {r["pc"]: r for r in hot + frequent}
    modules = set()
    selected = []
    for pc, row in sorted(chosen.items(), key=lambda item: (-item[1]["exclusive_ns"], item[0])):
        entry = sorted(entries[pc])
        blocks = sorted(owners[pc] - entries[pc])
        if not entry and not blocks:
            # Keep unresolved PCs visible instead of guessing a module.
            pass
        modules.update(entry + blocks)
        selected.append(dict(row, exact_entry_modules=entry, resume_candidate_modules=blocks))
    if not modules:
        raise ValueError("no selected PCs map to local IR metadata")
    verified = {}
    for name in sorted(modules):
        data = (ir / name).read_bytes()
        if sha(data) != cache["ir_hashes"].get(name):
            raise ValueError(f"IR hash differs from capture: {name}")
        verified[name] = data
    receipt = {
        "schema": "ee-hot-ir-selection-v1",
        "git_head": provenance["git_head"],
        "metadata_sha256": sha(metadata_bytes),
        "profile_sha256": sha(profile),
        "profile_rows": len(rows),
        "selection_per_rank": limit,
        "total_exclusive_ns": sum(r["exclusive_ns"] for r in rows),
        "selected_exclusive_ns": sum(r["exclusive_ns"] for r in chosen.values()),
        "selected": selected,
        "modules": {name: sha(data) for name, data in verified.items()},
        "limitations": [
            "entry counts are not CFG edge frequencies",
            "resume candidates are metadata ownership candidates, not observed native targets",
            "native boundary exclusive time includes host boundary overhead",
        ],
    }
    # Exclusive creation and validate all selected bytes before writing.
    with zipfile.ZipFile(out, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("selection.json", json.dumps(receipt, indent=2) + "\n")
        archive.writestr("first-boot.json.pc-profile.csv", profile)
        archive.writestr("ee_functions.json", metadata_bytes)
        for name, data in verified.items():
            archive.writestr("ir/" + name, data)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("replay", type=Path)
    parser.add_argument("--ir", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--top", type=int, default=25,
                        help="select this many PCs each by exclusive time and calls")
    args = parser.parse_args()
    try:
        result = collect(args.replay, args.ir, args.out, args.top)
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as exc:
        print(f"EE hot IR collection refused: {exc}", file=sys.stderr)
        return 1
    print(f"Collected {len(result['modules'])} verified IR modules for "
          f"{len(result['selected'])} selected PCs: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
