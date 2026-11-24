#!/usr/bin/env python3
"""Recover known EE functions without running the game or replacing saved IR.

A verified historical translation manifest supplies the known guest entries.
Reconstruction takes place in a new sibling directory. Only after a complete
native library passes cache validation is a tiny active-cache pointer replaced.
The previous cache and every previous recovery remain available for rollback.

IR byte mismatches are NEVER silently accepted. --allow-ir-changes explicitly
opts into rebuilding changed lowering inputs or non-identical output; the full
module set, object manifest and newly linked library are still verified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import subprocess
from datetime import datetime, timezone

import run_game
import run_known_replays


def history_dir(base_work):
    return base_work.parent / "ee-known-manifests"


def pointer_path(base_work):
    return base_work.parent / (base_work.name + ".active.txt")


def active_work(base_work):
    """Only accept a sibling recovered cache, never an arbitrary pointer path."""
    pointer = pointer_path(base_work)
    if not pointer.is_file():
        return base_work
    try:
        value = pointer.read_text(encoding="utf-8").strip()
        path = Path(value).resolve(strict=True)
        parent = base_work.parent.resolve(strict=True)
        if (path.parent == parent and
                path.name.startswith(base_work.name + "-recovery-") and
                path.is_dir()):
            return path
    except (OSError, ValueError):
        pass
    return base_work


def manifest_candidates(base_work):
    candidates = []
    # Profiles share the unoptimized guest IR inventory. A native profile
    # may have discovered more entries than the last base-library snapshot.
    # Only read the known launcher sibling names, not arbitrary directories.
    for source in dict.fromkeys((active_work(base_work), base_work)):
        candidates.append(source / "translation-cache.json")
        for suffix in ("-O1", "-O2", "-O3", "-O2-native"):
            candidates.append(source.with_name(source.name + suffix) /
                              "translation-cache.json")
    directory = history_dir(base_work)
    if directory.is_dir():
        candidates.extend(directory.glob("*.json"))
    # Duplicate file paths do not represent independent evidence.
    return list(dict.fromkeys(candidates))


def select_manifest(base_work, elf, ir_sources, allow_ir_changes=False):
    """Pick the largest authentic-ELF known-function inventory available.

    Prefer same-lowering-source snapshots at equal function counts. A source
    change requires explicit opt-in and never weakens the ELF hash guard.
    """
    expected_elf = run_game.sha256_path(elf)
    selected = []
    for candidate in manifest_candidates(base_work):
        if not candidate.is_file():
            continue
        try:
            manifest, entries = run_known_replays.read_manifest(candidate)
            if manifest["inputs"]["ELF"] != expected_elf:
                continue
            # These profiles all fingerprint the raw guest modules. Do
            # not infer compatibility for an unknown IR transformation.
            if (manifest.get("emit_opt") not in (0, 1, 2, 3, 5) or
                    manifest.get("ir_pipeline") is not None):
                continue
            if candidate.parent == history_dir(base_work):
                # Early snapshots serialized the IR map with sorted keys,
                # losing original discovery order. Never reconstruct from one
                # unless its explicit ordered-entry inventory is present.
                if not isinstance(manifest.get("known_entries"), list):
                    continue
                # Saved snapshots are content-addressed, so a truncated or
                # altered history record cannot quietly supply guest entries.
                canonical = (json.dumps(manifest, sort_keys=True, indent=2) +
                             "\n").encode("utf-8")
                digest = hashlib.sha256(canonical).hexdigest()[:16]
                if (not candidate.name.endswith("-" + digest + ".json") or
                        candidate.read_bytes() != canonical):
                    continue
            same_sources = manifest.get("ir_sources") == ir_sources
            if not same_sources and not allow_ir_changes:
                continue
            selected.append((len(entries), same_sources, candidate.stat().st_mtime_ns,
                             candidate, manifest, entries))
        except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError):
            continue
    if not selected:
        raise ValueError(
            "no known-function manifest matches this ELF and current guest "
            "lowering inputs; use --allow-ir-changes explicitly only when "
            "the guest lowering change was intentional"
        )
    selected.sort(key=lambda item: (item[0], item[1], item[2]), reverse=True)
    _, same_sources, _, candidate, manifest, entries = selected[0]
    return candidate, manifest, entries, same_sources


def new_recovery_work(base_work):
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    target = base_work.parent / (base_work.name + "-recovery-" + stamp)
    if target.exists():
        raise ValueError("recovery staging directory already exists")
    return target


def promote_pointer(base_work, work):
    """Commit only a pointer after verification; retain the old cache."""
    parent = base_work.parent.resolve()
    work = work.resolve(strict=True)
    if (work.parent != parent or
            not work.name.startswith(base_work.name + "-recovery-")):
        raise ValueError("recovery directory is outside the safe sibling namespace")
    pointer = pointer_path(base_work)
    pointer.parent.mkdir(parents=True, exist_ok=True)
    temporary = pointer.with_name(pointer.name + ".tmp")
    try:
        temporary.write_text(str(work) + "\n", encoding="utf-8")
        os.replace(temporary, pointer)
    finally:
        temporary.unlink(missing_ok=True)


def recover(elf, build, config, base_work, jobs, allow_ir_changes=False):
    """Reconstruct, link, validate, then switch active cache without guest runs."""
    source_root = Path(__file__).resolve().parents[1]
    recomp = run_game.native_executable(build, "ee_recomp", config)
    ir_sources = run_game.ir_generation_inputs(elf, source_root)
    candidate, manifest, entries, same_sources = select_manifest(
        base_work, elf, ir_sources, allow_ir_changes
    )
    previous_cache = active_work(base_work)
    work = new_recovery_work(base_work)
    print(
        f"Cache recovery: {len(entries)} known entries from {candidate}; "
        f"lowering_sources_match={same_sources}", flush=True,
    )
    # This function refuses any pre-existing target and verifies the complete
    # module inventory. The explicit opt-in only relaxes OLD IR-byte hashes.
    comparison = run_known_replays.reconstruct(
        manifest, entries, elf, build, config, work, jobs, allow_ir_changes
    )
    (work / "ir-comparison.json").write_text(
        json.dumps(comparison, indent=2) + "\n", encoding="utf-8"
    )
    library_name = ("game-translated.dll" if os.name == "nt" else
                    "libgame-translated.dylib" if sys.platform == "darwin" else
                    "libgame-translated.so")
    run_game.validate_translation_cache(
        work / "translation-cache.json",
        run_game.translation_inputs(elf, recomp, source_root), 0,
        work / "ir" / "ee_functions.json", work / library_name,
    )
    if comparison["missing"] or comparison["extra"] or (
        comparison["mismatched_sha256"] and not allow_ir_changes
    ):
        raise ValueError("reconstructed IR did not meet the recovery policy")

    promote_pointer(base_work, work)
    print(
        f"Cache recovery validated and activated: {work}\n"
        f"Previous cache retained: {previous_cache}\n"
        f"To undo, delete {pointer_path(base_work)}",
        flush=True,
    )
    return work


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--print-active", action="store_true",
                        help="print only the validated active work directory")
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--config", choices=("Debug", "Release", "RelWithDebInfo",
                                            "MinSizeRel"), default="Release")
    parser.add_argument("--base-work-dir", type=Path,
                        default=Path("build/ps2-game-hle"))
    parser.add_argument("--emit-jobs", type=int, default=8)
    parser.add_argument("--allow-ir-changes", action="store_true",
                        help="explicitly allow changed lowering/IR hashes")
    args = parser.parse_args(argv)
    if args.print_active:
        print(active_work(args.base_work_dir.resolve()))
        return 0
    if args.elf is None:
        parser.error("--elf is required for recovery")
    if not 1 <= args.emit_jobs <= 32:
        parser.error("--emit-jobs must be in 1..32")
    try:
        elf = args.elf.resolve(strict=True)
        build = args.build_dir.resolve()
        base_work = args.base_work_dir.resolve()
        recover(elf, build, args.config, base_work, args.emit_jobs,
                args.allow_ir_changes)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Cache recovery stopped without changing the active cache: {error}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
