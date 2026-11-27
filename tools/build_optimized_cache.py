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
import cache_backup


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
                    *, ir_sources, verified_ir_hashes=None):
    metadata = shared_ir.ir_directory(work) / "ee_functions.json"
    cache = work / "translation-cache.json"
    try:
        run_game.validate_translation_cache(cache, inputs, opt, metadata, library,
                                            verified_ir_hashes=verified_ir_hashes,
                                            ir_sources=ir_sources)
    except ValueError:
        # The canonical shared source may have been refreshed independently
        # while this native profile still records the old lowering inputs.
        # It is safe to build a NEW native library from the authenticated
        # current IR, not to relabel the old library or accept stale IR.
        snapshot = shared_ir.read_json(cache)
        # Native pipeline upgrades must re-emit objects, not relift the
        # authenticated shared IR or relabel old object manifests.
        pipeline_changed = (snapshot.get("schema") == run_game.CACHE_SCHEMA and
                            snapshot.get("emit_opt") == opt and
                            snapshot.get("ir_pipeline") != run_game.ir_pipeline_id(opt))
        if (pipeline_changed and shared_ir.active(metadata) and
                shared_ir.matching_sources(snapshot.get("ir_sources"), ir_sources)):
            hashes = (verified_ir_hashes if verified_ir_hashes is not None
                      else run_game.translated_ir_hashes(metadata))
            shared_ir.current_state(metadata, ir_sources, hashes)
            print("Optimized native pipeline changed; re-emitting objects "
                  "from verified shared IR", flush=True)
            root = Path(__file__).resolve().parents[1]
            run_game.build_linked_library(
                root, work, metadata, work / "objects", library,
                opt, emit_jobs=jobs)
            run_game.write_translation_cache(
                cache, inputs, opt, metadata, library, ir_sources)
            run_game.validate_translation_cache(cache, inputs, opt, metadata, library,
                                                ir_sources=ir_sources)
            return
        if (verified_ir_hashes is not None and
                shared_ir.active(metadata) and
                snapshot.get("schema") == run_game.CACHE_SCHEMA and
                snapshot.get("emit_opt") == opt and
                isinstance(snapshot.get("ir_sources"), dict) and
                snapshot["ir_sources"].get("ELF") == ir_sources.get("ELF") and
                (not shared_ir.matching_sources(snapshot["ir_sources"], ir_sources) or
                 snapshot.get("ir_hashes") != verified_ir_hashes or
                 snapshot.get("metadata_sha256") != run_game.sha256_path(metadata)) and
                not shared_ir.verified_extension(snapshot, metadata, ir_sources,
                                                 verified_ir_hashes)):
            shared_ir.current_state(metadata, ir_sources, verified_ir_hashes)
            print("Optimized profile source provenance is stale; "
                  "revalidating native objects against current shared IR",
                  flush=True)
            # The ordinary incremental emitter authenticates every candidate
            # object against its IR hash, object hash, LLVM and target identity.
            # Never use the direct relink path for this recovery.
            root = Path(__file__).resolve().parents[1]
            run_game.build_linked_library(
                root, work, metadata, work / "objects", library,
                opt, emit_jobs=jobs)
            run_game.write_translation_cache(
                cache, inputs, opt, metadata, library, ir_sources)
            run_game.validate_translation_cache(cache, inputs, opt, metadata, library,
                                                ir_sources=ir_sources)
            return
        result = run_game.main([
            str(elf), "--build-dir", str(build), "--config", config,
            "--work-dir", str(work), f"--emit-opt={opt}", f"--emit-jobs={jobs}",
            "--relink-existing", "--link-only"])
        if result != 0:
            raise ValueError("optimized cache relink failed")
        run_game.validate_translation_cache(cache, inputs, opt, metadata, library,
                                                ir_sources=ir_sources)


