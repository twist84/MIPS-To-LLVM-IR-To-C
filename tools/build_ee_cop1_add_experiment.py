#!/usr/bin/env python3
"""Share the exact COP1 add/sub helper over the validated multiplier overlay."""
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
from build_ee_quadword_experiment import QUAD, GUARD, SHARED_QUAD, body_of, rewrite_quadword
from build_ee_cop1_mul_experiment import DEPENDENCIES, ROOTS, rewrite_mul, shared, check_object, rows_of, read_json
from ee_ownership_binary import native_executable_sections
from link_native_objects import load_inputs

ADD = "ee_cop1_add_bits"
SHARED_ADD = "ee_shared_cop1_add_bits"
HEADER = b"define internal { i32, i1, i1 } @ee_cop1_add_bits(i32 %lhs, i32 %rhs, i1 %subtract) {"
DECL = b"declare { i32, i1, i1 } @ee_shared_cop1_add_bits(i32, i32, i1) noinline"
CTLZ = rb"^declare i32 @llvm\.ctlz\.i32\(i32, i1\)[ \t]*\r?$"
MODES = ("experimental_shared_counter", "experimental_shared_quadword", "experimental_shared_cop1_mul")
PIPELINES = ("O5-selective-Oz-shared-observation-counter",
             "O5-selective-Oz-shared-observation-counter-quadword",
             "O5-selective-Oz-shared-observation-counter-quadword-cop1-mul")


def rewrite_add(raw, prior, guest, expected, owner):
    inv = ir_inventory(raw, guest)
    info = inv.get(ADD)
    if (info is None or info["body_sha256"] != expected or
            info["direct_dependencies"] != ["llvm.ctlz.i32"] or
            refs(prior, SHARED_ADD)):
        raise ValueError("addition body/dependency differs")
    body = body_of(raw, ADD)
    if (body.splitlines()[0] != HEADER or body_of(prior, ADD) != body or
            refs(prior, ADD) != 1 + info["guest_direct_calls"] + info["helper_direct_calls"] or
            len(re.findall(CTLZ, prior, re.M)) != 1):
        raise ValueError("addition ABI, intrinsic or symbol use differs")
    replacement = (body.replace(HEADER, HEADER.replace(b"define internal ", b"define ", 1)
                    .replace(b"@ee_cop1_add_bits", b"@ee_shared_cop1_add_bits", 1)
                    .replace(b") {", b") noinline {", 1), 1) if owner else DECL)
    result = prior.replace(body, replacement, 1)
    result = re.sub(rb"@ee_cop1_add_bits(?![\w.$])", b"@ee_shared_cop1_add_bits", result)
    if refs(result, ADD):
        raise ValueError("incomplete addition rewrite")
    return result


def trial(path, obj, record, row, guest, family, counter, quad, mul, add,
          obs_owner, has_quad, quad_owner, has_mul, mul_owner, owner, triple, out):
    raw, data = path.read_bytes(), obj.read_bytes()
    mode = check_object(raw, data, record, row)
    before = rewrite_counter(raw, rewrite(raw, guest, family, obs_owner),
                             guest, counter, obs_owner)
    if has_quad:
        before = rewrite_quadword(raw, before, guest, quad, quad_owner)
    if has_mul:
        before = rewrite_mul(raw, before, guest, mul, mul_owner)
    if sha(before) != row["transformed_ir_sha256"]:
        raise ValueError(f"prior transformation differs: {path.name}")
    changed = rewrite_add(raw, before, guest, add, owner)
    result = emit_native(changed, triple, guest, mode, object_output=out)
    return {"module": path.name, "mode": mode, "addition_owner": owner,
            "source_ir_sha256": sha(raw), "transformed_ir_sha256": sha(changed),
            "source_object_sha256": sha(data), "source_object_bytes": len(data),
            "source_executable_bytes": native_executable_sections(data)[0],
            "removed_ir_bytes": len(before) - len(changed), "trial": result}


