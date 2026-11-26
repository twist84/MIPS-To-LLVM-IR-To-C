#!/usr/bin/env python3
"""Exact-body cross-module scalar helper sharing over the expanded overlay."""
import argparse
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from audit_ee_helper_inventory import ir_inventory, read_manifest, verified_object, sha
from audit_ee_scalar_memory_pilot import (outline, reconstruct_parent, verify_pilot_parent,
                                         plans_without_modules, read_json)
from benchmark_ee_native_size import emit_native
from build_ee_cop1_mul_experiment import rows_of, check_object
from build_ee_helper_experiment import refs
from build_ee_quadword_experiment import body_of
from ee_ownership_binary import native_executable_sections
from link_native_objects import load_inputs

LOCAL = "ee_pilot_scalar_"
SHARED = "ee_shared_scalar_"
HELPERS = tuple(LOCAL + operation + str(bits)
                for operation in ("read", "write") for bits in (8, 16, 32, 64))


def shared_name(name):
    return SHARED + name[len(LOCAL):]


def share_helpers(raw, guest, counts, expected, owners, module):
    inventory = ir_inventory(raw, guest)
    selected = {n for n in HELPERS if counts.get(n, 0)}
    if ({n for n in inventory if n.startswith(LOCAL)} != selected or
            any(refs(raw, shared_name(n)) for n in HELPERS)):
        raise ValueError("scalar helper inventory differs")
    result = raw
    for name in sorted(selected):
        operation, bits = re.fullmatch(r"(read|write)(8|16|32|64)", name[len(LOCAL):]).groups()
        ty = ("i" + bits).encode()
        write = operation == "write"
        ret = b"void" if write else ty
        args = b"ptr %rt, i32 %address" + (b", " + ty + b" %value" if write else b"")
        types = b"ptr, i32" + (b", " + ty if write else b"")
        target = "ee_mem_" + operation + bits
        body = body_of(raw, name)
        header = b"define internal " + ret + b" @" + name.encode() + b"(" + args + b") noinline {"
        info = inventory[name]
        if (body.splitlines()[0] != header or sha(body) != expected[name] or
                info["direct_dependencies"] != [target] or
                info["guest_direct_calls"] != counts[name] or info["helper_direct_calls"] or
                refs(raw, name) != counts[name] + 1):
            raise ValueError("scalar helper body, dependency or usage differs: " + name)
        declaration = (rb"^declare[ \t]+" + ret + rb"[ \t]+@" + target.encode() +
                       rb"[ \t]*\([ \t]*" + re.escape(types) + rb"[ \t]*\)[ \t]*\r?$")
        if len(re.findall(declaration, raw, re.M)) != 1 or any(
                m.group(1).decode() == target for m in
                re.finditer(rb"^define\b[^\r\n]*?@([\w.$]+)\(", raw, re.M)):
            raise ValueError("scalar fallback ABI differs: " + name)
        shared = shared_name(name).encode()
        if owners[name] == module:
            replacement = body.replace(header, b"define " + ret + b" @" + shared +
                                       b"(" + args + b") noinline {", 1)
        else:
            replacement = b"declare " + ret + b" @" + shared + b"(" + types + b") noinline"
        result = result.replace(body, replacement, 1)
        result = re.sub(rb"@" + name.encode() + rb"(?![\w.$])", b"@" + shared, result)
        if refs(result, name):
            raise ValueError("incomplete scalar rewrite")
    return result


def local_parent(task):
    name, guest, path, base_record, receipts, plans, scalar_row = task
    raw = path.read_bytes()
    if sha(raw) != base_record["ir_sha256"]:
        raise ValueError("original scalar IR differs: " + name)
    before, prior_row = reconstruct_parent(raw, guest, name, receipts, plans)
    if (prior_row["transformed_ir_sha256"] != base_record["transformed_ir_sha256"] or
            prior_row["trial"]["sha256"] != base_record["sha256"] or
            scalar_row["parent_ir_sha256"] != sha(before) or
            scalar_row["parent_object_sha256"] != base_record["sha256"] or
            scalar_row["original_ir_sha256"] != sha(raw)):
        raise ValueError("scalar parent reconstruction differs: " + name)
    local, counts = outline(before, guest)
    if counts != scalar_row["counts"] or sha(local) != scalar_row["candidate_ir_sha256"]:
        raise ValueError("local scalar outline differs: " + name)
    return raw, local, counts


