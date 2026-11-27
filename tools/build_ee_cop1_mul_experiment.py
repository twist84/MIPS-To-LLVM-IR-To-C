#!/usr/bin/env python3
"""Exact-body COP1 multiply sharing over the isolated quadword image."""
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from audit_ee_helper_inventory import ir_inventory
from benchmark_ee_native_size import emit_native, sha
from build_ee_helper_experiment import FAMILY, COUNTER, SHARED, SHARED_COUNTER, refs, rewrite, rewrite_counter
from build_ee_quadword_experiment import QUAD, GUARD, SHARED_QUAD, body_of, rewrite_quadword, verify_prior
from ee_ownership_binary import native_executable_sections
from link_native_objects import load_inputs

MUL = "ee_cop1_mul_bits"
CORRECT = "ee_cop1_mul_correct"
BOOTH = "ee_cop1_booth_row"
CSA = "ee_cop1_mul_csa"
ROOTS = (MUL, CORRECT)
LEAVES = (BOOTH, CSA)
DEPENDENCIES = {MUL: [CORRECT], CORRECT: sorted(LEAVES), BOOTH: [], CSA: []}
ABI = {
    MUL: (b"{ i32, i1, i1 }", b"i32 %lhs, i32 %rhs", b"i32, i32", b""),
    CORRECT: (b"i64", b"i32 %ma, i32 %mb, i64 %full", b"i32, i32, i64", b" noinline"),
}


def shared(name):
    return name.replace("ee_", "ee_shared_", 1)


def rewrite_mul(raw, prior, guest, expected, owner):
    inv = ir_inventory(raw, guest)
    bodies = {}
    for name, deps in DEPENDENCIES.items():
        if (name not in inv or inv[name]["body_sha256"] != expected[name] or
                inv[name]["direct_dependencies"] != deps or refs(prior, shared(name))):
            raise ValueError(f"multiply helper body/dependencies differ: {name}")
        body = body_of(raw, name)
        if body_of(prior, name) != body:
            raise ValueError(f"multiply helper altered by prior transform: {name}")
        bodies[name] = body
        # No address-taken or non-call references outside the definition.
        if refs(prior, name) != 1 + inv[name]["guest_direct_calls"] + inv[name]["helper_direct_calls"]:
            raise ValueError(f"unexpected multiply symbol use: {name}")
    for name in LEAVES:
        if refs(prior, name) != 1 + refs(bodies[CORRECT], name):
            raise ValueError(f"multiply leaf escapes correction family: {name}")
    result = prior
    for name in ROOTS:
        ret, params, types, attrs = ABI[name]
        old = b"define internal " + ret + b" @" + name.encode() + b"(" + params + b")" + attrs + b" {"
        if bodies[name].splitlines()[0] != old:
            raise ValueError(f"unexpected multiply ABI: {name}")
        if owner:
            new = b"define " + ret + b" @" + name.encode() + b"(" + params + b") noinline {"
            replacement = bodies[name].replace(old, new, 1)
        else:
            replacement = b"declare " + ret + b" @" + name.encode() + b"(" + types + b") noinline"
        result = result.replace(bodies[name], replacement, 1)
    if not owner:
        for name in LEAVES:
            result = result.replace(bodies[name], b"", 1)
    for name in ROOTS:
        result = re.sub(rb"@" + name.encode() + rb"(?![\w.$])",
                        b"@" + shared(name).encode(), result)
    if any(refs(result, name) for name in (*ROOTS, *(() if owner else LEAVES))):
        raise ValueError("incomplete multiply rewrite")
    return result


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def rows_of(plan):
    rows = {row["module"]: row for row in plan["modules"]}
    if len(rows) != len(plan["modules"]):
        raise ValueError("duplicate plan module")
    return rows


