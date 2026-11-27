#!/usr/bin/env python3
"""Share the exact quadword slow-path helper on top of shared accounting.

Only modules defining the helper are re-emitted. All other verified
native objects are referenced by absolute path in the isolated overlay.
"""
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
from build_ee_helper_experiment import (
    FAMILY, COUNTER, SHARED, SHARED_COUNTER, refs, rewrite, rewrite_counter)
from ee_ownership_binary import native_executable_sections
from link_native_objects import load_inputs

QUAD = "ee_store_quadword"
SHARED_QUAD = "ee_shared_store_quadword"
GUARD = "ee_guard_guest_store_trace"
DEPS = {GUARD, "ee_mem_is_quadword_fifo", "ee_mem_write128_words", "ee_mem_write64"}
ABI = b"ptr %rt, i32 %pc, i32 %op, i32 %effective, i32 %affected, i64 %lo, i64 %hi, i64 %base, i64 %sp, i64 %fp, i64 %ra"
TYPES = b"ptr, i32, i32, i32, i32, i64, i64, i64, i64, i64, i64"
DECLARATIONS = {
    b"ee_mem_is_quadword_fifo": (b"i32", b"ptr, i32"),
    b"ee_mem_write128_words": (b"void", b"ptr, i32, i64, i64"),
    b"ee_mem_write64": (b"void", b"ptr, i32, i64"),
}


def body_of(raw, name):
    for match in HEADER.finditer(raw):
        if match[1].decode("ascii") == name:
            end = END.search(raw, match.end())
            if end is not None:
                return raw[match.start():end.end()]
    raise ValueError(f"missing helper definition: {name}")


def rewrite_quadword(raw, prior, guest, expected, owner):
    inventory = ir_inventory(raw, guest)
    if (QUAD not in inventory or GUARD not in inventory or
            inventory[QUAD]["body_sha256"] != expected[QUAD] or
            inventory[GUARD]["body_sha256"] != expected[GUARD] or
            set(inventory[QUAD]["direct_dependencies"]) != DEPS or
            inventory[GUARD]["direct_dependencies"] != ["ee_trace_guest_store"]):
        raise ValueError("quadword helper or dependency differs")
    body = body_of(raw, QUAD)
    guard = body_of(raw, GUARD)
    header = b"define internal void @" + QUAD.encode() + b"(" + ABI + b") alwaysinline {"
    if (body.splitlines()[0] != header or body_of(prior, QUAD) != body or
            body_of(prior, GUARD) != guard or refs(prior, SHARED_QUAD) or
            refs(prior, QUAD) != 1 + inventory[QUAD]["guest_direct_calls"] +
            inventory[QUAD]["helper_direct_calls"]):
        raise ValueError("quadword helper ABI or symbol usage differs")
    for name, (ret, params) in DECLARATIONS.items():
        pattern = rb"^declare[ \t]+" + ret + rb"[ \t]+@" + name + rb"[ \t]*\(" + re.escape(params) + rb"\)[ \t]*\r?$"
        if len(re.findall(pattern, prior, re.M)) != 1:
            raise ValueError("quadword external dependency ABI differs")
    if owner:
        replacement = body.replace(header, b"define void @" + SHARED_QUAD.encode() +
                                   b"(" + ABI + b") noinline {", 1)
    else:
        replacement = b"declare void @" + SHARED_QUAD.encode() + b"(" + TYPES + b") noinline"
    result = prior.replace(body, replacement, 1)
    result = re.sub(rb"@" + QUAD.encode() + rb"(?![\w.$])", b"@" + SHARED_QUAD.encode(), result)
    if refs(result, QUAD):
        raise ValueError("incomplete quadword rewrite")
    return result


def verify_prior(raw, data, record, row, guest, family, counter, observation_owner):
    mode = record.get("experimental_pipeline", "O5")
    if (mode not in ("O5", "Oz") or row["mode"] != mode or
            sha(raw) != record["ir_sha256"] or row["source_ir_sha256"] != record["ir_sha256"] or
            sha(data) != record["sha256"] or row["trial"]["sha256"] != record["sha256"] or
            len(data) != record["size_bytes"] or row["trial"]["object_bytes"] != len(data) or
            row["trial"]["executable_bytes"] != native_executable_sections(data)[0] or
            row["shared_owner"] != observation_owner or
            row["transformed_ir_sha256"] != record.get("transformed_ir_sha256")):
        raise ValueError("prior counter object or IR identity differs")
    return mode


