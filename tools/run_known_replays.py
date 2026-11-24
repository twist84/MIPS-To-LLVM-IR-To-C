#!/usr/bin/env python3
"""Reproduce matched SDBZ neutral/Cross native replays without auto-expansion.

Two modes:
  Reuse the locally verified guest IR, objects and translated library:
    python tools/run_known_replays.py --disc-root PS2_GAME

  Reconstruct *known* guest entries from an archived translation-cache.json:
    python tools/run_known_replays.py --disc-root PS2_GAME \
      --manifest sdbz-graphics-pr148-20261121-203021.zip \
      --reconstruct --work-dir build/sdbz-known-1275

Reconstruction uses ee_recomp's batch entry API and the existing append
ownership checks. There are NO game replays during reconstruction, and only
one final LLVM object emission/link. This never rewrites the user's main cache.
The archived SHA-256 list authenticates IR identities, not their contents.
"""
import argparse
import csv
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile

import run_game


IR_NAME = re.compile(r"sub_0x([0-9a-fA-F]{1,8})_0x([0-9a-fA-F]{1,8})\.ll")
DIGEST = re.compile(r"[0-9a-f]{64}")
CROSS_PC = "0x00109d50"
DEFAULT_WORK = Path("build/ps2-game-hle")


def read_manifest(path):
    """Accept a translation-cache.json or one diagnostic ZIP containing it."""
    path = Path(path)
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            matches = [name for name in archive.namelist()
                       if Path(name).name == "translation-cache.json"]
            if len(matches) != 1:
                raise ValueError("archive must contain exactly one translation-cache.json")
            data = json.loads(archive.read(matches[0]).decode("utf-8"))
    else:
        data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != run_game.CACHE_SCHEMA:
        raise ValueError("unknown translation-cache schema")
    hashes = data.get("ir_hashes")
    if not isinstance(hashes, dict) or not hashes:
        raise ValueError("manifest contains no IR hashes")
    order = data.get("known_entries")
    if order is None:
        # Older translation-cache.json records preserve metadata order
        # through JSON object insertion order.
        order = list(hashes)
    elif (not isinstance(order, list) or len(order) != len(hashes) or
          any(not isinstance(name, str) for name in order) or
          len(set(order)) != len(order) or set(order) != set(hashes)):
        raise ValueError("known-function order does not match IR inventory")
    entries = []
    for name in order:
        digest = hashes[name]
        match = IR_NAME.fullmatch(name)
        if not match or match.group(1).lower() != match.group(2).lower():
            raise ValueError(f"unsafe/unsupported IR module name: {name}")
        if not isinstance(digest, str) or not DIGEST.fullmatch(digest):
            raise ValueError(f"invalid SHA-256 for {name}")
        address = int(match.group(1), 16)
        if address == 0 or address & 3 or address in entries:
            raise ValueError(f"duplicate/unaligned function address: 0x{address:08x}")
        entries.append(address)
    inputs = data.get("inputs")
    elf_hash = inputs.get("ELF") if isinstance(inputs, dict) else None
    if not isinstance(elf_hash, str) or not DIGEST.fullmatch(elf_hash):
        raise ValueError("manifest is missing a valid ELF SHA-256")
    return data, entries


def compare_ir(work_dir, hashes):
    """Compare the precise named module set and contents, not just a count."""
    ir_dir = Path(work_dir) / "ir"
    observed = {path.name: run_game.sha256_path(path)
                for path in ir_dir.glob("*.ll")}
    missing = sorted(set(hashes) - set(observed))
    extra = sorted(set(observed) - set(hashes))
    changed = sorted(name for name in set(observed) & set(hashes)
                     if observed[name] != hashes[name])
    return {
        "expected": len(hashes),
        "observed": len(observed),
        "matching": len(hashes) - len(missing) - len(changed),
        "missing": missing,
        "extra": extra,
        "mismatched_sha256": changed,
    }