def check_object(raw, data, record, row):
    mode = record.get("experimental_pipeline", "O5")
    if (mode not in ("O5", "Oz") or mode != row["mode"] or
            sha(raw) != record["ir_sha256"] or row["source_ir_sha256"] != record["ir_sha256"] or
            sha(data) != record["sha256"] or row["trial"]["sha256"] != record["sha256"] or
            len(data) != record["size_bytes"] or row["trial"]["object_bytes"] != len(data) or
            row["trial"]["executable_bytes"] != native_executable_sections(data)[0] or
            row["transformed_ir_sha256"] != record.get("transformed_ir_sha256")):
        raise ValueError("prior object or transformed IR identity differs")
    return mode


def trial(path, obj, record, row, guest, family, counter, quad, expected,
          observation_owner, has_quad, quad_owner, owner, triple, out):
    raw, data = path.read_bytes(), obj.read_bytes()
    mode = check_object(raw, data, record, row)
    before = rewrite_counter(raw, rewrite(raw, guest, family, observation_owner),
                             guest, counter, observation_owner)
    if has_quad:
        before = rewrite_quadword(raw, before, guest, quad, quad_owner)
    if sha(before) != row["transformed_ir_sha256"]:
        raise ValueError(f"prior transform differs: {path.name}")
    changed = rewrite_mul(raw, before, guest, expected, owner)
    result = emit_native(changed, triple, guest, mode, object_output=out)
    return {"module": path.name, "mode": mode, "multiply_owner": owner,
            "source_ir_sha256": sha(raw), "transformed_ir_sha256": sha(changed),
            "source_object_sha256": sha(data), "source_object_bytes": len(data),
            "source_executable_bytes": native_executable_sections(data)[0],
            "removed_ir_bytes": len(before) - len(changed), "trial": result}