def trial(path, obj, record, row, guest, family, counter, expected,
          observation_owner, quad_owner, triple, out):
    raw, data = path.read_bytes(), obj.read_bytes()
    mode = verify_prior(raw, data, record, row, guest, family, counter, observation_owner)
    before = rewrite_counter(raw, rewrite(raw, guest, family, observation_owner),
                             guest, counter, observation_owner)
    if sha(before) != row["transformed_ir_sha256"]:
        raise ValueError(f"prior transformation differs: {path.name}")
    changed = rewrite_quadword(raw, before, guest, expected, quad_owner)
    result = emit_native(changed, triple, guest, mode, object_output=out)
    return {"module": path.name, "mode": mode, "quadword_owner": quad_owner,
            "source_ir_sha256": sha(raw), "transformed_ir_sha256": sha(changed),
            "source_object_sha256": sha(data), "source_object_bytes": len(data),
            "source_executable_bytes": native_executable_sections(data)[0],
            "removed_ir_bytes": len(before) - len(changed), "trial": result}


def stage(ir, experiment, inventory_path, output, jobs):
    ir, experiment, output = (p.resolve() for p in (ir, experiment, output))
    if output.exists() or output.is_relative_to(ir) or output.is_relative_to(experiment):
        raise ValueError("use a new output outside inputs")
    metadata_path = ir / "ee_functions.json"
    meta_bytes = metadata_path.read_bytes()
    objects = experiment / "objects"
    manifest_bytes = (objects / "objects.json").read_bytes()
    metadata, manifest, _, _ = load_inputs(metadata_path, objects)
    plan = json.loads((experiment / "helper-plan.json").read_text(encoding="utf-8"))
    evidence = json.loads(inventory_path.read_text(encoding="utf-8"))
    dll = experiment / "game-translated.dll"
    if (plan.get("mode") != "experimental_shared_counter" or
            plan.get("metadata_sha256") != sha(meta_bytes) or
            (experiment / "ee_functions.json").read_bytes() != meta_bytes or
            plan.get("native_target") != manifest.get("native_target") or
            plan.get("linked_library_sha256") != sha(dll.read_bytes()) or
            plan.get("linked_library_bytes") != dll.stat().st_size or
            manifest.get("experimental_mixed_pipeline") != "O5-selective-Oz-shared-observation-counter" or
            manifest.get("production_cache_compatible") is not False or
            not manifest["target_triple"].endswith("windows-msvc") or
            evidence.get("mode") != "read_only_ee_helper_inventory" or
            evidence.get("metadata_sha256") != sha(meta_bytes)):
        raise ValueError("expected verified shared-counter input")
    names = {Path(f["ir_path"]).name: f["host_symbol"] for f in metadata["functions"]}
    records = {Path(r["input"]).name: r for r in manifest["objects"]}
    rows = {r["module"]: r for r in plan["modules"]}
    paths = {p.name: p for p in ir.glob("*.ll")}
    count = len(names)
    if (set(names) != set(records) or set(names) != set(paths) or set(names) != set(rows) or
            count != len(metadata["functions"]) or count != len(plan["modules"]) or
            count != plan["source_functions"] or count != plan["retained_functions"] or
            count != evidence["modules"]):
        raise ValueError("quadword inventory differs")
    groups = {}
    for g in evidence["groups"]:
        if g["name"] in (*FAMILY, COUNTER, QUAD, GUARD):
            if g["name"] in groups:
                raise ValueError("ambiguous helper bodies")
            groups[g["name"]] = g
    family = {n: groups[n]["body_sha256"] for n in FAMILY}
    counter = groups[COUNTER]["body_sha256"]
    expected = {n: groups[n]["body_sha256"] for n in (QUAD, GUARD)}
    observation_owner = plan["shared_owner"]
    if (family != plan["family_body_sha256"] or counter != plan["counter_body_sha256"] or
            observation_owner != min(names)):
        raise ValueError("prior transformation evidence differs")
    selected = []
    started = time.monotonic()
    for name in sorted(names):
        raw = paths[name].read_bytes()
        data = (objects / records[name]["object"]).read_bytes()
        record, row = records[name], rows[name]
        verify_prior(raw, data, record, row, names[name], family, counter, name == observation_owner)
        symbols = record["defined_symbols"]
        if (record.get("experimental_shared_counter") is not True or
                record.get("experimental_shared_observation") is not True or
                (SHARED in symbols) != (name == observation_owner) or
                (SHARED_COUNTER in symbols) != (name == observation_owner) or
                SHARED_QUAD in symbols or refs(raw, SHARED_QUAD)):
            raise ValueError(f"unexpected symbol inventory: {name}")
        if refs(raw, QUAD):
            selected.append(name)
    if not selected or len(selected) != groups[QUAD]["definitions"]:
        raise ValueError("quadword selection differs from audited inventory")
    owner = selected[0]
    output.mkdir(parents=True)
    staged = output / "objects"
    staged.mkdir()
    results = {}
    print(f"Re-emitting {len(selected):,} modules; reusing {count-len(selected):,}", flush=True)
    for first in range(0, len(selected), 128 * jobs):
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            futures = [pool.submit(trial, paths[name], objects / records[name]["object"],
                                   records[name], rows[name], names[name], family, counter, expected,
                                   name == observation_owner, name == owner, manifest["target_triple"],
                                   staged / (Path(name).stem + ".quad.o"))
                       for name in selected[first:first + 128 * jobs]]
            for future in as_completed(futures):
                result = future.result()
                results[result["module"]] = result
                if len(results) % 128 == 0 or len(results) == len(selected):
                    print(f"[{len(results)}/{len(selected)}] shared quadword emission", flush=True)
    entries = []
    for name, record in records.items():
        if name not in results:
            entries.append(dict(record, object=str((objects / record["object"]).resolve())))
            continue
        row = results[name]
        symbols = list(record["defined_symbols"])
        if name == owner:
            symbols.append(SHARED_QUAD)
        entries.append(dict(record, object=Path(name).stem + ".quad.o",
                            size_bytes=row["trial"]["object_bytes"],
                            sha256=row["trial"]["sha256"], defined_symbols=symbols,
                            transformed_ir_sha256=row["transformed_ir_sha256"],
                            experimental_shared_quadword=True))
    (output / "ee_functions.json").write_bytes(meta_bytes)
    overlay = dict(manifest, objects=entries, object_bytes=sum(x["size_bytes"] for x in entries),
                   experimental_mixed_pipeline="O5-selective-Oz-shared-observation-counter-quadword",
                   production_cache_compatible=False)
    (staged / "objects.json").write_text(json.dumps(overlay, indent=2) + "\n", encoding="utf-8")
    return {
        "mode": "experimental_shared_quadword", "metadata_sha256": sha(meta_bytes),
        "source_manifest_sha256": sha(manifest_bytes), "native_target": manifest["native_target"],
        "prior_library_sha256": plan["linked_library_sha256"],
        "source_functions": count, "retained_functions": count,
        "quadword_owner": owner, "shared_owner": observation_owner, "quadword_body_sha256": expected,
        "recompiled_modules": len(results), "reused_modules": count-len(results),
        "removed_ir_bytes": sum(x["removed_ir_bytes"] for x in results.values()),
        "executable_saving": sum(x["source_executable_bytes"] - x["trial"]["executable_bytes"] for x in results.values()),
        "object_saving": sum(x["source_object_bytes"] - x["trial"]["object_bytes"] for x in results.values()),
        "wall_seconds": time.monotonic() - started, "runtime_validation_required": True,
        "modules": [results[name] for name in selected],
    }


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
        if not 1 <= args.jobs <= 16 or os.name != "nt" or os.environ.get("SDBZ_NATIVE_ALU_CERTIFICATES") == "1":
            raise ValueError("Windows experiment requires 1–16 jobs and disabled certificates")
        receipt = stage(args.ir, args.experiment, args.inventory, args.output, args.jobs)
        path = args.output / "helper-plan.json"
        path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        if args.link:
            library = args.output / "game-translated.dll"
            subprocess.run([sys.executable, str(Path(__file__).with_name("link_native_objects.py")),
                            str(args.output / "ee_functions.json"), str(args.output / "objects"),
                            str(library)], check=True)
            data = library.read_bytes()
            receipt.update(linked_library_bytes=len(data), linked_library_sha256=sha(data),
                           linked_executable_raw_bytes=native_executable_sections(data)[0])
            path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(f"Shared quadword: {receipt['executable_saving']:,} executable bytes saved", flush=True)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"EE quadword experiment refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