def stage(ir, experiment, quadword_experiment, counter_experiment, inventory, output, jobs):
    ir, experiment, quadword_experiment, counter_experiment, output = (
        p.resolve() for p in (ir, experiment, quadword_experiment, counter_experiment, output))
    folders = (counter_experiment, quadword_experiment, experiment)
    if output.exists() or any(output.is_relative_to(p) for p in (ir, *folders)):
        raise ValueError("use a new output outside input caches")
    started = time.monotonic()
    meta_path = ir / "ee_functions.json"
    meta_bytes = meta_path.read_bytes()
    objects = experiment / "objects"
    metadata, manifest, _, _ = load_inputs(meta_path, objects)
    count = len(metadata["functions"])
    manifests_raw = [(p / "objects" / "objects.json").read_bytes() for p in folders]
    manifests = [json.loads(raw) for raw in manifests_raw]
    plans = [read_json(p / "helper-plan.json") for p in folders]
    evidence = read_json(inventory)
    if (evidence.get("mode") != "read_only_ee_helper_inventory" or
            evidence.get("metadata_sha256") != sha(meta_bytes) or evidence.get("modules") != count or
            not manifest["target_triple"].endswith("windows-msvc")):
        raise ValueError("expected audited Windows helper inventory")
    for i, (folder, plan, m) in enumerate(zip(folders, plans, manifests)):
        dll = folder / "game-translated.dll"
        if (plan.get("mode") != MODES[i] or m.get("experimental_mixed_pipeline") != PIPELINES[i] or
                m.get("production_cache_compatible") is not False or
                plan.get("metadata_sha256") != sha(meta_bytes) or
                (folder / "ee_functions.json").read_bytes() != meta_bytes or
                plan.get("source_functions") != count or plan.get("retained_functions") != count or
                plan.get("native_target") != manifest["native_target"] or
                m.get("native_target") != manifest["native_target"] or
                m.get("target_triple") != manifest["target_triple"] or
                sha(dll.read_bytes()) != plan.get("linked_library_sha256") or
                dll.stat().st_size != plan.get("linked_library_bytes")):
            raise ValueError(f"prior overlay identity differs: {folder}")
        if i and (plan.get("source_manifest_sha256") != sha(manifests_raw[i-1]) or
                  plan.get("prior_library_sha256") != plans[i-1].get("linked_library_sha256")):
            raise ValueError(f"prior overlay chain differs: {folder}")
    names = {Path(f["ir_path"]).name: f["host_symbol"] for f in metadata["functions"]}
    paths = {p.name: p for p in ir.glob("*.ll")}
    records_list = [{Path(r["input"]).name: r for r in m["objects"]} for m in manifests]
    records = records_list[-1]
    base_rows, quad_rows, mul_rows = [rows_of(p) for p in plans]
    if (len(names) != count or set(names) != set(paths) or set(names) != set(base_rows) or
            any(len(m["objects"]) != count or set(r) != set(names)
                for m, r in zip(manifests, records_list)) or
            any(not set(rows) <= set(names) or len(rows) != p["recompiled_modules"] or
                count-len(rows) != p["reused_modules"]
                for rows, p in zip((quad_rows, mul_rows), plans[1:]))):
        raise ValueError("prior module inventory differs")
    groups = {}
    for g in evidence["groups"]:
        if g["name"] in (*FAMILY, COUNTER, QUAD, GUARD, *DEPENDENCIES, ADD):
            if g["name"] in groups:
                raise ValueError("ambiguous audited body")
            groups[g["name"]] = g
    family = {n: groups[n]["body_sha256"] for n in FAMILY}
    counter = groups[COUNTER]["body_sha256"]
    quad = {n: groups[n]["body_sha256"] for n in (QUAD, GUARD)}
    mul = {n: groups[n]["body_sha256"] for n in DEPENDENCIES}
    add = groups[ADD]["body_sha256"]
    obs_owner, quad_owner, mul_owner = plans[0]["shared_owner"], plans[1]["quadword_owner"], plans[2]["multiply_owner"]
    if (family != plans[0]["family_body_sha256"] or counter != plans[0]["counter_body_sha256"] or
            quad != plans[1]["quadword_body_sha256"] or mul != plans[2]["multiply_body_sha256"] or
            any(p["shared_owner"] != obs_owner for p in plans) or
            plans[2]["quadword_owner"] != quad_owner or obs_owner != min(names) or
            quad_owner != min(quad_rows) or mul_owner != min(mul_rows) or
            len(quad_rows) != groups[QUAD]["definitions"] or
            any(len(mul_rows) != groups[n]["definitions"] for n in DEPENDENCIES) or
            groups[ADD]["direct_dependencies"] != ["llvm.ctlz.i32"]):
        raise ValueError("prior helper evidence differs")
    selected, current_rows = [], {}
    for name in sorted(names):
        raw = paths[name].read_bytes()
        row = base_rows[name]
        previous = records_list[0][name]
        if (row["shared_owner"] != (name == obs_owner) or
                row["trial"]["sha256"] != previous["sha256"] or
                row["transformed_ir_sha256"] != previous.get("transformed_ir_sha256") or
                row["source_ir_sha256"] != sha(raw) or
                bool(refs(raw, QUAD)) != (name in quad_rows) or
                any(bool(refs(raw, n)) != (name in mul_rows) for n in DEPENDENCIES)):
            raise ValueError(f"prior source evidence differs: {name}")
        for idx, rows, owner_key, owner in ((1, quad_rows, "quadword_owner", quad_owner),
                                          (2, mul_rows, "multiply_owner", mul_owner)):
            current = records_list[idx][name]
            if name in rows:
                row = rows[name]
                if (row["source_object_sha256"] != previous["sha256"] or
                        row[owner_key] != (name == owner)):
                    raise ValueError(f"prior object chain differs: {name}")
            if (row["trial"]["sha256"] != current["sha256"] or
                    row["transformed_ir_sha256"] != current.get("transformed_ir_sha256") or
                    row["source_ir_sha256"] != current["ir_sha256"]):
                raise ValueError(f"prior receipt differs: {name}")
            previous = current
        record = records[name]
        data = (objects / record["object"]).read_bytes()
        check_object(raw, data, record, row)
        symbols = record["defined_symbols"]
        if ((SHARED in symbols) != (name == obs_owner) or
                (SHARED_COUNTER in symbols) != (name == obs_owner) or
                (SHARED_QUAD in symbols) != (name == quad_owner) or
                any((shared(n) in symbols) != (name == mul_owner) for n in ROOTS) or
                SHARED_ADD in symbols or refs(raw, SHARED_ADD)):
            raise ValueError(f"prior symbol inventory differs: {name}")
        if refs(raw, ADD):
            selected.append(name)
            current_rows[name] = row
    if not selected or len(selected) != groups[ADD]["definitions"]:
        raise ValueError("addition selection differs from audited inventory")
    owner = selected[0]
    output.mkdir(parents=True)
    staged = output / "objects"
    staged.mkdir()
    results = {}
    print(f"Re-emitting {len(selected):,} modules; reusing {count-len(selected):,}", flush=True)
    for first in range(0, len(selected), 128 * jobs):
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            futures = [pool.submit(trial, paths[n], objects / records[n]["object"], records[n],
                                   current_rows[n], names[n], family, counter, quad, mul, add,
                                   n == obs_owner, n in quad_rows, n == quad_owner,
                                   n in mul_rows, n == mul_owner, n == owner,
                                   manifest["target_triple"], staged / (Path(n).stem + ".add.o"))
                       for n in selected[first:first+128*jobs]]
            for future in as_completed(futures):
                row = future.result()
                results[row["module"]] = row
                if len(results) % 128 == 0 or len(results) == len(selected):
                    print(f"[{len(results)}/{len(selected)}] shared addition emission", flush=True)
    entries = []
    for name, record in records.items():
        if name not in results:
            entries.append(dict(record, object=str((objects / record["object"]).resolve())))
            continue
        row = results[name]
        symbols = list(record["defined_symbols"])
        if name == owner:
            symbols.append(SHARED_ADD)
        entries.append(dict(record, object=Path(name).stem + ".add.o",
                            size_bytes=row["trial"]["object_bytes"], sha256=row["trial"]["sha256"],
                            defined_symbols=symbols, transformed_ir_sha256=row["transformed_ir_sha256"],
                            experimental_shared_cop1_add=True))
    (output / "ee_functions.json").write_bytes(meta_bytes)
    overlay = dict(manifest, objects=entries, object_bytes=sum(x["size_bytes"] for x in entries),
                   experimental_mixed_pipeline=PIPELINES[-1] + "-cop1-add",
                   production_cache_compatible=False)
    (staged / "objects.json").write_text(json.dumps(overlay, indent=2) + "\n", encoding="utf-8")
    return {
        "mode": "experimental_shared_cop1_add", "metadata_sha256": sha(meta_bytes),
        "source_manifest_sha256": sha(manifests_raw[-1]), "native_target": manifest["native_target"],
        "prior_library_sha256": plans[-1]["linked_library_sha256"],
        "source_functions": count, "retained_functions": count,
        "addition_owner": owner, "shared_owner": obs_owner, "quadword_owner": quad_owner,
        "multiply_owner": mul_owner, "addition_body_sha256": add,
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
    parser.add_argument("--quadword-experiment", type=Path, required=True)
    parser.add_argument("--counter-experiment", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--link", action="store_true")
    args = parser.parse_args()
    try:
        if not 1 <= args.jobs <= 16 or os.name != "nt" or os.environ.get("SDBZ_NATIVE_ALU_CERTIFICATES") == "1":
            raise ValueError("Windows experiment requires 1–16 jobs and disabled certificates")
        receipt = stage(args.ir, args.experiment, args.quadword_experiment,
                        args.counter_experiment, args.inventory, args.output, args.jobs)
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
        print(f"Shared COP1 addition: {receipt['executable_saving']:,} executable bytes saved", flush=True)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"EE addition experiment refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
