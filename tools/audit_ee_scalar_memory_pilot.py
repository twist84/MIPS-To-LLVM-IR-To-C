#!/usr/bin/env python3
"""Paired native-size pilot for exact scalar fastmem outlining.

Only canonical generated regions are rewritten. Alignment, guest tracing,
state publication and all callers remain unchanged. Helpers are local to
each module; no DLL or production cache changes. Runtime cost is unknown.
"""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
import json
import os
import subprocess
import time
from pathlib import Path
import re
import sys

from audit_ee_helper_inventory import read_manifest, verified_object, sha
from benchmark_ee_native_size import emit_native
from build_ee_helper_experiment import rewrite, rewrite_counter
from build_ee_quadword_experiment import body_of, rewrite_quadword
from build_ee_cop1_mul_experiment import check_object, rows_of, rewrite_mul
from build_ee_cop1_add_experiment import rewrite_add
from ee_ownership_binary import native_executable_sections
from link_native_objects import load_inputs

TEMP = rb"%t\d+"
VALUE = rb"(?:%t\d+|-?\d+)"
TOKEN = re.compile(rb"%[\w.$]+")
SYMBOL = b"ee_pilot_scalar_"


def pattern(width, write):
    bits = str(width * 8).encode()
    ty = b"i" + bits
    prefix = b"fastmem_word_" if width == 4 else b"fastmem_scalar_"
    # Backreferences certify every SSA operand and CFG edge, not just labels.
    def var(name, token=TEMP):
        return b"(?P<" + name + b">" + token + b")"
    def ref(name):
        return b"(?P=" + name + b")"
    def label(name, first=False):
        return prefix + name + b"_" + (var(b"suffix", rb"\d+") if first else ref(b"suffix"))
    check, direct, slow, done = (label(n) for n in (b"check", b"direct", b"slow", b"done"))
    lines = [
        b"  " + var(b"null") + rb" = icmp eq ptr %rt, null",
        b"  br i1 " + ref(b"null") + b", label %" + label(b"slow", True) + b", label %" + check,
        check + b":",
        b"  " + var(b"sp") + rb" = getelementptr i8, ptr %rt, i64 " + var(b"sizeoff", rb"\d+"),
        b"  " + var(b"size") + b" = load i32, ptr " + ref(b"sp"),
        b"  " + var(b"bp") + rb" = getelementptr i8, ptr %rt, i64 " + var(b"baseoff", rb"\d+"),
        b"  " + var(b"base") + b" = load ptr, ptr " + ref(b"bp"),
        b"  " + var(b"offset") + b" = zext i32 " + var(b"address", VALUE) + b" to i64",
        b"  " + var(b"end") + b" = add i64 " + ref(b"offset") + b", " + str(width).encode(),
        b"  " + var(b"limit") + b" = zext i32 " + ref(b"size") + b" to i64",
        b"  " + var(b"fits") + b" = icmp ule i64 " + ref(b"end") + b", " + ref(b"limit"),
        b"  " + var(b"present") + b" = icmp ne ptr " + ref(b"base") + b", null",
        b"  " + var(b"safe") + b" = and i1 " + ref(b"fits") + b", " + ref(b"present"),
        b"  br i1 " + ref(b"safe") + b", label %" + direct + b", label %" + slow,
        direct + b":",
        b"  " + var(b"ptr") + b" = getelementptr i8, ptr " + ref(b"base") + b", i64 " + ref(b"offset"),
    ]
    if write:
        lines += [b"  store " + ty + b" " + var(b"value", VALUE) + b", ptr " + ref(b"ptr") + b", align 1"]
    else:
        lines += [b"  " + var(b"read") + b" = load " + ty + b", ptr " + ref(b"ptr") + b", align 1"]
    lines += [b"  br label %" + done, slow + b":"]
    if write:
        lines += [b"  call void @ee_mem_write" + bits + rb"\(ptr %rt, i32 " + ref(b"address") +
                  b", " + ty + b" " + ref(b"value") + rb"\)"]
    else:
        lines += [b"  " + var(b"fallback") + b" = call " + ty + b" @ee_mem_read" + bits +
                  rb"\(ptr %rt, i32 " + ref(b"address") + rb"\)"]
    lines += [b"  br label %" + done, done + b":"]
    if not write:
        lines += [b"  " + var(b"result") + b" = phi " + ty + rb" \[ " + ref(b"read") +
                  b", %" + direct + rb" \], \[ " + ref(b"fallback") + b", %" + slow + rb" \]"]
    return re.compile(rb"^" + b"\n".join(lines) + rb"\n", re.M)


