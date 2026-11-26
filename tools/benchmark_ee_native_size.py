#!/usr/bin/env python3
"""Read-only paired O5 / full O2 / Os / Oz native EE size experiment.

Use current verified O5 cache inputs. Emit independent temporary LLVM
modules in memory for each trial; never write IR or native cache objects.
All alternatives use the exact same host-native backend O2 target.
Size measurements alone do not authorize a compiler profile change.
"""
import argparse
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import sys
import time

import llvmlite.binding as llvm

from audit_ee_entry_shadow import basename
from ee_ownership_binary import native_executable_sections
from verify_ir import get_target_machine, ir_pipeline_id, native_target_identity, uses_ir_pipeline

MODES = ("O5", "O2", "Os", "Oz")
BANDS = ((1, 4), (5, 16), (17, 32), (33, 128), (129, 512), (513, None))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def band_for(count):
    for low, high in BANDS:
        if count >= low and (high is None or count <= high):
            return f"{low}-{high}" if high is not None else f"{low}+"
    raise ValueError("invalid guest PC count")


def select_modules(rows, per_band, top, include=()):
    """Stratify by guest instruction count and current pipeline kind later.

    Within each size band select uniformly spaced ranks by native bytes.
    Include the largest executable modules and explicit PC selections.
    Selection is deterministic; this is not a random population estimate.
    """
    groups = defaultdict(list)
    for row in rows:
        groups[band_for(row["guest_pcs"])].append(row)
    selected = defaultdict(set)
    def rank(row):
        return row["native_executable_bytes"], row["module"]
    for band, members in groups.items():
        ordered = sorted(members, key=rank)
        count = min(per_band, len(ordered))
        if count == 1:
            indexes = [len(ordered) // 2]
        elif count > 1:
            indexes = [i * (len(ordered) - 1) // (count - 1) for i in range(count)]
        else:
            indexes = []
        for i in indexes:
            selected[ordered[i]["module"]].add("band:" + band)
    for row in sorted(rows, key=rank, reverse=True)[:top]:
        selected[row["module"]].add("largest")
    for name in include:
        if not any(row["module"] == name for row in rows):
            raise ValueError(f"requested module is not in the audit: {name}")
        selected[name].add("explicit")
    return {name: sorted(reasons) for name, reasons in sorted(selected.items())}


def emit_native(raw, triple, symbol, mode):
    started = time.monotonic()
    machine = get_target_machine(5, triple)
    if mode not in MODES:
        raise ValueError("unsupported experimental mode")
    size_level = ({"O2": 0, "Os": 1, "Oz": 2}[mode] if mode != "O5" else
                  0 if uses_ir_pipeline(5, raw) else None)
    with llvm.create_context() as context, llvm.parse_assembly(
            raw.decode("utf-8"), context=context) as module:
        module.triple = triple
        module.data_layout = str(machine.target_data)
        module.verify()
        # The guest entry must remain a defined external symbol.
        def public_definitions():
            return sorted(fn.name for fn in module.functions
                          if not fn.is_declaration and fn.linkage == llvm.Linkage.external)
        public = public_definitions()
        if symbol not in public:
            raise ValueError("guest entry symbol missing from LLVM input")
        if size_level is not None:
            with llvm.create_pipeline_tuning_options(2, size_level) as tuning:
                with llvm.create_pass_builder(machine, tuning) as builder:
                    with builder.getModulePassManager() as passes:
                        passes.run(module, builder)
            module.verify()
        if public_definitions() != public:
            raise ValueError("experimental pipeline changed external function inventory")
        data = machine.emit_object(module)
    executable, _ = native_executable_sections(data)
    return {"object_bytes": len(data), "executable_bytes": executable,
            "sha256": sha(data), "seconds": time.monotonic() - started,
            "middle_end_size_level": size_level}


def measure(path, object_path, item, symbol, row, reasons, triple):
    raw = path.read_bytes()
    obj = object_path.read_bytes()
    if (sha(raw) != item["ir_sha256"] or sha(obj) != item["sha256"] or
            len(obj) != item["size_bytes"]):
        raise ValueError(f"stale IR or cached object: {path.name}")
    cached, _ = native_executable_sections(obj)
    if cached != row["native_executable_bytes"]:
        raise ValueError(f"audit executable size differs: {path.name}")
    variants = {mode: emit_native(raw, triple, symbol, mode) for mode in MODES}
    base = variants["O5"]
    for result in variants.values():
        result["executable_delta_vs_reemitted_o5"] = (
            result["executable_bytes"] - base["executable_bytes"])
        result["object_delta_vs_reemitted_o5"] = result["object_bytes"] - base["object_bytes"]
    return {
        "module": path.name, "guest_pcs": row["guest_pcs"],
        "band": band_for(row["guest_pcs"]), "reasons": reasons,
        "shared_guest_pcs": row["shared_guest_pcs"],
        "ir_sha256": sha(raw), "cached_object_sha256": sha(obj),
        "cached_executable_bytes": cached, "cached_object_bytes": len(obj),
        "o5_reemission_matches_cache": base["sha256"] == item["sha256"],
        "o5_uses_middle_end": uses_ir_pipeline(5, raw),
        "variants": variants,
    }


def summarize(results):
    totals = {}
    for mode in MODES:
        totals[mode] = {
            "executable_bytes": sum(row["variants"][mode]["executable_bytes"] for row in results),
            "object_bytes": sum(row["variants"][mode]["object_bytes"] for row in results),
            "compile_seconds_sum": sum(row["variants"][mode]["seconds"] for row in results),
            "smaller_modules": sum(row["variants"][mode]["executable_delta_vs_reemitted_o5"] < 0
                                   for row in results),
            "larger_modules": sum(row["variants"][mode]["executable_delta_vs_reemitted_o5"] > 0
                                  for row in results),
        }
    baseline = totals["O5"]["executable_bytes"]
    for total in totals.values():
        total["executable_delta_vs_o5"] = total["executable_bytes"] - baseline
    return totals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    parser.add_argument("objects", type=Path)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--per-band", type=int, default=8)
    parser.add_argument("--top", type=int, default=8)
    parser.add_argument("--include", action="append", default=[],
                        help="explicit .ll basename; repeatable")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    try:
        ir, objects, output = args.ir.resolve(), args.objects.resolve(), args.json.resolve()
        if (output.exists() or output.is_relative_to(ir) or output.is_relative_to(objects)):
            raise ValueError("use a new report path outside caches")
        if not 1 <= args.jobs <= 16 or args.per_band < 0 or args.top < 0:
            raise ValueError("invalid selection/worker counts")
        metadata = json.loads((ir / "ee_functions.json").read_text(encoding="utf-8"))
        manifest_bytes = (objects / "objects.json").read_bytes()
        manifest = json.loads(manifest_bytes)
        if (not manifest.get("complete") or manifest.get("optimization_level") != 5 or
                manifest.get("ir_pipeline") != ir_pipeline_id(5) or
                manifest.get("native_target") != native_target_identity(5) or
                manifest.get("target_triple") != llvm.get_default_triple() or
                manifest.get("llvm_version") != ".".join(map(str, llvm.llvm_version_info))):
            raise ValueError("current LLVM/CPU does not match the complete O5 manifest")
        rows = json.loads(args.baseline.read_text(encoding="utf-8"))["module_details"]
        by_name = {row["module"]: row for row in rows}
        functions = {basename(fn["ir_path"]): fn for fn in metadata["functions"]}
        records = {basename(item["input"]): item for item in manifest["objects"]}
        paths = {path.name: path for path in ir.glob("*.ll")}
        if (len(by_name) != len(rows) or len(functions) != len(metadata["functions"]) or
                len(records) != len(manifest["objects"]) or
                set(by_name) != set(functions) or set(functions) != set(records) or
                set(records) != set(paths)):
            raise ValueError("audit, metadata, IR and object inventories differ")
        selected = select_modules(rows, args.per_band, args.top, args.include)
        if not selected:
            raise ValueError("no modules selected")
        results = []
        started = time.monotonic()
        with ProcessPoolExecutor(max_workers=args.jobs) as pool:
            futures = {}
            for name, reasons in selected.items():
                item, fn = records[name], functions[name]
                future = pool.submit(measure, paths[name], objects / item["object"], item,
                                     fn["host_symbol"], by_name[name], reasons,
                                     manifest["target_triple"])
                futures[future] = name
            for future in as_completed(futures):
                row = future.result()
                results.append(row)
                print(f"[{len(results)}/{len(selected)}] {row['module']} " +
                      " ".join(f"{mode}={row['variants'][mode]['executable_bytes']:,}"
                               for mode in MODES), flush=True)
        results.sort(key=lambda row: row["module"])
        totals = summarize(results)
        groups = defaultdict(list)
        for row in results:
            groups[row["band"]].append(row)
        report = {
            "schema": 1, "mode": "read_only_native_size_pilot",
            "manifest_sha256": sha(manifest_bytes), "native_target": manifest["native_target"],
            "population_modules": len(rows), "sample_modules": len(results),
            "reproduced_o5_objects": sum(row["o5_reemission_matches_cache"] for row in results),
            "wall_seconds": time.monotonic() - started, "totals": totals,
            "bands": {band: {"modules": len(group), "totals": summarize(group)}
                      for band, group in sorted(groups.items())},
            "modules": results,
            "limitations": ["selected size-stratified sample, not a population estimate",
                            "no native execution or linked library measurement",
                            "same backend O2; experimental changes are middle-end pipelines only",
                            "not a production optimization profile or cache manifest"],
        }
        with output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
        print(f"O5 cache objects reproduced: {report['reproduced_o5_objects']}/{len(results)}")
        for mode, total in totals.items():
            print(f"{mode}: {total['executable_bytes']:,} executable bytes "
                  f"({total['executable_delta_vs_o5']:+,} vs O5)")
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(f"EE native size pilot refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
