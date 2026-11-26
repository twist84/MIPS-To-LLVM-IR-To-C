#!/usr/bin/env python3
"""Isolated selective Oz experiment: preserve O5 cache and dispatch ownership.

Only try modules within a bounded guest-PC size stratum. Keep a replacement
only when both executable and object file bytes decrease. Never promote
this mixed manifest into the production cache. Requires independent replay.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

import llvmlite.binding as llvm

from audit_ee_entry_shadow import basename
from benchmark_ee_native_size import emit_native, sha
from ee_ownership_binary import native_executable_sections
from link_native_objects import load_inputs
from verify_ir import ir_pipeline_id, native_target_identity


def should_replace(base_exec, base_object, candidate_exec, candidate_object):
    return candidate_exec < base_exec and candidate_object < base_object


def candidate(path, original, record, row, symbol, triple, destination):
    raw, data = path.read_bytes(), original.read_bytes()
    if (sha(raw) != record["ir_sha256"] or sha(data) != record["sha256"] or
            len(data) != record["size_bytes"]):
        raise ValueError(f"stale input: {path.name}")
    executable, _ = native_executable_sections(data)
    if executable != row["native_executable_bytes"]:
        raise ValueError(f"audit executable bytes differ: {path.name}")
    trial = emit_native(raw, triple, symbol, "Oz", object_output=destination)
    accepted = should_replace(executable, len(data), trial["executable_bytes"],
                              trial["object_bytes"])
    if not accepted:
        destination.unlink()
    return {
        "module": path.name, "guest_pcs": row["guest_pcs"],
        "accepted": accepted, "baseline_executable_bytes": executable,
        "baseline_object_bytes": len(data), "baseline_sha256": sha(data),
        "ir_sha256": sha(raw), "trial": trial,
        "executable_saving": executable - trial["executable_bytes"] if accepted else 0,
        "object_saving": len(data) - trial["object_bytes"] if accepted else 0,
    }


def stage(ir, objects, audit, pilot_path, output, max_pcs, jobs):
    ir, objects, output = ir.resolve(), objects.resolve(), output.resolve()
    if output.exists() or output.is_relative_to(ir) or output.is_relative_to(objects):
        raise ValueError("use a new output directory outside original caches")
    metadata_path = ir / "ee_functions.json"
    metadata_bytes = metadata_path.read_bytes()
    manifest_bytes = (objects / "objects.json").read_bytes()
    metadata, manifest, _, _ = load_inputs(metadata_path, objects)
    if (metadata != json.loads(metadata_bytes) or manifest != json.loads(manifest_bytes) or
            manifest.get("optimization_level") != 5 or
            manifest.get("ir_pipeline") != ir_pipeline_id(5) or
            manifest.get("native_target") != native_target_identity(5) or
            manifest.get("target_triple") != llvm.get_default_triple() or
            manifest.get("llvm_version") != ".".join(map(str, llvm.llvm_version_info))):
        raise ValueError("current inputs do not match the native O5 manifest")
    pilot = json.loads(pilot_path.read_text(encoding="utf-8"))
    if (pilot.get("schema") != 1 or pilot.get("mode") != "read_only_native_size_pilot" or
            pilot.get("manifest_sha256") != sha(manifest_bytes) or
            pilot.get("native_target") != manifest["native_target"] or
            not pilot.get("sample_modules") or
            pilot.get("sample_modules") != pilot.get("reproduced_o5_objects")):
        raise ValueError("pilot does not reproduce the current O5 cache")
    rows = json.loads(audit.read_text(encoding="utf-8"))["module_details"]
    by_name = {row["module"]: row for row in rows}
    functions = {basename(fn["ir_path"]): fn for fn in metadata["functions"]}
    records = {basename(item["input"]): item for item in manifest["objects"]}
    paths = {path.name: path for path in ir.glob("*.ll")}
    if (len(by_name) != len(rows) or len(functions) != len(metadata["functions"]) or
            len(records) != len(manifest["objects"]) or
            set(by_name) != set(functions) or set(functions) != set(records) or
            set(records) != set(paths) or pilot["population_modules"] != len(rows)):
        raise ValueError("audit, pilot, metadata and object inventories differ")
    selected = sorted(name for name, row in by_name.items()
                      if 1 <= row["guest_pcs"] <= max_pcs)
    if not selected:
        raise ValueError("no selected modules")
    # Verify selected and unchanged IR before linking. Source files are
    # not staged or rewritten; the metadata remains byte-identical.
    for name, path in paths.items():
        if sha(path.read_bytes()) != records[name]["ir_sha256"]:
            raise ValueError(f"IR digest mismatch: {name}")
    output.mkdir(parents=True, exist_ok=False)
    staged = output / "objects"
    staged.mkdir()
    results = {}
    start = time.monotonic()
    # Use bounded batches with fresh pools instead of max_tasks_per_child:
    # Python 3.12 can hang when workers recycle with queued futures.
    batch_size = 128 * jobs
    for first in range(0, len(selected), batch_size):
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            futures = {}
            for name in selected[first:first + batch_size]:
                item = records[name]
                futures[pool.submit(candidate, paths[name], objects / item["object"],
                                    item, by_name[name], functions[name]["host_symbol"],
                                    manifest["target_triple"], staged / (Path(name).stem + ".oz.o"))] = name
            for future in as_completed(futures):
                row = future.result()
                results[row["module"]] = row
                if len(results) % 128 == 0 or len(results) == len(selected):
                    print(f"[{len(results)}/{len(selected)}] size trials complete", flush=True)
    accepted = {name: row for name, row in results.items() if row["accepted"]}
    if not accepted:
        raise ValueError("no smaller replacements")
    # Preserve ordering and all metadata, including host symbols and PCs.
    output_records = []
    for name, item in records.items():
        record = dict(item, object=str((objects / item["object"]).resolve()),
                      input=str(paths[name]))
        if name in accepted:
            trial = accepted[name]["trial"]
            record.update(object=Path(name).stem + ".oz.o",
                          size_bytes=trial["object_bytes"], sha256=trial["sha256"],
                          experimental_pipeline="Oz")
        output_records.append(record)
    (output / "ee_functions.json").write_bytes(metadata_bytes)
    # The existing linker requires a validated O5 host target. Keep that
    # target provenance and explicitly mark per-object mixed emission.
    # This isolated manifest is not an incremental O5 cache.
    mixed = dict(manifest, objects=output_records,
                 object_bytes=sum(item["size_bytes"] for item in output_records),
                 experimental_mixed_pipeline="O5-selective-Oz",
                 production_cache_compatible=False)
    (staged / "objects.json").write_text(json.dumps(mixed, indent=2) + "\n", encoding="utf-8")
    receipt = {
        "mode": "experimental_selective_native_size",
        "metadata_sha256": sha(metadata_bytes),
        "source_manifest_sha256": sha(manifest_bytes),
        "native_target": manifest["native_target"],
        "source_functions": len(functions), "retained_functions": len(functions),
        "max_guest_pcs": max_pcs, "attempted_modules": len(selected),
        "replaced_modules": len(accepted),
        "executable_saving": sum(row["executable_saving"] for row in results.values()),
        "object_saving": sum(row["object_saving"] for row in results.values()),
        "wall_seconds": time.monotonic() - start,
        "runtime_validation_required": True,
        "modules": [results[name] for name in sorted(results)],
    }
    (output / "size-plan.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    parser.add_argument("objects", type=Path)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--pilot", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-guest-pcs", type=int, default=32)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--link", action="store_true")
    args = parser.parse_args()
    try:
        if not 1 <= args.jobs <= 16 or not 1 <= args.max_guest_pcs <= 128:
            raise ValueError("invalid worker count or guest-PC threshold")
        if os.environ.get("SDBZ_NATIVE_ALU_CERTIFICATES") == "1":
            raise ValueError("disable native ALU certificates for experiment")
        receipt = stage(args.ir, args.objects, args.baseline, args.pilot,
                        args.output, args.max_guest_pcs, args.jobs)
        print(f"Selected Oz: {receipt['replaced_modules']}/{receipt['attempted_modules']} "
              f"replacements, {receipt['executable_saving']:,} executable bytes saved", flush=True)
        if args.link:
            name = ("game-translated.dll" if os.name == "nt" else
                    "libgame-translated.dylib" if sys.platform == "darwin" else "libgame-translated.so")
            library = args.output / name
            subprocess.run([sys.executable, str(Path(__file__).with_name("link_native_objects.py")),
                            str(args.output / "ee_functions.json"), str(args.output / "objects"),
                            str(library)], check=True)
            data = library.read_bytes()
            receipt.update(linked_library_bytes=len(data), linked_library_sha256=sha(data),
                           linked_executable_raw_bytes=native_executable_sections(data)[0])
            (args.output / "size-plan.json").write_text(
                json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, RuntimeError, TypeError, subprocess.CalledProcessError) as error:
        print(f"EE size experiment refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