PATTERNS = [(w, write, pattern(w, write)) for w in (1, 2, 4, 8) for write in (False, True)]


def helper(width, write, sizeoff, baseoff):
    ty = f"i{width*8}".encode()
    name = SYMBOL + (b"write" if write else b"read") + str(width*8).encode()
    ret = b"void" if write else ty
    params = b"ptr %rt, i32 %address" + (b", " + ty + b" %value" if write else b"")
    body = b"define internal " + ret + b" @" + name + b"(" + params + b") noinline {\n"
    body += b"""entry:
  %null = icmp eq ptr %rt, null
  br i1 %null, label %slow, label %check
check:
  %sp = getelementptr i8, ptr %rt, i64 """ + sizeoff + b"""
  %size = load i32, ptr %sp
  %bp = getelementptr i8, ptr %rt, i64 """ + baseoff + b"""
  %base = load ptr, ptr %bp
  %offset = zext i32 %address to i64
  %end = add i64 %offset, """ + str(width).encode() + b"""
  %limit = zext i32 %size to i64
  %fits = icmp ule i64 %end, %limit
  %present = icmp ne ptr %base, null
  %safe = and i1 %fits, %present
  br i1 %safe, label %direct, label %slow
direct:
  %ptr = getelementptr i8, ptr %base, i64 %offset
"""
    bits = str(width*8).encode()
    if write:
        body += b"  store " + ty + b" %value, ptr %ptr, align 1\n  ret void\nslow:\n"
        body += b"  call void @ee_mem_write" + bits + b"(ptr %rt, i32 %address, " + ty + b" %value)\n  ret void\n}\n"
    else:
        body += b"  %read = load " + ty + b", ptr %ptr, align 1\n  ret " + ty + b" %read\nslow:\n"
        body += b"  %fallback = call " + ty + b" @ee_mem_read" + bits + b"(ptr %rt, i32 %address)\n  ret " + ty + b" %fallback\n}\n"
    return name, body


def outline(raw, guest):
    if SYMBOL in raw:
        raise ValueError("pilot helper symbol already present")
    body = body_of(raw, guest)
    all_tokens = Counter(TOKEN.findall(body))
    matches, helpers, counts = [], {}, Counter()
    for width, write, regex in PATTERNS:
        for m in regex.finditer(body):
            g = m.groupdict()
            prefix = b"%fastmem_word_" if width == 4 else b"%fastmem_scalar_"
            local = [g[k] for k in ("null", "sp", "size", "bp", "base", "offset", "end",
                                    "limit", "fits", "present", "safe", "ptr")]
            if not write:
                local += [g["read"], g["fallback"]]
            labels = [prefix + n + b"_" + g["suffix"] for n in (b"check", b"direct", b"slow", b"done")]
            inside = Counter(TOKEN.findall(m[0]))
            # Do not delete SSA values or predecessor blocks used elsewhere.
            if any(all_tokens[x] != inside[x] for x in local + labels):
                counts["rejected_escaping_region"] += 1
                continue
            if re.match(rb"\s*%[\w.$]+ = phi\b", body[m.end():]):
                counts["rejected_extra_phi"] += 1
                continue
            name, definition = helper(width, write, g["sizeoff"], g["baseoff"])
            if name in helpers and helpers[name] != definition:
                raise ValueError("inconsistent runtime offsets for scalar width")
            helpers[name] = definition
            ty = f"i{width*8}".encode()
            args = b"ptr %rt, i32 " + g["address"]
            if write:
                args += b", " + ty + b" " + g["value"]
                call = b"  call void @" + name + b"(" + args + b")\n"
            else:
                call = b"  " + g["result"] + b" = call " + ty + b" @" + name + b"(" + args + b")\n"
            done = labels[-1]
            replacement = call + b"  br label " + done + b"\n" + done[1:] + b":\n"
            matches.append((m.start(), m.end(), replacement))
            counts[name.decode()] += 1
    if not matches:
        return raw, dict(counts)
    rewritten = bytearray()
    cursor = 0
    for start, end, replacement in sorted(matches):
        if start < cursor:
            raise ValueError("overlapping scalar matches")
        rewritten.extend(body[cursor:start])
        rewritten.extend(replacement)
        cursor = end
    rewritten.extend(body[cursor:])
    result = raw.replace(body, bytes(rewritten), 1) + b"\n" + b"\n".join(helpers.values())
    return result, dict(counts)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def trial(task):
    name, guest, path, obj_dir, record, receipts, plans, triple = task[:8]
    object_output = task[8] if len(task) == 9 else None
    raw = path.read_bytes()
    data = verified_object(obj_dir, record, sha(raw))
    counter, quad, mul, add = plans
    r0, r1, r2, r3 = receipts
    owner = name == counter["shared_owner"]
    changed = rewrite_counter(raw, rewrite(raw, guest, counter["family_body_sha256"], owner),
                              guest, counter["counter_body_sha256"], owner)
    if sha(changed) != r0["transformed_ir_sha256"]:
        raise ValueError(f"counter digest mismatch: {name}")
    row = r0
    for r, p, fn, key, owner_key in (
            (r1, quad, rewrite_quadword, "quadword_body_sha256", "quadword_owner"),
            (r2, mul, rewrite_mul, "multiply_body_sha256", "multiply_owner"),
            (r3, add, rewrite_add, "addition_body_sha256", "addition_owner")):
        if r is not None:
            if r["source_object_sha256"] != row["trial"]["sha256"]:
                raise ValueError(f"prior object chain mismatch: {name}")
            changed = fn(raw, changed, guest, p[key], name == p[owner_key])
            if sha(changed) != r["transformed_ir_sha256"]:
                raise ValueError(f"prior transform digest mismatch: {name}")
            row = r
    mode = check_object(raw, data, record, row)
    candidate, counts = outline(changed, guest)
    result = {"module": name, "mode": mode, "original_ir_sha256": sha(raw),
              "parent_ir_sha256": sha(changed), "parent_object_sha256": sha(data),
              "parent_executable_bytes": row["trial"]["executable_bytes"],
              "parent_object_bytes": len(data), "counts": counts,
              "ir_bytes_saved": len(changed)-len(candidate)}
    if candidate != changed:
        # Paired compile of exact parent confirms the same emitter/policy.
        baseline = emit_native(changed, triple, guest, mode)
        if baseline["sha256"] != record["sha256"]:
            raise ValueError(f"parent object not reproducible: {name}")
        result["candidate"] = emit_native(candidate, triple, guest, mode, object_output=object_output)
        result["candidate_ir_sha256"] = sha(candidate)
        result["executable_saving"] = result["parent_executable_bytes"] - result["candidate"]["executable_bytes"]
        result["object_saving"] = len(data) - result["candidate"]["object_bytes"]
    return result


