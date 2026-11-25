#!/usr/bin/env python3
"""Explicitly overwrite canonical shared EE IR and build one native profile."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import recover_translation_cache as recovery
import run_game
import run_known_replays as known
import shared_ee_ir as shared_ir


PROFILE_DIRS = {0: "", 1: "O1", 2: "O2", 3: "O3",
                4: "IR-O2", 5: "O2-native"}


def active_manifest(elf, base):
    """Use the actual shared inventory, not a possibly stale O0 snapshot."""
    metadata_path = base / "ir" / "ee_functions.json"
    metadata = shared_ir.read_json(metadata_path)
    if shared_ir.elf_path(metadata, metadata_path.parent) != elf:
        raise ValueError("active EE cache references another ELF")
    if shared_ir.active(metadata_path):
        state = shared_ir.read_json(metadata_path.parent / shared_ir.PROVENANCE)
        state = shared_ir.current_state(metadata_path, state["ir_sources"])
        sources = state["ir_sources"]
        hashes = state["ir_hashes"]
        manifest = {
            "schema": run_game.CACHE_SCHEMA,
            "inputs": {"ELF": sources["ELF"]},
            "ir_hashes": hashes,
        }
    else:
        manifest, _ = known.read_manifest(base / "translation-cache.json")
        if manifest.get("metadata_sha256") != run_game.sha256_path(metadata_path):
            raise ValueError("active EE function metadata changed")
        hashes = manifest["ir_hashes"]
    if manifest["inputs"]["ELF"] != run_game.sha256_path(elf):
        raise ValueError("active EE cache ELF SHA-256 differs")
    order = [fn["ir_path"] for fn in metadata["functions"]]
    if (not order or len(order) != len(set(order)) or set(order) != set(hashes)
            or metadata["function_count"] != len(order)):
        raise ValueError("active EE inventory does not match cached provenance")
    # Reuse the exact existing manifest parser, including filename/PC checks.
    manifest["known_entries"] = order
    return manifest


def refresh(elf, build, config, base, opt, jobs):
    elf = elf.resolve(strict=True)
    base = base.resolve(strict=True)
    work = base / PROFILE_DIRS[opt]
    root = Path(__file__).resolve().parents[1]
    with shared_ir.ir_lock(base):
        ir = base / "ir"
        if (ir.is_symlink() or shared_ir.ir_directory(base) != ir or
                not recovery.canonical_portable(base, base)):
            raise ValueError("overwrite requires a canonical shared EE cache; migrate legacy caches first")
        if work != base and work.exists():
            if (work.is_symlink() or (work / "ir").exists() or
                    shared_ir.ir_directory(work) != ir):
                raise ValueError("selected native profile is not attached to the canonical EE IR")
        manifest = active_manifest(elf, base)
        sources = run_game.ir_generation_inputs(elf, root)
        recomp = run_game.native_executable(build, "ee_recomp", config)
        inputs = run_game.translation_inputs(elf, recomp, root)
        stage = Path(tempfile.mkdtemp(prefix=".overwrite-ir-", dir=base))
        previous = stage / "previous-ir"
        staged_ir = stage / "ir"
        active_replaced = False
        old_moved = False
        cache = work / "translation-cache.json"
        old_snapshot = cache.read_bytes() if cache.is_file() else None
        try:
            inventory = stage / "known-entries.json"
            shared_ir.write_json(inventory, manifest)
            manifest, entries = known.read_manifest(inventory)
            print(f"Overwrite EE IR: {len(entries)} known entries; selected O{opt}", flush=True)
            comparison = known.reconstruct(
                manifest, entries, elf, build, config, stage, jobs, True,
                ir_only=True)
            if comparison["missing"] or comparison["extra"]:
                raise ValueError("re-emitted module inventory changed")
            staged_meta = staged_ir / "ee_functions.json"
            metadata = shared_ir.read_json(staged_meta)
            hashes = run_game.hash_ir_modules(staged_ir, metadata["functions"],
                                              require_owned_directory=True)
            if list(hashes) != manifest["known_entries"]:
                raise ValueError("re-emitted ordered EE inventory changed")
            # Rebase the external ELF reference BEFORE publishing the new
            # provenance record and moving the directory into its final path.
            metadata["elf"] = shared_ir.relative_path(elf, ir)
            shared_ir.write_json(staged_meta, metadata)
            shared_ir.publish(staged_meta, sources, hashes)
            ir.rename(previous)
            old_moved = True
            staged_ir.rename(ir)
            active_replaced = True
            metadata_path = ir / "ee_functions.json"
            if shared_ir.elf_path(metadata, ir) != elf:
                raise ValueError("re-emitted ELF reference changed")
            shared_ir.current_state(metadata_path, sources, hashes)
            work.mkdir(parents=True, exist_ok=True)
            if work != base:
                shared_ir.attach(work, ir)
            library_name = ("game-translated.dll" if os.name == "nt" else
                            "libgame-translated.dylib" if sys.platform == "darwin" else
                            "libgame-translated.so")
            library = work / library_name
            # Existing objects are incremental candidates only. verify_ir
            # checks current IR bytes, target, LLVM version and object SHA.
            run_game.build_linked_library(root, work, metadata_path, work / "objects",
                                          library, opt, emit_jobs=jobs)
            run_game.write_translation_cache(cache, inputs, opt, metadata_path,
                                             library, sources)
            run_game.validate_translation_cache(cache, inputs, opt, metadata_path, library)
        except BaseException:
            # Do not authorize a replay on failure. Restore source and its
            # old snapshot; modified native objects still need validation.
            # If a rename fails, retain the staging directory for recovery.
            if active_replaced:
                ir.rename(staged_ir)
            if old_moved:
                previous.rename(ir)
            if active_replaced:
                if old_snapshot is None:
                    cache.unlink(missing_ok=True)
                else:
                    cache.write_bytes(old_snapshot)
            shutil.rmtree(stage)
            raise
        shutil.rmtree(stage)
        print(f"Overwrote {len(entries)} EE IR modules; validated O{opt} cache: {work}",
              flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--config", default="Release")
    parser.add_argument("--base-work-dir", required=True, type=Path)
    parser.add_argument("--emit-opt", type=int, choices=range(6), required=True)
    parser.add_argument("--emit-jobs", type=int, default=8)
    args = parser.parse_args(argv)
    if not 1 <= args.emit_jobs <= 32:
        parser.error("--emit-jobs must be in 1..32")
    try:
        refresh(args.elf, args.build_dir, args.config, args.base_work_dir,
                args.emit_opt, args.emit_jobs)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError,
            subprocess.SubprocessError) as error:
        print(f"EE IR overwrite failed; replay not started: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
