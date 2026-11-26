#!/usr/bin/env python3
"""Isolated exact-body sharing of the cold EE instruction observer family."""
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from audit_ee_helper_inventory import HEADER, END, ir_inventory
from benchmark_ee_native_size import emit_native, sha
from ee_ownership_binary import native_executable_sections
from link_native_objects import load_inputs

PUBLISH = "ee_publish_instruction_observation"
SHARED = "ee_shared_publish_instruction_observation"
LEAVES = ("ee_observe_timing_event", "ee_trace_instruction_observation")
FAMILY = (PUBLISH, *LEAVES)


def refs(raw, name):
    return len(re.findall(rb"@" + name.encode() + rb"(?![\w.$])", raw))


def rewrite(raw, guest, expected, owner):
    inventory = ir_inventory(raw, guest)
    if refs(raw, SHARED):
        raise ValueError("shared observation name already present")
    bodies = {}
    for match in HEADER.finditer(raw):
        name = match[1].decode("ascii")
        if name in FAMILY:
            end = END.search(raw, match.end())
            if end is None:
                raise ValueError("unterminated observation helper")
            body = raw[match.start():end.end()]
            if (name not in inventory or sha(body) != expected[name] or
                    not body.startswith(b"define internal void @")):
                raise ValueError(f"observation helper body differs: {name}")
            bodies[name] = body
    if set(bodies) != set(FAMILY):
        raise ValueError("incomplete observation helper family")
    if (set(inventory[PUBLISH]["direct_dependencies"]) != set(LEAVES) or
            any(inventory[name]["direct_dependencies"] for name in LEAVES)):
        raise ValueError("unexpected observation dependencies")
    # Leaf symbols cannot escape the family. Reject address-taken or
    # unexpected symbol uses, including references from other helpers.
    for name in LEAVES:
        if refs(raw, name) != 2 or refs(bodies[PUBLISH], name) != 1:
            raise ValueError(f"observation leaf escapes family: {name}")
    publish = bodies[PUBLISH]
    header = publish.splitlines()[0]
    if header != (b"define internal void @" + PUBLISH.encode() +
                  b"(ptr %rt, i32 %pc, i32 %opcode, i32 %kind, i64 %attempt, i64 %slot) noinline cold {"):
        raise ValueError("unexpected publication ABI")
    if refs(raw, PUBLISH) != 1 + inventory[PUBLISH]["guest_direct_calls"] + inventory[PUBLISH]["helper_direct_calls"]:
        raise ValueError("unexpected observation publisher symbol use")
    if owner:
        result = raw.replace(publish, publish.replace(b"define internal void @", b"define void @", 1), 1)
    else:
        declaration = (b"declare void @" + PUBLISH.encode() +
                       b"(ptr, i32, i32, i32, i64, i64) noinline cold")
        result = raw.replace(publish, declaration, 1)
        for name in LEAVES:
            result = result.replace(bodies[name], b"", 1)
    result = re.sub(rb"@" + PUBLISH.encode() + rb"(?![\w.$])",
                    b"@" + SHARED.encode(), result)
    if any(refs(result, name) for name in (PUBLISH, *(() if owner else LEAVES))):
        raise ValueError("incomplete observation rewrite")
    return result


def trial(path, obj, record, guest, expected, owner, triple, out):
    raw, data = path.read_bytes(), obj.read_bytes()
    if (sha(raw) != record["ir_sha256"] or sha(data) != record["sha256"] or
            len(data) != record["size_bytes"]):
        raise ValueError(f"stale helper experiment input: {path.name}")
    mode = record.get("experimental_pipeline", "O5")
    if mode not in ("O5", "Oz"):
        raise ValueError("unexpected prior native policy")
    changed = rewrite(raw, guest, expected, owner)
    result = emit_native(changed, triple, guest, mode, object_output=out)
    return {"module": path.name, "mode": mode, "shared_owner": owner,
            "source_ir_sha256": sha(raw), "transformed_ir_sha256": sha(changed),
            "source_object_sha256": sha(data), "source_object_bytes": len(data),
            "source_executable_bytes": native_executable_sections(data)[0],
            "removed_ir_bytes": len(raw) - len(changed), "trial": result}