def select(rows, per_band, largest):
    groups = defaultdict(list)
    for row in rows:
        if row.get("memory_helper_calls", 0):
            n = row["guest_pcs"]
            band = next((hi for hi in (4, 16, 32, 128, 512) if n <= hi), 2**32)
            groups[band].append(row)
    def rank(r):
        return r["memory_helper_calls"], r["current_executable_bytes"], r["module"]
    selected = set()
    for group in groups.values():
        ordered = sorted(group, key=rank)
        count = min(len(ordered), per_band)
        if count == 1:
            selected.add(ordered[len(ordered)//2]["module"])
        elif count > 1:
            selected.update(ordered[i*(len(ordered)-1)//(count-1)]["module"] for i in range(count))
    # Audit Counters omit zero-valued fields. Only rank actual memory
    # candidates, including for the global largest-module selection.
    candidates = [row for group in groups.values() for row in group]
    selected.update(r["module"] for r in sorted(candidates, key=rank, reverse=True)[:largest])
    return sorted(selected)


def stable_receipt(row):
    # Compilation time is deliberately not part of the identity.
    return {k: (dict((n, v) for n, v in value.items() if n != "seconds")
                if k == "candidate" else value) for k, value in row.items()}


def winners(pilot, selected):
    rows = {r["module"]: r for r in pilot["modules"]}
    if (len(rows) != len(pilot["modules"]) or set(rows) != set(selected) or
            pilot["selected_modules"] != len(rows) or
            pilot["rewritten_modules"] != sum("candidate" in r for r in rows.values())):
        raise ValueError("pilot module inventory differs")
    for row in rows.values():
        if "candidate" in row:
            if (row["executable_saving"] != row["parent_executable_bytes"] - row["candidate"]["executable_bytes"] or
                    row["object_saving"] != row["parent_object_bytes"] - row["candidate"]["object_bytes"]):
                raise ValueError("pilot size arithmetic differs")
        elif row.get("executable_saving", 0) or row.get("object_saving", 0):
            raise ValueError("uncompiled pilot row reports a saving")
    for key in ("executable_saving", "object_saving", "ir_bytes_saved"):
        if pilot[key] != sum(r.get(key, 0) for r in rows.values()):
            raise ValueError("pilot aggregate differs: " + key)
    return {n: r for n, r in rows.items() if r.get("executable_saving", 0) > 0}


def audit(args):
    started = time.monotonic()
    ir = args.ir.resolve()
    folders = [p.resolve() for p in (args.counter_experiment, args.quadword_experiment,
                                    args.multiply_experiment, args.experiment)]
    meta = (ir / "ee_functions.json").read_bytes()
    functions = json.loads(meta)["functions"]
    names = {Path(f["ir_path"]).name: f["host_symbol"] for f in functions}
    plans = [read_json(p / "helper-plan.json") for p in folders]
    manifest_data = [read_manifest(p / "objects", set(names)) for p in folders]
    modes = ("experimental_shared_counter", "experimental_shared_quadword",
             "experimental_shared_cop1_mul", "experimental_shared_cop1_add")
    for i, (folder, plan, (manifest_raw, manifest, _)) in enumerate(zip(folders, plans, manifest_data)):
        dll = folder / "game-translated.dll"
        if (plan["mode"] != modes[i] or plan["metadata_sha256"] != sha(meta) or
                (folder / "ee_functions.json").read_bytes() != meta or
                plan["retained_functions"] != len(names) or
                plan["native_target"] != manifest["native_target"] or
                manifest["native_target"] != manifest_data[-1][1]["native_target"] or
                manifest.get("production_cache_compatible") is not False or
                dll.stat().st_size != plan["linked_library_bytes"] or
                sha(dll.read_bytes()) != plan["linked_library_sha256"]):
            raise ValueError("prior experiment identity differs")
        if i and (plan["source_manifest_sha256"] != sha(manifest_data[i-1][0]) or
                  plan["prior_library_sha256"] != plans[i-1]["linked_library_sha256"]):
            raise ValueError("prior manifest chain differs")
    report = read_json(args.audit)
    if (report["mode"] != "read_only_ee_lowering_cost" or
            report["metadata_sha256"] != sha(meta) or
            report["current_manifest_sha256"] != sha(manifest_data[-1][0]) or
            report["current_library_sha256"] != plans[-1]["linked_library_sha256"] or
            len(report["modules"]) != len(names) or {r["module"] for r in report["modules"]} != set(names)):
        raise ValueError("lowering audit does not match current parent")
    rows = [rows_of(p) for p in plans]
    _, manifest, records = manifest_data[-1]
    selected = select(report["modules"], args.per_band, args.largest)
    if not selected:
        raise ValueError("no selected scalar-memory candidates")
    promoted = None
    if args.promote:
        pilot_raw = args.promote.read_bytes()
        pilot = json.loads(pilot_raw)
        if (pilot["mode"] != "read_only_ee_scalar_memory_pilot" or
                pilot["metadata_sha256"] != sha(meta) or
                pilot["parent_manifest_sha256"] != sha(manifest_data[-1][0]) or
                pilot["parent_library_sha256"] != plans[-1]["linked_library_sha256"] or
                pilot["native_target"] != manifest["native_target"]):
            raise ValueError("pilot provenance differs")
        promoted = winners(pilot, selected)
        if not promoted:
            raise ValueError("no measured executable size wins")
        for row in pilot["modules"]:
            record = records[row["module"]]
            if (row["original_ir_sha256"] != record["ir_sha256"] or
                    row["parent_ir_sha256"] != record["transformed_ir_sha256"] or
                    row["parent_object_sha256"] != record["sha256"] or
                    row["parent_object_bytes"] != record["size_bytes"]):
                raise ValueError("pilot parent row identity differs")
        load_inputs(ir / "ee_functions.json", folders[-1] / "objects")
        selected = sorted(promoted)
        args.output.mkdir(parents=True)
        (args.output / "objects").mkdir()
    tasks = [(n, names[n], ir / n, folders[-1] / "objects", records[n],
              tuple(r.get(n) for r in rows), plans_without_modules(plans), manifest["target_triple"])
             for n in selected]
    if promoted is not None:
        tasks = [task + (args.output / "objects" / (Path(task[0]).stem + ".scalar.o"),)
                 for task in tasks]
    result = []
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for row in pool.map(trial, tasks):
            if promoted is not None and stable_receipt(row) != stable_receipt(promoted[row["module"]]):
                raise ValueError("promoted candidate differs from pilot: " + row["module"])
            result.append(row)
            print(f"[{len(result)}/{len(tasks)}] {row['module']}: "
                  f"{row.get('executable_saving', 0):+,} executable bytes saved", flush=True)
    if promoted is not None:
        changed = {r["module"]: r for r in result}
        entries = []
        for name, record in records.items():
            if name in changed:
                row = changed[name]
                entries.append(dict(record, object=Path(name).stem + ".scalar.o",
                                    size_bytes=row["candidate"]["object_bytes"],
                                    sha256=row["candidate"]["sha256"],
                                    transformed_ir_sha256=row["candidate_ir_sha256"],
                                    experimental_scalar_memory=True))
            else:
                entries.append(dict(record, object=str((folders[-1] / "objects" / record["object"]).resolve())))
        (args.output / "ee_functions.json").write_bytes(meta)
        overlay = dict(manifest, objects=entries, object_bytes=sum(r["size_bytes"] for r in entries),
                       experimental_mixed_pipeline=manifest["experimental_mixed_pipeline"] + "-scalar-pilot",
                       production_cache_compatible=False)
        (args.output / "objects" / "objects.json").write_text(json.dumps(overlay, indent=2) + "\n", encoding="utf-8")
        receipt = {"mode": "experimental_selective_scalar_memory",
                   "metadata_sha256": sha(meta), "source_manifest_sha256": sha(manifest_data[-1][0]),
                   "prior_library_sha256": plans[-1]["linked_library_sha256"],
                   "native_target": manifest["native_target"], "pilot_sha256": sha(pilot_raw),
                   "source_functions": len(names), "retained_functions": len(names),
                   "recompiled_modules": len(result), "reused_modules": len(names)-len(result),
                   "executable_saving": sum(r["executable_saving"] for r in result),
                   "object_saving": sum(r["object_saving"] for r in result),
                   "removed_ir_bytes": sum(r["ir_bytes_saved"] for r in result),
                   "wall_seconds": time.monotonic()-started, "runtime_validation_required": True,
                   "modules": result}
        library = args.output / "game-translated.dll"
        subprocess.run([sys.executable, str(Path(__file__).with_name("link_native_objects.py")),
                        str(args.output / "ee_functions.json"), str(args.output / "objects"), str(library)],
                       check=True)
        data = library.read_bytes()
        receipt.update(linked_library_bytes=len(data), linked_library_sha256=sha(data),
                       linked_executable_raw_bytes=native_executable_sections(data)[0])
        (args.output / "helper-plan.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        return receipt
    return {"mode": "read_only_ee_scalar_memory_pilot", "metadata_sha256": sha(meta),
            "parent_manifest_sha256": sha(manifest_data[-1][0]),
            "parent_library_sha256": plans[-1]["linked_library_sha256"],
            "native_target": manifest["native_target"], "modules": result,
            "selected_modules": len(result),
            "rewritten_modules": sum("candidate" in r for r in result),
            "executable_saving": sum(r.get("executable_saving", 0) for r in result),
            "object_saving": sum(r.get("object_saving", 0) for r in result),
            "ir_bytes_saved": sum(r["ir_bytes_saved"] for r in result),
            "limitations": ["Selected modules only; no whole-image extrapolation.",
                            "Exact canonical scalar regions only; nonmatching regions remain unchanged.",
                            "Local noinline helpers; no runtime or linked DLL measurement."]}


def plans_without_modules(plans):
    return tuple({k: v for k, v in p.items() if k != "modules"} for p in plans)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    for flag in ("counter-experiment", "quadword-experiment", "multiply-experiment", "experiment", "audit"):
        parser.add_argument("--" + flag, type=Path, required=True)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--promote", type=Path, help="existing measured pilot receipt")
    parser.add_argument("--output", type=Path, help="new isolated linked experiment directory")
    parser.add_argument("--per-band", type=int, default=8)
    parser.add_argument("--largest", type=int, default=8)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    try:
        if bool(args.promote) != bool(args.output) or bool(args.json) == bool(args.promote):
            raise ValueError("choose --json or --promote with --output")
        output = (args.output or args.json).resolve()
        if (not 1 <= args.jobs <= 16 or args.per_band < 0 or args.largest < 0 or output.exists() or
                any(output.is_relative_to(p.resolve()) for p in (args.ir, args.counter_experiment,
                     args.quadword_experiment, args.multiply_experiment, args.experiment))):
            raise ValueError("use a new output outside caches and valid sample parameters")
        if args.promote:
            if os.name != "nt" or os.environ.get("SDBZ_NATIVE_ALU_CERTIFICATES") == "1":
                raise ValueError("promotion requires Windows and disabled certificates")
            args.output = output
        report = audit(args)
        if not args.promote:
            with output.open("x", encoding="utf-8") as stream:
                json.dump(report, stream, indent=2)
                stream.write("\n")
        print(f"Scalar memory: {report['executable_saving']:,} executable bytes saved", flush=True)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"EE scalar memory pilot refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
