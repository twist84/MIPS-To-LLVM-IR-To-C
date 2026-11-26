#!/usr/bin/env python3
"""Isolated source-lowering comparison against an authenticated Windows O5 cache.

Regenerate only known retail entries. Require exact function/block ownership
and per-module guest PC coverage before linking. Do not activate a cache,
change the baseline, apply post-processing overlays or execute the game.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time

import run_game
import run_known_replays
import verify_ir
from audit_ee_helper_inventory import HEADER, END, read_manifest, verified_object
from audit_ee_lowering_cost import band, scan_guest
from ee_ownership_binary import coff_file_payload, native_executable_sections


def load_json(path):
    return json.loads(path.read_bytes())


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def separate(left, right):
    return left != right and left not in right.parents and right not in left.parents


def inventory(metadata):
    result = {}
    symbols = set()
    for row in metadata["functions"]:
        name = Path(row["ir_path"]).name
        address = int(row["vaddr"], 0)
        symbol = row["host_symbol"]
        blocks = [int(pc, 0) for pc in row["block_pcs"]]
        if (name in result or symbol in symbols or address not in blocks or
                len(blocks) != len(set(blocks))):
            raise ValueError("invalid function inventory: " + name)
        result[name] = (address, symbol, tuple(sorted(blocks)))
        symbols.add(symbol)
    if len(result) != metadata["function_count"]:
        raise ValueError("function count does not match inventory")
    return result


def ownership_changes(before, after):
    return {
        "missing": sorted(before.keys() - after.keys()),
        "extra": sorted(after.keys() - before.keys()),
        "changed": sorted(name for name in before.keys() & after.keys()
                          if before[name] != after[name]),
    }


def ir_metrics(raw, symbol):
    counts, _, opcodes, _, _, pcs = scan_guest(raw, symbol)
    # Exact LLVM instruction/block counts, including internal helpers.
    with verify_ir.llvm.create_context() as context, verify_ir.llvm.parse_assembly(
            raw.decode("utf-8"), context=context) as module:
        module.verify()
        all_instructions = all_blocks = guest_instructions = guest_blocks = 0
        found_guest = False
        for function in module.functions:
            if function.is_declaration:
                continue
            if function.name == symbol:
                found_guest = True
            for block in function.blocks:
                number = sum(1 for _ in block.instructions)
                all_instructions += number
                all_blocks += 1
                if function.name == symbol:
                    guest_instructions += number
                    guest_blocks += 1
    if not found_guest:
        raise ValueError("missing parsed guest definition: " + symbol)
    # LLVM may materialize implicit unnamed blocks after terminators.
    # Lexical attribution uses emitted annotations, not LLVM's CFG.
    # Report differences explicitly; use verified LLVM counts as totals.
    return {
        "ir_bytes": len(raw),
        "llvm_instructions": all_instructions,
        "llvm_blocks": all_blocks,
        "guest_llvm_instructions": guest_instructions,
        "guest_llvm_blocks": guest_blocks,
        "lexical_guest_instructions": counts["ir_instructions"],
        "lexical_guest_blocks": counts["llvm_blocks"],
        "lexical_count_disagreement": int(
            guest_instructions != counts["ir_instructions"] or
            guest_blocks != counts["llvm_blocks"]),
        "guest_pcs": len(pcs),
    }, opcodes, pcs


# For runtime-first builds only, match the same PC union used by the
# lexical scanner without computing the expensive per-opcode IR cost.
PC_ANNOTATION = re.compile(rb"^\\s*;\\s+(0x[0-9a-fA-F]+):\\s+\\S+", re.M)
PC_COUNT = re.compile(rb"^\\s*call void @ee_count_guest_instruction\\(ptr %rt, i32 (\\d+), i32 \\d+\\)", re.M)


def coverage_metrics(raw, symbol):
    matches = [match for match in HEADER.finditer(raw)
               if match[1].decode("ascii") == symbol]
    if len(matches) != 1:
        raise ValueError("expected exactly one guest definition: " + symbol)
    start = matches[0]
    end = END.search(raw, start.end())
    if end is None:
        raise ValueError("missing guest definition end: " + symbol)
    body = raw[start.end():end.start()]
    pcs = {int(match[1], 16) for match in PC_ANNOTATION.finditer(body)}
    pcs.update(int(match[1]) for match in PC_COUNT.finditer(body))
    return {"ir_bytes": len(raw), "guest_pcs": len(pcs)}, {}, pcs


def native_metrics(data):
    return dict(coff_file_payload(data),
                native_executable_bytes=native_executable_sections(data)[0],
                object_bytes=len(data))


def linked_metrics(path):
    data = path.read_bytes()
    executable, virtual = native_executable_sections(data)
    return {"bytes": len(data), "sha256": digest(data),
            "executable_bytes": executable, "virtual_executable_bytes": virtual}


def aggregate_opcode(target, values):
    for opcode, counts in values.items():
        target[opcode].update({
            key: counts[key] for key in (
                "function_scoped_annotated_pcs", "annotation_occurrences",
                "ir_instructions", "ir_instruction_line_bytes")
        })


def write_receipt(path, report):
    verify_ir.write_atomic(
        path, (json.dumps(report, indent=2) + "\n").encode("utf-8"))


RESUME_IDENTITY_FIELDS = (
    "schema", "profile", "baseline_metadata_sha256",
    "baseline_translation_sha256", "baseline_object_manifest_sha256",
    "baseline_library", "native_target", "elf_sha256", "functions",
    "candidate_lowering_inputs", "recompiler_sha256", "candidate_work",
)


def validate_resume(previous, current, comparison, metadata_hash, hashes):
    for field in RESUME_IDENTITY_FIELDS:
        if field not in previous or previous[field] != current[field]:
            raise ValueError("resume identity differs: " + field)
    if (previous.get("retail_replay_performed") is not False or
            previous.get("candidate_metadata_sha256") != metadata_hash or
            previous.get("reconstruction") != comparison or
            comparison["missing"] or comparison["extra"]):
        raise ValueError("resume reconstruction/metadata identity differs")
    old_hashes = previous.get("candidate_ir_hashes")
    if old_hashes is not None and old_hashes != hashes:
        raise ValueError("resumed candidate IR hashes differ")
    return old_hashes is not None


def compare(args, report):
    source_root = Path(__file__).resolve().parents[1]
    elf, build = args.elf.resolve(strict=True), args.build_dir.resolve(strict=True)
    base_ir = args.baseline_ir.resolve(strict=True)
    profile = args.baseline_profile.resolve(strict=True)
    work = args.work_dir.resolve()
    receipt = args.output.resolve()
    for protected in (base_ir.parent, profile):
        if not separate(protected, work):
            raise ValueError("candidate must be isolated from baseline cache")
        if receipt == protected or protected in receipt.parents:
            raise ValueError("receipt must not write into baseline cache")
    if work.exists() and args.resume_from is None:
        raise ValueError("candidate work directory already exists; use --resume-from or choose a fresh path")
    if args.resume_from is not None and not work.is_dir():
        raise ValueError("resume requires the original candidate work directory")
    if os.environ.get("SDBZ_NATIVE_ALU_CERTIFICATES", "0") != "0":
        raise ValueError("disable native ALU certificates for comparable O5 output")

    manifest_path = profile / "translation-cache.json"
    manifest, entries = run_known_replays.read_manifest(manifest_path)
    meta_path = base_ir / "ee_functions.json"
    meta_raw = meta_path.read_bytes()
    before = inventory(json.loads(meta_raw))
    names = set(before)
    if (manifest.get("emit_opt") != 5 or manifest.get("inputs", {}).get("ELF") !=
            run_game.sha256_path(elf) or
            manifest.get("inputs", {}).get("native_alu_certificate_mode") !=
            digest(b"0") or manifest.get("metadata_sha256") != digest(meta_raw) or
            set(manifest["ir_hashes"]) != names or len(entries) != len(names) or
            (args.expected_functions is not None and len(names) != args.expected_functions)):
        raise ValueError("baseline O5 ELF, metadata or inventory differs")
    if run_game.translated_ir_hashes(meta_path) != manifest["ir_hashes"]:
        raise ValueError("baseline IR bytes differ from saved O5 provenance")
    base_objects = profile / "objects"
    base_manifest_raw, base_manifest, base_rows = read_manifest(base_objects, names)
    expected_target = {
        "native_target": verify_ir.native_target_identity(5),
        "target_triple": verify_ir.llvm.get_default_triple(),
        "ir_pipeline": verify_ir.ir_pipeline_id(5),
        "llvm_version": ".".join(map(str, verify_ir.llvm.llvm_version_info)),
    }
    if any(base_manifest.get(k) != v for k, v in expected_target.items()):
        raise ValueError("native compiler target/pipeline differs from baseline")
    baseline_dll = linked_metrics(profile / "game-translated.dll")
    if (baseline_dll["sha256"] != manifest.get("library_sha256") or
            (args.expected_baseline_bytes is not None and
             baseline_dll["bytes"] != args.expected_baseline_bytes)):
        raise ValueError("baseline linked DLL identity differs")
    report.update({
        "baseline_metadata_sha256": digest(meta_raw),
        "baseline_translation_sha256": run_game.sha256_path(manifest_path),
        "baseline_object_manifest_sha256": digest(base_manifest_raw),
        "baseline_library": baseline_dll, "native_target": expected_target,
        "elf_sha256": manifest["inputs"]["ELF"], "functions": len(names),
        "candidate_lowering_inputs": run_game.ir_generation_inputs(elf, source_root),
        "recompiler_sha256": run_game.sha256_path(
            run_game.native_executable(build, "ee_recomp", args.config)),
        "candidate_work": str(work),
    })

    candidate_ir = work / "ir"
    candidate_meta = candidate_ir / "ee_functions.json"
    previous = None
    if args.resume_from is not None:
        previous = load_json(args.resume_from)
        comparison = run_known_replays.compare_ir(work, manifest["ir_hashes"])
        report["ir_generation_seconds"] = previous["ir_generation_seconds"]
    else:
        started = time.monotonic()
        comparison = run_known_replays.reconstruct(
            manifest, entries, elf, build, args.config, work, args.jobs,
            True, ir_only=True)
        report["ir_generation_seconds"] = time.monotonic() - started
    report["reconstruction"] = comparison
    report["candidate_metadata_sha256"] = run_game.sha256_path(candidate_meta)
    candidate_hashes = run_game.hash_ir_modules(
        candidate_ir, load_json(candidate_meta)["functions"],
        require_owned_directory=True)
    if set(candidate_hashes) != names:
        raise ValueError("candidate IR inventory differs")
    if previous is not None:
        exact = validate_resume(previous, report, comparison,
                                report["candidate_metadata_sha256"], candidate_hashes)
        report["resume"] = {
            "receipt_sha256": run_game.sha256_path(args.resume_from),
            "previous_candidate_hashes_verified": exact,
            "ir_regenerated": False,
        }
    report["candidate_ir_hashes"] = candidate_hashes
    after = inventory(load_json(candidate_meta))
    changes = ownership_changes(before, after)
    report["ownership_changes"] = changes
    if any(changes.values()):
        raise ValueError("reconstructed function/block ownership changed; no comparison accepted")

    report["stage"] = "reconstruction_complete"
    write_receipt(receipt, report)
    measure = coverage_metrics if args.performance_first else ir_metrics
    report["performance_first"] = args.performance_first
    started = time.monotonic()
    totals = {arm: Counter() for arm in ("before", "after")}
    by_opcode = {arm: defaultdict(Counter) for arm in totals}
    by_band = {arm: defaultdict(Counter) for arm in totals}
    rows = []
    coverage_changes = []
    for index, name in enumerate(sorted(names), 1):
        symbol = before[name][1]
        old_raw, new_raw = (base_ir / name).read_bytes(), (candidate_ir / name).read_bytes()
        old_hash, new_hash = digest(old_raw), digest(new_raw)
        if old_hash != manifest["ir_hashes"][name]:
            raise ValueError("baseline IR changed during measurement: " + name)
        if new_hash != candidate_hashes[name]:
            raise ValueError("candidate IR changed during measurement: " + name)
        a = measure(old_raw, symbol)
        b = a if old_hash == new_hash else measure(new_raw, symbol)
        if a[2] != b[2]:
            coverage_changes.append({
                "module": name, "missing": sorted(a[2] - b[2]),
                "extra": sorted(b[2] - a[2]),
            })
        row = {"module": name, "symbol": symbol, "before_sha256": old_hash,
               "after_sha256": new_hash, "before": a[0], "after": b[0]}
        rows.append(row)
        for arm, values in (("before", a), ("after", b)):
            totals[arm].update(values[0])
            by_band[arm][band(values[0]["guest_pcs"])].update(values[0])
            if not args.performance_first:
                aggregate_opcode(by_opcode[arm], values[1])
        if index % 256 == 0 or index == len(names):
            print(f"[{index}/{len(names)}] paired IR metrics", flush=True)
    report.update({"ir_scan_seconds": time.monotonic() - started,
                   "ir_totals": totals, "opcodes": by_opcode, "function_bands": by_band,
                   "guest_pc_changes": coverage_changes, "modules": rows,
                   "lexical_count_disagreements": {
                       arm: [row["module"] for row in rows
                             if row[arm].get("lexical_count_disagreement", 0)]
                       for arm in totals
                   }})
    if not args.performance_first:
        for arm, values in totals.items():
            values["llvm_instructions_per_guest_pc"] = (
                values["llvm_instructions"] / values["guest_pcs"])
            values["guest_llvm_instructions_per_guest_pc"] = (
                values["guest_llvm_instructions"] / values["guest_pcs"])
    if coverage_changes:
        raise ValueError("function-scoped guest instruction coverage changed")
    report["stage"] = "static_ir_complete"
    write_receipt(receipt, report)
    if args.ir_only:
        return

    candidate_objects = work / "objects"
    candidate_library = work / "game-translated.dll"
    started = time.monotonic()
    run_game.build_linked_library(
        source_root, work, candidate_meta, candidate_objects, candidate_library,
        5, emit_jobs=args.jobs, emit_only=True)
    report["native_compilation_seconds"] = time.monotonic() - started
    candidate_raw, candidate_manifest, candidate_rows = read_manifest(
        candidate_objects, names)
    if any(candidate_manifest.get(k) != v for k, v in expected_target.items()):
        raise ValueError("candidate compiler target/pipeline differs")
    native_totals = {arm: Counter() for arm in totals}
    native_bands = {arm: defaultdict(Counter) for arm in totals}
    for row in rows:
        name = row["module"]
        for arm, directory, record in (
                ("before", base_objects, base_rows[name]),
                ("after", candidate_objects, candidate_rows[name])):
            if row["symbol"] not in record["defined_symbols"]:
                raise ValueError("missing guest symbol: " + name)
            data = verified_object(directory, record, row[arm + "_sha256"])
            metrics = {"object_bytes": len(data)} if args.performance_first else native_metrics(data)
            row[arm].update(metrics, object_sha256=record["sha256"])
            native_totals[arm].update(metrics)
            native_bands[arm][band(row[arm]["guest_pcs"])].update(metrics)
    started = time.monotonic()
    run_game.build_linked_library(
        source_root, work, candidate_meta, candidate_objects, candidate_library,
        5, reuse_objects=True)
    report["link_seconds"] = time.monotonic() - started
    report.update({
        "candidate_object_manifest_sha256": digest(candidate_raw),
        "candidate_library": linked_metrics(candidate_library),
        "native_totals": native_totals, "native_function_bands": native_bands,
        "stage": "linked_o5_candidate", "runtime_validation_required": True,
    })
    # Recheck the authenticated baseline identities after the isolated build.
    if (run_game.sha256_path(meta_path) != report["baseline_metadata_sha256"] or
            run_game.sha256_path(manifest_path) != report["baseline_translation_sha256"] or
            run_game.sha256_path(base_objects / "objects.json") !=
            report["baseline_object_manifest_sha256"] or
            run_game.sha256_path(profile / "game-translated.dll") != baseline_dll["sha256"]):
        raise ValueError("baseline changed during comparison")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--baseline-ir", type=Path, required=True)
    parser.add_argument("--baseline-profile", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--config", default="Release")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--expected-functions", type=int)
    parser.add_argument("--expected-baseline-bytes", type=int)
    parser.add_argument("--ir-only", action="store_true")
    parser.add_argument("--performance-first", action="store_true",
                        help="verify ownership/PC coverage and compile; omit detailed IR and COFF cost scans")
    parser.add_argument("--resume-from", type=Path,
                        help="prior receipt for the same isolated reconstruction")
    args = parser.parse_args(argv)
    if args.performance_first and args.ir_only:
        parser.error("--performance-first requires native LLVM verification, not --ir-only")
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be in 1..32")
    output = args.output.resolve()
    if args.resume_from is not None and args.resume_from.resolve() == output:
        parser.error("choose a new output receipt when resuming")
    if output.exists():
        parser.error("--output already exists; choose a new receipt")
    for protected in (args.baseline_ir.resolve().parent, args.baseline_profile.resolve()):
        if output == protected or protected in output.parents:
            parser.error("--output must not be inside the baseline cache")
    if args.work_dir.resolve() == output or args.work_dir.resolve() in output.parents:
        parser.error("--output must be outside the isolated work directory")
    report = {"schema": "ee-source-lowering-comparison-v1",
              "profile": 5, "stage": "preflight", "retail_replay_performed": False}
    try:
        compare(args, report)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError,
            subprocess.CalledProcessError) as error:
        report["error"] = str(error)
        write_receipt(output, report)
        print(f"source-lowering comparison refused: {error}", file=sys.stderr)
        return 1
    write_receipt(output, report)
    print(f"Source-lowering comparison: {report['stage']}; receipt {output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