def trial(task):
    local_task, obj_dir, record, triple, expected, owners, out = task
    name, guest = local_task[:2]
    scalar_row = local_task[-1]
    raw, local, counts = local_parent(local_task)
    data = verified_object(obj_dir, record, sha(raw))
    mode = record.get("experimental_pipeline", "O5")
    if (mode != scalar_row["mode"] or record["transformed_ir_sha256"] != sha(local) or
            sha(data) != scalar_row["candidate"]["sha256"] or
            native_executable_sections(data)[0] != scalar_row["candidate"]["executable_bytes"]):
        raise ValueError("local scalar object differs: " + name)
    changed = share_helpers(local, guest, counts, expected, owners, name)
    result = emit_native(changed, triple, guest, mode, object_output=out)
    return {"module": name, "mode": mode, "source_ir_sha256": sha(raw),
            "parent_ir_sha256": sha(local), "transformed_ir_sha256": sha(changed),
            "source_object_sha256": sha(data), "source_object_bytes": len(data),
            "source_executable_bytes": scalar_row["candidate"]["executable_bytes"],
            "shared_definitions": sorted(shared_name(n) for n in HELPERS if owners[n] == name),
            "removed_ir_bytes": len(local)-len(changed), "trial": result}


def stage(args):
    started = time.monotonic()
    ir = args.ir.resolve()
    folders = [p.resolve() for p in (args.counter_experiment, args.quadword_experiment,
                                    args.multiply_experiment, args.add_experiment)]
    pilot, expanded = args.pilot_experiment.resolve(), args.experiment.resolve()
    out = args.output.resolve()
    if out.exists() or any(out.is_relative_to(p) for p in (ir, *folders, pilot, expanded)):
        raise ValueError("use a new output outside input caches")
    meta = (ir / "ee_functions.json").read_bytes()
    functions = json.loads(meta)["functions"]
    names = {Path(f["ir_path"]).name: f["host_symbol"] for f in functions}
    if len(names) != len(functions):
        raise ValueError("duplicate module metadata")
    plans = [read_json(p / "helper-plan.json") for p in folders]
    manifests = [read_manifest(p / "objects", set(names)) for p in folders]
    modes = ("experimental_shared_counter", "experimental_shared_quadword",
             "experimental_shared_cop1_mul", "experimental_shared_cop1_add")
    for i, (folder, plan, (raw, manifest, _)) in enumerate(zip(folders, plans, manifests)):
        dll = folder / "game-translated.dll"
        if (plan["mode"] != modes[i] or plan["metadata_sha256"] != sha(meta) or
                (folder / "ee_functions.json").read_bytes() != meta or
                plan["retained_functions"] != len(names) or
                plan["native_target"] != manifest["native_target"] or
                manifest["native_target"] != manifests[-1][1]["native_target"] or
                manifest.get("production_cache_compatible") is not False or
                dll.stat().st_size != plan["linked_library_bytes"] or
                sha(dll.read_bytes()) != plan["linked_library_sha256"]):
            raise ValueError("prior experiment identity differs")
        if i and (plan["source_manifest_sha256"] != sha(manifests[i-1][0]) or
                  plan["prior_library_sha256"] != plans[i-1]["linked_library_sha256"]):
            raise ValueError("prior manifest chain differs")
    base_raw, base_manifest, base_records = manifests[-1]
    pilot_raw, pilot_manifest, pilot_records, pilot_plan, first = verify_pilot_parent(
        pilot, meta, names, base_raw, base_manifest, base_records, plans[-1])
    parent_raw, manifest, records, parent_plan, rest = verify_pilot_parent(
        expanded, meta, names, pilot_raw, pilot_manifest, pilot_records, pilot_plan, expanded=True)
    if (set(first) & set(rest) or parent_plan["retained_pilot_modules"] != len(first) or
            parent_plan["cumulative_scalar_modules"] != len(first)+len(rest)):
        raise ValueError("scalar overlay inventories overlap or differ")
    scalar = dict(first, **rest)
    if {n for n, r in records.items() if r.get("experimental_scalar_memory")} != set(scalar):
        raise ValueError("scalar overlay flags differ")
    rows = [rows_of(p) for p in plans]
    small_plans = plans_without_modules(plans)
    def task_for(n):
        return (n, names[n], ir / n, base_records[n],
                tuple(r.get(n) for r in rows), small_plans, scalar[n])
    owners, expected = {}, {}
    for helper in HELPERS:
        candidates = sorted(n for n, row in scalar.items() if row["counts"].get(helper, 0))
        if not candidates:
            raise ValueError("missing scalar width/direction: " + helper)
        # Prefer an O5 owner for the common implementation, then stable name.
        owner = min(candidates, key=lambda n: (scalar[n]["mode"] != "O5", n))
        _, local, _ = local_parent(task_for(owner))
        owners[helper] = owner
        expected[helper] = sha(body_of(local, helper))
    out.mkdir(parents=True)
    staged = out / "objects"
    staged.mkdir()
    results = []
    selected = sorted(scalar)
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for start in range(0, len(selected), args.jobs*128):
            batch = selected[start:start+args.jobs*128]
            tasks = [(task_for(n), expanded / "objects", records[n], manifest["target_triple"],
                      expected, owners, staged / (Path(n).stem + ".shared-scalar.o")) for n in batch]
            for row in pool.map(trial, tasks):
                results.append(row)
                if len(results) % 128 == 0 or len(results) == len(selected):
                    print(f"[{len(results)}/{len(selected)}] shared scalar emission", flush=True)
    wins = sum(r["source_executable_bytes"]-r["trial"]["executable_bytes"] for r in results)
    if wins <= 0:
        raise ValueError("cross-module sharing did not reduce aggregate executable size")
    changed = {r["module"]: r for r in results}
    entries = []
    for n, record in records.items():
        if n in changed:
            row = changed[n]
            symbols = list(record["defined_symbols"]) + row["shared_definitions"]
            if len(symbols) != len(set(symbols)):
                raise ValueError("duplicate shared definition")
            entries.append(dict(record, object=Path(n).stem + ".shared-scalar.o",
                                size_bytes=row["trial"]["object_bytes"], sha256=row["trial"]["sha256"],
                                defined_symbols=symbols, transformed_ir_sha256=row["transformed_ir_sha256"],
                                experimental_shared_scalar=True))
        else:
            entries.append(dict(record, object=str((expanded / "objects" / record["object"]).resolve())))
    if Counter(s for r in entries for s in r["defined_symbols"] if s.startswith(SHARED)) != Counter(
            shared_name(n) for n in HELPERS):
        raise ValueError("shared scalar owner inventory differs")
    (out / "ee_functions.json").write_bytes(meta)
    overlay = dict(manifest, objects=entries, object_bytes=sum(r["size_bytes"] for r in entries),
                   experimental_mixed_pipeline=manifest["experimental_mixed_pipeline"] + "-shared",
                   production_cache_compatible=False)
    (staged / "objects.json").write_text(json.dumps(overlay, indent=2)+"\n", encoding="utf-8")
    plan = {"mode": "experimental_shared_scalar_memory", "metadata_sha256": sha(meta),
            "source_manifest_sha256": sha(parent_raw), "prior_library_sha256": parent_plan["linked_library_sha256"],
            "native_target": manifest["native_target"], "source_functions": len(names),
            "retained_functions": len(names), "recompiled_modules": len(results),
            "reused_modules": len(names)-len(results), "shared_owners": owners,
            "scalar_body_sha256": expected, "executable_saving": wins,
            "object_saving": sum(r["source_object_bytes"]-r["trial"]["object_bytes"] for r in results),
            "removed_ir_bytes": sum(r["removed_ir_bytes"] for r in results),
            "wall_seconds": time.monotonic()-started, "runtime_validation_required": True,
            "modules": results}
    library = out / "game-translated.dll"
    subprocess.run([sys.executable, str(Path(__file__).with_name("link_native_objects.py")),
                    str(out / "ee_functions.json"), str(staged), str(library)], check=True)
    data = library.read_bytes()
    plan.update(linked_library_bytes=len(data), linked_library_sha256=sha(data),
                linked_executable_raw_bytes=native_executable_sections(data)[0])
    (out / "helper-plan.json").write_text(json.dumps(plan, indent=2)+"\n", encoding="utf-8")
    return plan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    for flag in ("counter-experiment", "quadword-experiment", "multiply-experiment",
                 "add-experiment", "pilot-experiment", "experiment", "output"):
        parser.add_argument("--"+flag, type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    try:
        if os.name != "nt" or not 1 <= args.jobs <= 16 or os.environ.get("SDBZ_NATIVE_ALU_CERTIFICATES") == "1":
            raise ValueError("Windows experiment requires 1–16 jobs and disabled certificates")
        result = stage(args)
        print(f"Shared scalar memory: {result['executable_saving']:,} executable bytes saved", flush=True)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"EE shared scalar experiment refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
