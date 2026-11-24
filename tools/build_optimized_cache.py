#!/usr/bin/env python3
"""Emit an independent optimized EE library from verified existing IR."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

import run_game
import shared_ee_ir as shared_ir


def function_map(metadata):
    return [{key: value for key, value in fn.items() if key != "ir_path"}
            for fn in metadata["functions"]]


def verify_modules(work, metadata, hashes):
    expected = run_game.hash_ir_modules(
        shared_ir.ir_directory(work), metadata["functions"],
        require_owned_directory=True)
    if expected != hashes:
        raise ValueError("verified IR hashes do not match the function map")


def refresh_profile(elf, build, config, work, opt, jobs, inputs, library,
                    *, verified_ir_hashes=None):
    metadata = shared_ir.ir_directory(work) / "ee_functions.json"
    cache = work / "translation-cache.json"
    try:
        run_game.validate_translation_cache(cache, inputs, opt, metadata, library,
                                            verified_ir_hashes=verified_ir_hashes)
    except ValueError:
        result = run_game.main([
            str(elf), "--build-dir", str(build), "--config", config,
            "--work-dir", str(work), f"--emit-opt={opt}", f"--emit-jobs={jobs}",
            "--relink-existing", "--link-only"])
        if result != 0:
            raise ValueError("optimized cache relink failed")
        run_game.validate_translation_cache(cache, inputs, opt, metadata, library)


def migrate_legacy(work, metadata_path, ir_sources, inputs, opt):
    """Adopt only a verified prefix/suffix, then rekey existing objects."""
    old_path = work / "ir" / "ee_functions.json"
    old_cache = shared_ir.read_json(work / "translation-cache.json")
    old = shared_ir.read_json(old_path)
    hashes = old_cache["ir_hashes"]
    if (old_cache.get("schema") != run_game.CACHE_SCHEMA or
            old_cache.get("ir_sources") != ir_sources or
            old_cache.get("metadata_sha256") != run_game.sha256_path(old_path)):
        raise ValueError("legacy EE IR provenance cannot be verified")
    verify_modules(work, old, hashes)
    pending, _ = run_game.validate_preserved_translation(
        work / "translation-cache.json", inputs, ir_sources, opt,
        old_path, work / "objects")
    if pending:
        raise ValueError("legacy profile has an unfinished discovery transaction")
    current = shared_ir.read_json(metadata_path)
    state = shared_ir.current_state(metadata_path, ir_sources)
    current_hashes = state["ir_hashes"]
    old_count, count = len(old["functions"]), len(current["functions"])
    common = min(old_count, count)
    if (shared_ir.map_digest(old, common) != shared_ir.map_digest(current, common) or
            shared_ir.prefix_hashes(old, hashes, common) !=
            shared_ir.prefix_hashes(current, current_hashes, common)):
        raise ValueError("legacy profile has divergent function ownership/IR; preserved unchanged")

    if old_count > count:
        # Copy only unique, verified discoveries, never the common IR set.
        original_bytes = metadata_path.read_bytes()
        committed = []
        staged = []
        try:
            for entry in old["functions"][count:]:
                source = Path(entry["ir_path"])
                destination = metadata_path.parent / source.name
                if destination.exists():
                    raise ValueError(f"shared IR filename collision: {destination}")
                temporary = destination.with_suffix(".ll.migrate")
                staged.append(temporary)
                shutil.copyfile(source, temporary)
                temporary.replace(destination)
                committed.append(destination)
                current["functions"].append(dict(entry, ir_path=str(destination)))
            current["function_count"] = old_count
            shared_ir.write_json(metadata_path, current)
            shared_ir.publish(metadata_path, ir_sources, hashes)
        except BaseException:
            temporary = metadata_path.with_suffix(".rollback")
            temporary.write_bytes(original_bytes)
            temporary.replace(metadata_path)
            for path in committed + staged:
                path.unlink(missing_ok=True)
            raise
        current_hashes = hashes
        print(f"Shared EE IR: adopted {old_count - count} verified legacy discoveries", flush=True)

    # A profile cache with legacy path spellings can be synchronized even
    # if interrupted after attaching the marker but before relinking.
    shared_ir.publish(metadata_path, ir_sources, current_hashes,
                      alias=(old_cache["metadata_sha256"], old_count,
                             shared_ir.map_digest(old)))
    manifest_path = work / "objects" / "objects.json"
    manifest = shared_ir.read_json(manifest_path)
    for item in manifest["objects"]:
        name = Path(item["input"]).name
        if item["ir_sha256"] != current_hashes.get(name):
            raise ValueError("legacy object does not match shared IR")
        item["input"] = str(metadata_path.parent / name)
    shared_ir.write_json(manifest_path, manifest)
    shared_ir.attach(work, metadata_path.parent)


def remove_verified_duplicates(work, metadata_path, hashes):
    legacy = work / "ir"
    if (legacy.is_symlink() or legacy.resolve() == metadata_path.parent.resolve()
            or not legacy.is_dir()):
        return
    old_path = legacy / "ee_functions.json"
    if not old_path.is_file():
        return
    old = shared_ir.read_json(old_path)
    current = shared_ir.read_json(metadata_path)
    count = len(old["functions"])
    if (count > len(current["functions"]) or
            shared_ir.map_digest(old) != shared_ir.map_digest(current, count)):
        raise ValueError("legacy map differs; keeping its files")
    paths = list(legacy.glob("*.ll"))
    if any(hashes.get(path.name) != run_game.sha256_path(path) for path in paths):
        raise ValueError("legacy IR differs; keeping its files")
    for path in paths:
        path.unlink()
    old_path.unlink()
    if not any(legacy.iterdir()):
        legacy.rmdir()
    print(f"Shared EE IR: removed {len(paths)} verified duplicate modules", flush=True)


def prepare(elf, build, config, source_work, work, opt, jobs, source_opt=0):
    elf = elf.resolve(strict=True)
    source_work = source_work.resolve(strict=True)
    work = work.resolve()
    if (work == source_work or source_work in work.parents or
            work in source_work.parents or
            work == shared_ir.ir_directory(source_work).parent):
        raise ValueError("optimized cache must be separate from the source")
    with shared_ir.ir_lock(source_work), shared_ir.ir_lock(work):
        prepare_locked(elf, build, config, source_work, work, opt, jobs, source_opt)


def validate_source_ir(elf, source_work, source_opt, ir_sources):
    """Authenticate source modules independently of any native DLL/objects.

    A shared provenance record is newer than a possibly stale O0/O2
    snapshot after another profile discovers functions. For a legacy
    source, require its exact saved metadata, lowering inputs and IR.
    Never repair, relink or execute a source native library here.
    """
    metadata_path = shared_ir.ir_directory(source_work) / "ee_functions.json"
    metadata = shared_ir.read_json(metadata_path)
    if (Path(metadata.get("elf", "")).resolve() != elf or
            run_game.elf_entry(elf) not in run_game.covered_pcs(metadata)):
        raise ValueError("shared IR source ELF/entry mismatch")
    if shared_ir.active(metadata_path):
        state = shared_ir.current_state(metadata_path, ir_sources)
        hashes = state["ir_hashes"]
    else:
        snapshot = shared_ir.read_json(source_work / "translation-cache.json")
        if (snapshot.get("schema") != run_game.CACHE_SCHEMA or
                snapshot.get("emit_opt") != source_opt or
                snapshot.get("ir_sources") != ir_sources or
                snapshot.get("metadata_sha256") != run_game.sha256_path(metadata_path)):
            raise ValueError("source IR provenance is stale; recover the source cache explicitly")
        hashes = snapshot["ir_hashes"]
    started = time.monotonic()
    verify_modules(source_work, metadata, hashes)
    shared_ir.publish(metadata_path, ir_sources, hashes)
    print(f"Shared EE IR: verified {len(hashes)} modules (no source DLL relink) "
          f"in {time.monotonic() - started:.3f}s", flush=True)
    return metadata_path, hashes


def prepare_locked(elf, build, config, source_work, work, opt, jobs, source_opt):
    root = Path(__file__).resolve().parents[1]
    recomp = run_game.native_executable(build.resolve(), "ee_recomp", config)
    inputs = run_game.translation_inputs(elf, recomp, root)
    ir_sources = run_game.ir_generation_inputs(elf, root)
    library_name = ("game-translated.dll" if os.name == "nt" else
                    "libgame-translated.dylib" if sys.platform == "darwin" else
                    "libgame-translated.so")
    metadata_path, hashes = validate_source_ir(
        elf, source_work, source_opt, ir_sources)

    cache = work / "translation-cache.json"
    library = work / library_name
    if work.exists():
        fresh_hashes = hashes
        if shared_ir.ir_directory(work) == work / "ir":
            migrate_legacy(work, metadata_path, ir_sources, inputs, opt)
            # Migration can append a verified legacy suffix. Do not use
            # the original map's hashes for the now-extended source.
            fresh_hashes = None
        elif shared_ir.ir_directory(work) != metadata_path.parent:
            raise ValueError("optimized profile references another shared IR store")
        refresh_profile(elf, build, config, work, opt, jobs, inputs, library,
                        verified_ir_hashes=fresh_hashes)
        snapshot = shared_ir.read_json(cache)
        remove_verified_duplicates(work, metadata_path, snapshot["ir_hashes"])
        print(f"Reusing verified O{opt} EE cache: {work}", flush=True)
        return

    # A new profile only owns native objects and diagnostics, not guest IR.
    work.mkdir(parents=True, exist_ok=False)
    try:
        shared_ir.attach(work, metadata_path.parent)
        run_game.build_linked_library(
            root, work, metadata_path, work / "objects", library,
            opt, emit_jobs=jobs)
        run_game.write_translation_cache(
            cache, inputs, opt, metadata_path, library, ir_sources)
        run_game.validate_translation_cache(
            cache, inputs, opt, metadata_path, library)
        if run_game.translated_ir_hashes(metadata_path) != hashes:
            raise ValueError("optimized build changed the original IR")
    except BaseException:
        shutil.rmtree(work)
        raise
    print(f"Built O{opt} EE variant from {len(hashes)} shared IR modules: {work}",
          flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--config", default="Release")
    parser.add_argument("--source-work-dir", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--emit-opt", type=int, choices=(1, 2, 3, 4, 5), required=True)
    parser.add_argument("--source-opt", type=int, choices=(0, 2), default=0,
                        help="legacy source provenance profile (shared IR is independent of native profile)")
    parser.add_argument("--emit-jobs", type=int, default=2)
    args = parser.parse_args(argv)
    if args.source_opt == 2 and args.emit_opt not in (4, 5):
        parser.error("O2 source is only supported for independent IR/native profiles")
    if not 1 <= args.emit_jobs <= 32:
        parser.error("--emit-jobs must be between 1 and 32")
    try:
        prepare(args.elf, args.build_dir, args.config, args.source_work_dir,
                args.work_dir, args.emit_opt, args.emit_jobs, args.source_opt)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError,
            subprocess.CalledProcessError) as error:
        print(f"Optimized cache refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