def migrate_legacy(work, metadata_path, ir_sources, inputs, opt):
    """Adopt only a verified prefix/suffix, then rekey existing objects."""
    old_path = work / "ir" / "ee_functions.json"
    old_cache = shared_ir.read_json(work / "translation-cache.json")
    old = shared_ir.read_json(old_path)
    hashes = old_cache["ir_hashes"]
    if (old_cache.get("schema") != run_game.CACHE_SCHEMA or
            not shared_ir.matching_sources(old_cache.get("ir_sources"), ir_sources) or
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
                source = shared_ir.module_path(entry["ir_path"], old_path.parent)
                destination = metadata_path.parent / source.name
                if destination.exists():
                    raise ValueError(f"shared IR filename collision: {destination}")
                temporary = destination.with_suffix(".ll.migrate")
                staged.append(temporary)
                shutil.copyfile(source, temporary)
                temporary.replace(destination)
                committed.append(destination)
                current["functions"].append(dict(entry, ir_path=destination.name))
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
        item["input"] = shared_ir.relative_path(metadata_path.parent / name, manifest_path.parent)
    manifest["path_format"] = "manifest-relative-v1"
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


def prepare(elf, build, config, source_work, work, opt, jobs, source_opt=0,
            *, defer_warm_validation=False):
    elf = elf.resolve(strict=True)
    source_work = source_work.resolve(strict=True)
    work = work.resolve()
    nested_profile = (work.parent == source_work and
                      work.name in ("O1", "O2", "O3", "IR-O2", "O2-native"))
    if (work == source_work or
            (source_work in work.parents and not nested_profile) or
            work in source_work.parents or
            work == shared_ir.ir_directory(source_work).parent):
        raise ValueError("optimized cache must be separate from the source")
    with shared_ir.ir_lock(source_work), shared_ir.ir_lock(work):
        prepare_locked(elf, build, config, source_work, work, opt, jobs, source_opt,
                       defer_warm_validation=defer_warm_validation)


def validate_source_ir(elf, source_work, source_opt, ir_sources):
    """Authenticate source modules independently of any native DLL/objects.

    A shared provenance record is newer than a possibly stale O0/O2
    snapshot after another profile discovers functions. For a legacy
    source, require its exact saved metadata, lowering inputs and IR.
    Never repair, relink or execute a source native library here.
    """
    metadata_path = shared_ir.ir_directory(source_work) / "ee_functions.json"
    metadata = shared_ir.read_json(metadata_path)
    if (shared_ir.elf_path(metadata, metadata_path.parent) != elf or
            run_game.elf_entry(elf) not in run_game.covered_pcs(metadata)):
        raise ValueError("shared IR source ELF/entry mismatch")
    if shared_ir.active(metadata_path):
        state = shared_ir.current_state(metadata_path, ir_sources)
        hashes = state["ir_hashes"]
    else:
        snapshot = shared_ir.read_json(source_work / "translation-cache.json")
        if (snapshot.get("schema") != run_game.CACHE_SCHEMA or
                snapshot.get("emit_opt") != source_opt or
                not shared_ir.matching_sources(snapshot.get("ir_sources"), ir_sources) or
                snapshot.get("metadata_sha256") != run_game.sha256_path(metadata_path)):
            raise ValueError("source IR provenance is stale; recover the source cache explicitly")
        hashes = snapshot["ir_hashes"]
    started = time.monotonic()
    verify_modules(source_work, metadata, hashes)
    shared_ir.publish(metadata_path, ir_sources, hashes)
    print(f"Shared EE IR: verified {len(hashes)} modules (no source DLL relink) "
          f"in {time.monotonic() - started:.3f}s", flush=True)
    return metadata_path, hashes


def warm_profile_ready(source_work, work, inputs, ir_sources, opt, library):
    """Preflight only. The actual runner must perform full IR validation."""
    try:
        directory = shared_ir.ir_directory(source_work)
        if (not (work / shared_ir.MARKER).is_file() or
                shared_ir.ir_directory(work) != directory or
                not shared_ir.active(directory / "ee_functions.json") or
                (work / "ir" / "ee_functions.json").exists()):
            return False
        metadata = directory / "ee_functions.json"
        snapshot = run_game.validate_cached_library_identity(
            work / "translation-cache.json", inputs, opt, metadata, library)
        state = shared_ir.current_state(metadata, ir_sources)
        return (shared_ir.matching_sources(snapshot.get("ir_sources"), ir_sources) and
                snapshot.get("ir_hashes") == state["ir_hashes"] and
                (work / "objects" / "objects.json").is_file())
    except (OSError, ValueError, KeyError, TypeError):
        return False


def seed_backup_objects(source_work, work, metadata_path, hashes, opt):
    """Copy only candidate matching objects; the normal emitter rehashes them."""
    if work.parent != source_work:
        return 0
    from verify_ir import llvm, native_target_identity, ir_pipeline_id

    with cache_backup.backup_object_candidates(source_work, work.name) as candidates:
        return seed_candidate_objects(candidates, work, metadata_path, hashes, opt,
                                      llvm, native_target_identity, ir_pipeline_id)


def seed_candidate_objects(candidates, work, metadata_path, hashes, opt,
                           llvm, native_target_identity, ir_pipeline_id):
    best = None
    for source in candidates:
        directory = source / "objects"
        try:
            manifest = shared_ir.read_json(directory / "objects.json")
            if (manifest.get("complete") is not True or
                    manifest.get("optimization_level") != opt or
                    manifest.get("ir_pipeline") != ir_pipeline_id(opt) or
                    manifest.get("native_target") != native_target_identity(opt) or
                    manifest.get("llvm_version") != ".".join(map(str, llvm.llvm_version_info)) or
                    manifest.get("target_triple") != llvm.get_default_triple()):
                continue
            matches = {}
            for item in manifest["objects"]:
                name = Path(item["input"]).name
                if (name not in hashes or item.get("ir_sha256") != hashes[name] or
                        item.get("object") != Path(name).stem + ".o" or
                        not isinstance(item.get("size_bytes"), int) or
                        item["size_bytes"] < 1 or
                        not (directory / item["object"]).is_file()):
                    continue
                if name in matches:
                    raise ValueError("duplicate backup object")
                matches[name] = item
            if matches and (best is None or len(matches) > len(best[2])):
                best = (directory, manifest, matches)
        except (OSError, ValueError, KeyError, TypeError):
            continue
    if best is None:
        return 0
    directory, manifest, matches = best
    destination = work / "objects"
    destination.mkdir()
    copied = []
    for name, item in matches.items():
        shutil.copy2(directory / item["object"], destination / item["object"])
        copied.append(dict(item, input=shared_ir.relative_path(
            metadata_path.parent / name, destination)))
    shared_ir.write_json(destination / "objects.json", dict(
        manifest, path_format="manifest-relative-v1",
        input_module_count=len(copied), object_count=len(copied),
        object_bytes=sum(item["size_bytes"] for item in copied), objects=copied))
    print(f"Seeded {len(copied)} candidate O{opt} objects from backup; "
          "incremental emitter will verify every reused object", flush=True)
    return len(copied)


def prepare_locked(elf, build, config, source_work, work, opt, jobs, source_opt,
                   *, defer_warm_validation=False):
    root = Path(__file__).resolve().parents[1]
    recomp = run_game.native_executable(build.resolve(), "ee_recomp", config)
    inputs = run_game.translation_inputs(elf, recomp, root)
    ir_sources = run_game.ir_generation_inputs(elf, root)
    library_name = ("game-translated.dll" if os.name == "nt" else
                    "libgame-translated.dylib" if sys.platform == "darwin" else
                    "libgame-translated.so")
    cache = work / "translation-cache.json"
    library = work / library_name
    if defer_warm_validation and warm_profile_ready(
            source_work, work, inputs, ir_sources, opt, library):
        print(f"Warm O{opt} profile: native identity matches; "
              "full IR verification deferred to runner", flush=True)
        return
    metadata_path, hashes = validate_source_ir(
        elf, source_work, source_opt, ir_sources)
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
                        ir_sources=ir_sources, verified_ir_hashes=fresh_hashes)
        snapshot = shared_ir.read_json(cache)
        remove_verified_duplicates(work, metadata_path, snapshot["ir_hashes"])
        print(f"Reusing verified O{opt} EE cache: {work}", flush=True)
        return

    # A new profile only owns native objects and diagnostics, not guest IR.
    work.mkdir(parents=True, exist_ok=False)
    try:
        shared_ir.attach(work, metadata_path.parent)
        seed_backup_objects(source_work, work, metadata_path, hashes, opt)
        run_game.build_linked_library(
            root, work, metadata_path, work / "objects", library,
            opt, emit_jobs=jobs)
        run_game.write_translation_cache(
            cache, inputs, opt, metadata_path, library, ir_sources)
        run_game.validate_translation_cache(
            cache, inputs, opt, metadata_path, library, ir_sources=ir_sources)
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
    parser.add_argument("--defer-warm-validation", action="store_true",
                        help="Preflight an existing shared profile only; "
                             "requires full run_game validation before execution")
    args = parser.parse_args(argv)
    if args.source_opt == 2 and args.emit_opt not in (4, 5):
        parser.error("O2 source is only supported for independent IR/native profiles")
    if not 1 <= args.emit_jobs <= 32:
        parser.error("--emit-jobs must be between 1 and 32")
    try:
        prepare(args.elf, args.build_dir, args.config, args.source_work_dir,
                args.work_dir, args.emit_opt, args.emit_jobs, args.source_opt,
                defer_warm_validation=args.defer_warm_validation)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError,
            subprocess.CalledProcessError) as error:
        print(f"Optimized cache refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