def reconstruct(manifest, entries, elf, build, config, work, jobs,
                allow_mismatches):
    """Build known guest entries without any intermediate game execution."""
    ir_dir = work / "ir"
    for candidate in (ir_dir, work / "objects", work / "translation-cache.json",
                      work / "game-translated.dll", work / "libgame-translated.so",
                      work / "libgame-translated.dylib"):
        if candidate.exists():
            raise ValueError(
                f"reconstruction target is not fresh: {candidate}\n"
                "Choose a new --work-dir; existing IR is never replaced."
            )
    expected_elf = manifest["inputs"]["ELF"]
    actual_elf = run_game.sha256_path(elf)
    if expected_elf != actual_elf:
        raise ValueError("ELF SHA-256 differs from archived translation manifest")
    recomp = run_game.native_executable(build, "ee_recomp", config)
    source_root = Path(__file__).resolve().parents[1]
    metadata_path = ir_dir / "ee_functions.json"
    ir_dir.mkdir(parents=True, exist_ok=False)
    # First entry seeds full-ELF discovery and writes the normal metadata.
    # Subsequent batches reuse the discovery cache and exact ownership logic
    # used by production auto-expansion, WITHOUT replaying the game.
    print(f"Known-function reconstruction: {len(entries)} entries", flush=True)
    run_game.run([recomp, elf, ir_dir, f"--function=0x{entries[0]:08x}"])
    for first in range(1, len(entries), 128):
        batch = entries[first:first + 128]
        print(f"Known IR entries {first + 1}..{first + len(batch)} "
              f"of {len(entries)}", flush=True)
        run_game.batch_append_functions(recomp, elf, work, metadata_path, batch)

    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    generated = [int(item["vaddr"], 0) for item in metadata["functions"]]
    if len(generated) != len(entries) or set(generated) != set(entries):
        raise ValueError(
            f"reconstructed entries differ: expected={len(entries)} "
            f"generated={len(generated)}"
        )
    comparison = compare_ir(work, manifest["ir_hashes"])
    print("Known IR SHA-256 verification: "
          f"{comparison['matching']}/{comparison['expected']} identical; "
          f"mismatches={len(comparison['mismatched_sha256'])}; "
          f"missing={len(comparison['missing'])}; "
          f"extra={len(comparison['extra'])}", flush=True)
    if comparison["missing"] or comparison["extra"]:
        raise ValueError("reconstructed module set differs from known manifest")
    if comparison["mismatched_sha256"] and not allow_mismatches:
        raise ValueError(
            "IR SHA-256 mismatch: " +
            ", ".join(comparison["mismatched_sha256"][:12]) +
            "\nUse --allow-ir-mismatches in an ISOLATED work directory only "
            "after inspecting the differences; parity is not established."
        )
    run_game.shared_ir.normalize_metadata(metadata_path, elf)
    # Emit all guest objects once, then link one library. Never run a guest
    # between function batches or rebuild guest code just for host changes.
    lib_name = "game-translated.dll" if os.name == "nt" else (
        "libgame-translated.dylib" if sys.platform == "darwin" else
        "libgame-translated.so"
    )
    library = work / lib_name
    run_game.build_linked_library(
        source_root, work, metadata_path, work / "objects",
        library, 0, emit_jobs=jobs,
    )
    run_game.write_translation_cache(
        work / "translation-cache.json",
        run_game.translation_inputs(elf, recomp, source_root),
        0, metadata_path, library,
        run_game.ir_generation_inputs(elf, source_root),
    )
    return comparison