def stage(ir, experiment, counter_experiment, inventory, output, jobs):
    ir, experiment, counter_experiment, output = (
        p.resolve() for p in (ir, experiment, counter_experiment, output))
    if output.exists() or any(output.is_relative_to(p) for p in (ir, experiment, counter_experiment)):
        raise ValueError("use a new output outside input caches")
    started = time.monotonic()
    meta_path = ir / "ee_functions.json"
    meta_bytes = meta_path.read_bytes()
    objects = experiment / "objects"
    manifest_bytes = (objects / "objects.json").read_bytes()
    metadata, manifest, _, _ = load_inputs(meta_path, objects)
    plan = read_json(experiment / "helper-plan.json")
    counter_plan = read_json(counter_experiment / "helper-plan.json")
    counter_manifest_bytes = (counter_experiment / "objects" / "objects.json").read_bytes()
    evidence = read_json(inventory)
    count = len(metadata["functions"])
    if (plan.get("mode") != "experimental_shared_quadword" or
            counter_plan.get("mode") != "experimental_shared_counter" or
            plan.get("source_manifest_sha256") != sha(counter_manifest_bytes) or
            plan.get("prior_library_sha256") != counter_plan.get("linked_library_sha256") or
            any(p.get("metadata_sha256") != sha(meta_bytes) or
                p.get("native_target") != manifest["native_target"] or
                p.get("source_functions") != count or p.get("retained_functions") != count
                for p in (plan, counter_plan)) or
            (experiment / "ee_functions.json").read_bytes() != meta_bytes or
            (counter_experiment / "ee_functions.json").read_bytes() != meta_bytes or
            manifest.get("experimental_mixed_pipeline") != "O5-selective-Oz-shared-observation-counter-quadword" or
            manifest.get("production_cache_compatible") is not False or
            not manifest["target_triple"].endswith("windows-msvc") or
            evidence.get("mode") != "read_only_ee_helper_inventory" or
            evidence.get("metadata_sha256") != sha(meta_bytes) or evidence.get("modules") != count):
        raise ValueError("expected verified cumulative quadword input")
    for folder, receipt in ((experiment, plan), (counter_experiment, counter_plan)):
        dll = folder / "game-translated.dll"
        if sha(dll.read_bytes()) != receipt["linked_library_sha256"] or dll.stat().st_size != receipt["linked_library_bytes"]:
            raise ValueError("prior library identity differs")
    names = {Path(f["ir_path"]).name: f["host_symbol"] for f in metadata["functions"]}
    records = {Path(r["input"]).name: r for r in manifest["objects"]}
    base_records = {Path(r["input"]).name: r for r in json.loads(counter_manifest_bytes)["objects"]}
    paths = {p.name: p for p in ir.glob("*.ll")}
    base_rows, quad_rows = rows_of(counter_plan), rows_of(plan)
    if (len(names) != count or set(names) != set(records) or set(names) != set(base_records) or
            set(names) != set(paths) or set(names) != set(base_rows) or
            not set(quad_rows) <= set(names) or len(quad_rows) != plan["recompiled_modules"] or
            count-len(quad_rows) != plan["reused_modules"]):
        raise ValueError("prior module inventory differs")
    groups = {}
    for g in evidence["groups"]:
        if g["name"] in (*FAMILY, COUNTER, QUAD, GUARD, *DEPENDENCIES):
            if g["name"] in groups:
                raise ValueError("ambiguous audited body")
            groups[g["name"]] = g
    family = {n: groups[n]["body_sha256"] for n in FAMILY}
    counter = groups[COUNTER]["body_sha256"]
    quad = {n: groups[n]["body_sha256"] for n in (QUAD, GUARD)}
    expected = {n: groups[n]["body_sha256"] for n in DEPENDENCIES}
    obs_owner, quad_owner = counter_plan["shared_owner"], plan["quadword_owner"]
    if (family != counter_plan["family_body_sha256"] or counter != counter_plan["counter_body_sha256"] or
            quad != plan["quadword_body_sha256"] or obs_owner != plan["shared_owner"] or
            obs_owner != min(names) or quad_owner != min(quad_rows) or
            len(quad_rows) != groups[QUAD]["definitions"]):
        raise ValueError("previous helper evidence differs")
    selected = []
    for name in sorted(names):
        raw = paths[name].read_bytes()
        record, base, base_row = records[name], base_records[name], base_rows[name]
        data = (objects / record["object"]).read_bytes()
        has_quad = name in quad_rows
        row = quad_rows[name] if has_quad else base_row
        check_object(raw, data, record, row)
        if (base_row["trial"]["sha256"] != base["sha256"] or
                base_row["source_ir_sha256"] != base["ir_sha256"] or
                base_row["transformed_ir_sha256"] != base["transformed_ir_sha256"] or
                base_row["shared_owner"] != (name == obs_owner) or
                bool(refs(raw, QUAD)) != has_quad or
                (has_quad and (row["source_object_sha256"] != base["sha256"] or
                               row["quadword_owner"] != (name == quad_owner))) or
                (not has_quad and record["sha256"] != base["sha256"])):
            raise ValueError(f"prior chain differs: {name}")
        symbols = record["defined_symbols"]
        if ((SHARED in symbols) != (name == obs_owner) or
                (SHARED_COUNTER in symbols) != (name == obs_owner) or
                (SHARED_QUAD in symbols) != (name == quad_owner) or
                any(shared(n) in symbols or refs(raw, shared(n)) for n in DEPENDENCIES)):
            raise ValueError(f"unexpected prior symbol inventory: {name}")
        if any(refs(raw, n) for n in DEPENDENCIES):
            selected.append(name)
    if not selected or any(len(selected) != groups[n]["definitions"] for n in DEPENDENCIES):
        raise ValueError("multiply selection differs from audited inventory")
    owner = selected[0]
    output.mkdir(parents=True)
    staged = output / "objects"
    staged.mkdir()
    results = {}
    print(f"Re-emitting {len(selected):,} modules; reusing {count-len(selected):,}", flush=True)
    for first in range(0, len(selected), 128 * jobs):
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            futures = [pool.submit(trial, paths[n], objects / records[n]["object"], records[n],
                                   quad_rows.get(n, base_rows[n]), names[n], family, counter, quad, expected,
                                   n == obs_owner, n in quad_rows, n == quad_owner, n == owner,
                                   manifest["target_triple"], staged / (Path(n).stem + ".mul.o"))
                       for n in selected[first:first+128*jobs]]
            for future in as_completed(futures):
                row = future.result()
                results[row["module"]] = row
                if len(results) % 128 == 0 or len(results) == len(selected):
                    print(f"[{len(results)}/{len(selected)}] shared multiply emission", flush=True)
    entries = []
    for name, record in records.items():
        if name not in results:
            entries.append(dict(record, object=str((objects / record["object"]).resolve())))
            continue
        row = results[name]
        symbols = list(record["defined_symbols"])
        if name == owner:
            symbols.extend(shared(n) for n in ROOTS)
        entries.append(dict(record, object=Path(name).stem + ".mul.o",
                            size_bytes=row["trial"]["object_bytes"], sha256=row["trial"]["sha256"],
                            defined_symbols=symbols, transformed_ir_sha256=row["transformed_ir_sha256"],
                            experimental_shared_cop1_mul=True))
    (output / "ee_functions.json").write_bytes(meta_bytes)
    overlay = dict(manifest, objects=entries, object_bytes=sum(x["size_bytes"] for x in entries),
                   experimental_mixed_pipeline=manifest["experimental_mixed_pipeline"] + "-cop1-mul",
                   production_cache_compatible=False)
    (staged / "objects.json").write_text(json.dumps(overlay, indent=2) + "\n", encoding="utf-8")
    return {
        "mode": "experimental_shared_cop1_mul", "metadata_sha256": sha(meta_bytes),
        "source_manifest_sha256": sha(manifest_bytes), "native_target": manifest["native_target"],
        "prior_library_sha256": plan["linked_library_sha256"],
        "source_functions": count, "retained_functions": count,
        "multiply_owner": owner, "shared_owner": obs_owner, "quadword_owner": quad_owner,
        "multiply_body_sha256": expected,
        "recompiled_modules": len(results), "reused_modules": count-len(results),
        "removed_ir_bytes": sum(x["removed_ir_bytes"] for x in results.values()),
        "executable_saving": sum(x["source_executable_bytes"] - x["trial"]["executable_bytes"] for x in results.values()),
        "object_saving": sum(x["source_object_bytes"] - x["trial"]["object_bytes"] for x in results.values()),
        "wall_seconds": time.monotonic() - started, "runtime_validation_required": True,
        "modules": [results[n] for n in selected],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    parser.add_argument("--experiment", type=Path, required=True)
    parser.add_argument("--counter-experiment", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--link", action="store_true")
    args = parser.parse_args()
    try:
        if not 1 <= args.jobs <= 16 or os.name != "nt" or os.environ.get("SDBZ_NATIVE_ALU_CERTIFICATES") == "1":
            raise ValueError("Windows experiment requires 1–16 jobs and disabled certificates")
        receipt = stage(args.ir, args.experiment, args.counter_experiment, args.inventory, args.output, args.jobs)
        path = args.output / "helper-plan.json"
        path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        if args.link:
            library = args.output / "game-translated.dll"
            subprocess.run([sys.executable, str(Path(__file__).with_name("link_native_objects.py")),
                            str(args.output / "ee_functions.json"), str(args.output / "objects"), str(library)],
                           check=True)
            data = library.read_bytes()
            receipt.update(linked_library_bytes=len(data), linked_library_sha256=sha(data),
                           linked_executable_raw_bytes=native_executable_sections(data)[0])
            path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(f"Shared COP1 multiply: {receipt['executable_saving']:,} executable bytes saved", flush=True)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"EE multiply experiment refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