def stage(ir, experiment, inventory_path, output, jobs):
    ir, experiment, output = (p.resolve() for p in (ir, experiment, output))
    if output.exists() or output.is_relative_to(ir) or output.is_relative_to(experiment):
        raise ValueError("use a new directory outside existing inputs")
    metadata_path = ir / "ee_functions.json"
    metadata_bytes = metadata_path.read_bytes()
    objects = experiment / "objects"
    manifest_bytes = (objects / "objects.json").read_bytes()
    metadata, manifest, _, _ = load_inputs(metadata_path, objects)
    evidence = json.loads(inventory_path.read_text(encoding="utf-8"))
    plan = json.loads((experiment / "size-plan.json").read_text(encoding="utf-8"))
    if (evidence.get("mode") != "read_only_ee_helper_inventory" or
            evidence.get("metadata_sha256") != sha(metadata_bytes) or
            evidence.get("mixed_manifest_sha256") != sha(manifest_bytes) or
            evidence.get("original_manifest_sha256") != plan.get("source_manifest_sha256") or
            plan.get("metadata_sha256") != sha(metadata_bytes) or
            plan.get("mode") != "experimental_selective_native_size" or
            (experiment / "ee_functions.json").read_bytes() != metadata_bytes or
            manifest.get("experimental_mixed_pipeline") != "O5-selective-Oz" or
            manifest.get("production_cache_compatible") is not False or
            not manifest["target_triple"].endswith("windows-msvc")):
        raise ValueError("helper evidence does not match the selective Oz input")
    names = {Path(f["ir_path"]).name: f["host_symbol"] for f in metadata["functions"]}
    records = {Path(r["input"]).name: r for r in manifest["objects"]}
    paths = {p.name: p for p in ir.glob("*.ll")}
    count = len(names)
    if (set(names) != set(records) or set(names) != set(paths) or
            count != len(metadata["functions"]) or count != evidence["modules"] or
            count != plan["attempted_modules"]):
        raise ValueError("helper experiment inventory differs")
    expected = {}
    for group in evidence["groups"]:
        name = group["name"]
        if name in FAMILY:
            if name in expected or group["definitions"] != count:
                raise ValueError("observation helper not identical across inventory")
            expected[name] = group["body_sha256"]
    if set(expected) != set(FAMILY):
        raise ValueError("missing observation helper evidence")
    owner = min(names)
    output.mkdir(parents=True)
    staged = output / "objects"
    staged.mkdir()
    started = time.monotonic()
    results = {}
    ordered = sorted(names)
    for first in range(0, count, 128 * jobs):
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            futures = [pool.submit(trial, paths[name], objects / records[name]["object"],
                                   records[name], names[name], expected, name == owner,
                                   manifest["target_triple"], staged / (Path(name).stem + ".shared.o"))
                       for name in ordered[first:first + 128 * jobs]]
            for future in as_completed(futures):
                result = future.result()
                results[result["module"]] = result
                if len(results) % 128 == 0 or len(results) == count:
                    print(f"[{len(results)}/{count}] shared observation emission", flush=True)
    entries = []
    for name, record in records.items():
        row = results[name]
        symbols = list(record["defined_symbols"])
        if name == owner:
            symbols.append(SHARED)
        entries.append(dict(record, object=Path(name).stem + ".shared.o",
                            size_bytes=row["trial"]["object_bytes"],
                            sha256=row["trial"]["sha256"], defined_symbols=symbols,
                            transformed_ir_sha256=row["transformed_ir_sha256"],
                            experimental_shared_observation=True))
    (output / "ee_functions.json").write_bytes(metadata_bytes)
    overlay = dict(manifest, objects=entries,
                   object_bytes=sum(x["size_bytes"] for x in entries),
                   experimental_mixed_pipeline="O5-selective-Oz-shared-observation",
                   production_cache_compatible=False)
    (staged / "objects.json").write_text(json.dumps(overlay, indent=2) + "\n", encoding="utf-8")
    receipt = {
        "mode": "experimental_shared_observation", "metadata_sha256": sha(metadata_bytes),
        "source_manifest_sha256": sha(manifest_bytes), "native_target": manifest["native_target"],
        "source_functions": count, "retained_functions": count,
        "shared_owner": owner, "family_body_sha256": expected,
        "removed_ir_bytes": sum(x["removed_ir_bytes"] for x in results.values()),
        "executable_saving": sum(x["source_executable_bytes"] - x["trial"]["executable_bytes"] for x in results.values()),
        "object_saving": sum(x["source_object_bytes"] - x["trial"]["object_bytes"] for x in results.values()),
        "wall_seconds": time.monotonic() - started, "runtime_validation_required": True,
        "modules": [results[name] for name in ordered],
    }
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    parser.add_argument("--experiment", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--link", action="store_true")
    args = parser.parse_args()
    try:
        if not 1 <= args.jobs <= 16 or os.environ.get("SDBZ_NATIVE_ALU_CERTIFICATES") == "1":
            raise ValueError("invalid jobs or enabled native ALU certificates")
        receipt = stage(args.ir, args.experiment, args.inventory, args.output, args.jobs)
        path = args.output / "helper-plan.json"
        path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        if args.link:
            if os.name != "nt":
                raise ValueError("Windows retail experiment required")
            library = args.output / "game-translated.dll"
            subprocess.run([sys.executable, str(Path(__file__).with_name("link_native_objects.py")),
                            str(args.output / "ee_functions.json"), str(args.output / "objects"),
                            str(library)], check=True)
            data = library.read_bytes()
            receipt.update(linked_library_bytes=len(data), linked_library_sha256=sha(data),
                           linked_executable_raw_bytes=native_executable_sections(data)[0])
            path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(f"Shared observation: {receipt['executable_saving']:,} executable bytes saved", flush=True)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"EE helper experiment refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