def prepare_existing(elf, build, config, work, jobs):
    """Validate cached guest IR, or safely relink HOST objects if needed."""
    source_root = Path(__file__).resolve().parents[1]
    base = [
        sys.executable, str(source_root / "tools" / "run_game.py"),
        str(elf), "--build-dir", str(build), "--config", config,
        "--work-dir", str(work), "--emit-opt=0",
        f"--emit-jobs={jobs}", "--link-only",
    ]
    validation = subprocess.run([*base, "--no-recompile"], check=False)
    if validation.returncode == 0:
        return
    print("Exact cache validation failed; attempting verified host relink. "
          "Saved guest IR will not be deleted.", flush=True)
    relink = subprocess.run([*base, "--relink-existing"], check=False)
    if relink.returncode != 0:
        raise ValueError(
            "saved guest IR failed both exact validation and safe host relink; "
            "not regenerating it. Preserve the existing work directory."
        )


def trace_branch_count(path):
    if not path.is_file():
        raise ValueError(f"missing original EE instruction trace: {path}")
    count = 0
    truncated = False
    with path.open(encoding="utf-8") as source:
        rows = csv.reader(source)
        header = next(rows, [])
        if header[:2] != ["attempt", "pc"]:
            raise ValueError("unexpected EE instruction trace format")
        for row in rows:
            if row and row[0].startswith("#"):
                truncated = "truncated=true" in row[0]
                continue
            if len(row) > 1 and row[1].lower() == CROSS_PC:
                count += 1
    if truncated:
        raise ValueError("instruction trace truncated; branch count incomplete")
    return count


def replay_pair(elf, disc, build, config, work, output,
                max_dispatches, timeout_seconds, jobs):
    """Execute both experiments against exactly one validated native DLL."""
    source_root = Path(__file__).resolve().parents[1]
    output.mkdir(parents=True, exist_ok=True)
    base = [
        sys.executable, str(source_root / "tools" / "run_game.py"),
        str(elf), "--build-dir", str(build), "--config", config,
        "--work-dir", str(work), "--emit-opt=0",
        f"--emit-jobs={jobs}", "--no-recompile", "--auto-expand=0",
        "--memcard-discard-writes",
        f"--max-dispatches={max_dispatches}",
        f"--timeout-seconds={timeout_seconds}",
        "--loop-budget=1000000", "--loop-yield-resumes=16",
        "--sif-poll-limit=2048", "--disc-folder", str(disc),
        "--hle-startup", "--kseg-ram",
    ]
    library_cache = work / "translation-cache.json"
    before = library_cache.read_bytes()
    outcomes = {}
    for profile in ("neutral", "cross"):
        report = output / f"{profile}.json"
        trace = output / f"{profile}-branch.csv"
        if report.exists() or trace.exists():
            raise ValueError(f"existing replay evidence would be overwritten: {profile}")
        env = os.environ.copy()
        env.pop("SDBZ_PAD0_CROSS_FIRST_POLL", None)
        env.pop("SDBZ_PAD0_CROSS_POLL_COUNT", None)
        if profile == "cross":
            env["SDBZ_PAD0_CROSS_FIRST_POLL"] = "200"
            env["SDBZ_PAD0_CROSS_POLL_COUNT"] = "8"
        command = [
            *base, "--report", str(report),
            "--trace-instructions", str(trace),
            "--trace-instructions-window=0x00109d50:0x00109d50",
        ]
        print(f"=== Known-cache CPU-only {profile} replay ===", flush=True)
        result = subprocess.run(command, env=env, check=False)
        if result.returncode not in (0, 3, 4):
            raise ValueError(
                f"{profile} replay failed with status {result.returncode}; "
                "no result parity claimed"
            )
        if not report.is_file():
            raise ValueError(f"{profile} run did not produce report")
        report_data = json.loads(report.read_text(encoding="utf-8"))
        if report_data.get("dispatches") != max_dispatches:
            raise ValueError(
                f"{profile} stopped after {report_data.get('dispatches')} "
                f"dispatches; expected {max_dispatches}; replay parity is incomplete"
            )
        count = trace_branch_count(trace)
        outcomes[profile] = {
            "exit_code": result.returncode,
            "dispatches": report_data.get("dispatches"),
            "final_pc": report_data.get("final_pc"),
            "original_branch_0x00109d50": count,
            "report": str(report),
            "instruction_trace": str(trace),
        }
        if library_cache.read_bytes() != before:
            raise ValueError("cache metadata changed during known-function replay")
    summary = {
        "schema": "ee-known-function-matched-replays-v1",
        "guest_cache_rebuilt_during_replays": False,
        "work_dir": str(work),
        "max_dispatches": max_dispatches,
        "results": outcomes,
    }
    (output / "comparison.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2), flush=True)
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disc-root", type=Path, default=Path("PS2_GAME"))
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--config", default="Release",
                        choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"))
    parser.add_argument("--work-dir", type=Path, default=DEFAULT_WORK)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--manifest", type=Path,
                        help="Prior translation-cache.json or diagnostic ZIP")
    parser.add_argument("--reconstruct", action="store_true",
                        help="Regenerate known entries in a fresh work directory")
    parser.add_argument("--allow-ir-mismatches", action="store_true",
                        help="Explicitly allow non-identical reconstructed IR")
    parser.add_argument("--emit-jobs", type=int, default=8)
    parser.add_argument("--max-dispatches", type=int, default=6000000)
    parser.add_argument("--timeout-seconds", type=int, default=600)
    args = parser.parse_args(argv)
    if args.reconstruct and args.manifest is None:
        parser.error("--reconstruct requires --manifest")
    if args.reconstruct and args.work_dir.resolve() == DEFAULT_WORK.resolve():
        parser.error("--reconstruct requires an isolated --work-dir")
    if args.allow_ir_mismatches and not args.reconstruct:
        parser.error("--allow-ir-mismatches requires --reconstruct")
    if args.emit_jobs not in range(1, 33):
        parser.error("--emit-jobs must be between 1 and 32")
    if args.max_dispatches <= 0 or not 1 <= args.timeout_seconds <= 3600:
        parser.error("invalid replay limits")
    source_root = Path(__file__).resolve().parents[1]
    disc = args.disc_root.resolve(strict=True)
    if not disc.is_dir():
        parser.error("--disc-root must be a directory")
    resolve_boot = source_root / "tools" / "resolve_ps2_boot.py"
    boot = subprocess.check_output(
        [sys.executable, str(resolve_boot), str(disc)], text=True
    ).strip()
    elf = Path(boot).resolve(strict=True)
    build = args.build_dir.resolve()
    work = args.work_dir.resolve()
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output = (args.output_dir or (work / "known-replays" / timestamp)).resolve()
    if output == work or output == work / "ir" or output == work / "objects":
        parser.error("--output-dir cannot replace guest cache directories")
    try:
        if args.reconstruct:
            manifest, entries = read_manifest(args.manifest)
            comparison = reconstruct(
                manifest, entries, elf, build, args.config, work,
                args.emit_jobs, args.allow_ir_mismatches,
            )
            output.mkdir(parents=True, exist_ok=True)
            (output / "ir-comparison.json").write_text(
                json.dumps(comparison, indent=2) + "\n", encoding="utf-8"
            )
        else:
            prepare_existing(elf, build, args.config, work, args.emit_jobs)
            if args.manifest is not None:
                manifest, _ = read_manifest(args.manifest)
                if run_game.sha256_path(elf) != manifest["inputs"]["ELF"]:
                    raise ValueError("ELF differs from expected manifest")
                compare = compare_ir(work, manifest["ir_hashes"])
                if compare["missing"] or compare["extra"] or compare["mismatched_sha256"]:
                    raise ValueError("local cache does not match expected known IR hashes")
        replay_pair(
            elf, disc, build, args.config, work, output,
            args.max_dispatches, args.timeout_seconds, args.emit_jobs,
        )
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError,
            zipfile.BadZipFile) as exc:
        print(f"known-replays: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
