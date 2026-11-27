#!/usr/bin/env python3
"""Recover known EE functions without running the game or replacing saved IR.

A verified historical translation manifest supplies the known guest entries.
Reconstruction or verified migration takes place in a recovery-* child
of the canonical cache. After validation the cache contents are promoted
to the root and previous data retained in verified backup-*.7z archives
(or backup-* directories if 7-Zip is unavailable). Optimized
profiles are children of the same root. Legacy siblings and active pointers
are read for migration but no new pointer is written.

IR byte mismatches are NEVER silently accepted. --allow-ir-changes explicitly
opts into rebuilding changed lowering inputs or non-identical output; the full
module set, object manifest and newly linked library are still verified.
"""
import argparse
import hashlib
import json
import os
import shutil
from contextlib import ExitStack
from pathlib import Path
import sys
import subprocess
from datetime import datetime, timezone

import run_game
import run_known_replays
import shared_ee_ir as shared_ir
import cache_backup
import recovery_metrics as metrics


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


def check_source_header(elf, work, ir_sources):
    """Read-only preflight. Full module hashes are checked before native use."""
    metadata = shared_ir.ir_directory(work) / "ee_functions.json"
    if shared_ir.active(metadata):
        state = shared_ir.current_state(metadata, ir_sources)
        count = len(state["ir_hashes"])
    else:
        snapshot, entries = run_known_replays.read_manifest(
            work / "translation-cache.json")
        if snapshot["inputs"]["ELF"] != run_game.sha256_path(elf):
            raise ValueError("source cache ELF differs")
        if snapshot.get("emit_opt") != 0:
            raise ValueError("source cache is not the base EE profile")
        if not shared_ir.matching_sources(snapshot.get("ir_sources"), ir_sources):
            raise ValueError("source cache lowering inputs differ")
        if snapshot.get("metadata_sha256") != run_game.sha256_path(metadata):
            raise ValueError("source cache function metadata differs")
        count = len(entries)
    if count == 0:
        raise ValueError("source cache has no known functions")
    return count


PROFILE_SUFFIXES = ("-O1", "-O2", "-O3", "-IR-O2", "-O2-native")
PROFILE_DIRS = ("", "O1", "O2", "O3", "IR-O2", "O2-native")


def native_profile(root, opt):
    return root / PROFILE_DIRS[opt]


def legacy_recovery_directories(base_work):
    return [path for path in sorted(
        base_work.parent.glob(base_work.name + "-recovery-*"))
        if path.is_dir() and not path.is_symlink()]


def manifest_candidates(base_work):
    candidates = []
    # Profiles share the unoptimized guest IR inventory. A native profile
    # may have discovered more entries than the last base-library snapshot.
    # Only read the known launcher sibling names, not arbitrary directories.
    roots = [active_work(base_work), base_work]
    roots.extend(root for root in sorted(base_work.glob("recovery-*"))
                 if root.is_dir())
    # Previous pointer-based recoveries may have a larger native inventory.
    # Only consider actual recovery roots, not their optimized siblings.
    for root in legacy_recovery_directories(base_work):
        if not root.name.endswith(PROFILE_SUFFIXES):
            roots.append(root)
        else:
            # An old optimized sibling may outlive its source directory.
            candidates.append(root / "translation-cache.json")
    for source in dict.fromkeys(roots):
        candidates.append(source / "translation-cache.json")
        for suffix in PROFILE_SUFFIXES:
            candidates.append(source / suffix[1:] / "translation-cache.json")
            candidates.append(source.with_name(source.name + suffix) /
                              "translation-cache.json")
    directory = history_dir(base_work)
    if directory.is_dir():
        candidates.extend(directory.glob("*.json"))
    # Duplicate file paths do not represent independent evidence.
    return list(dict.fromkeys(candidates))


@metrics.measure("manifest_selection")
def select_manifest(base_work, elf, ir_sources, allow_ir_changes=False):
    """Pick the largest authentic-ELF known-function inventory available.

    Prefer same-lowering-source snapshots at equal function counts. A source
    change requires explicit opt-in and never weakens the ELF hash guard.
    """
    with metrics.stage("elf_hashing"):
        expected_elf = run_game.sha256_path(elf)
    selected = []
    for candidate in manifest_candidates(base_work):
        if not candidate.is_file():
            continue
        try:
            manifest, entries = run_known_replays.read_manifest(candidate)
            if manifest["inputs"]["ELF"] != expected_elf:
                continue
            # Recovery selects raw guest entry inventories, never native
            # objects. Legacy backend-only O5 and selective O5 both hash
            # the same source IR; accept the known legacy identity here
            # without relaxing native object/link provenance elsewhere.
            profile = manifest.get("emit_opt")
            if profile not in range(6):
                continue
            pipeline = manifest.get("ir_pipeline")
            if (pipeline != run_game.ir_pipeline_id(profile) and
                    not (profile == 5 and pipeline is None)):
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
            same_sources = shared_ir.matching_sources(manifest.get("ir_sources"), ir_sources)
            if not same_sources and not allow_ir_changes:
                continue
            selected.append((len(entries), same_sources,
                             candidate.parent != history_dir(base_work),
                             candidate.stat().st_mtime_ns,
                             candidate, manifest, entries))
        except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError):
            continue
    if not selected:
        raise ValueError(
            "no known-function manifest matches this ELF and current guest "
            "lowering inputs; use --allow-ir-changes explicitly only when "
            "the guest lowering change was intentional"
        )
    selected.sort(key=lambda item: item[:4], reverse=True)
    _, same_sources, _, _, candidate, manifest, entries = selected[0]
    return candidate, manifest, entries, same_sources


def new_recovery_work(base_work):
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    target = base_work / ("recovery-" + stamp)
    if target.exists():
        raise ValueError("recovery staging directory already exists")
    return target


def canonical_portable(base_work, work):
    metadata_path = shared_ir.ir_directory(work) / "ee_functions.json"
    metadata = shared_ir.read_json(metadata_path)
    return (work == base_work and not pointer_path(base_work).exists()
            and not any(base_work.with_name(base_work.name + suffix).exists()
                        for suffix in PROFILE_SUFFIXES)
            and not legacy_recovery_directories(base_work)
            and metadata.get("path_format") == "manifest-relative-v1"
            and all(Path(fn["ir_path"]).name == fn["ir_path"]
                    for fn in metadata["functions"]))


def migrate_source_fingerprints(elf, base_work, source_root):
    """Migrate a verified canonical shared source store without any build."""
    with shared_ir.ir_lock(base_work):
        work = active_work(base_work)
        if not canonical_portable(base_work, work):
            raise ValueError("fingerprint migration requires canonical relative-path metadata")
        metadata_path = shared_ir.ir_directory(work) / "ee_functions.json"
        if not shared_ir.active(metadata_path):
            raise ValueError("fingerprint migration requires existing shared IR provenance")
        if metadata_path.parent.is_symlink() or metadata_path.is_symlink():
            raise ValueError("fingerprint migration requires owned IR metadata")
        sources = run_game.ir_generation_inputs(elf, source_root)
        state = shared_ir.current_state(metadata_path, sources)
        metadata = shared_ir.read_json(metadata_path)
        if shared_ir.elf_path(metadata, metadata_path.parent) != elf:
            raise ValueError("shared IR source ELF location differs")
        hashes = run_game.hash_ir_modules(metadata_path.parent, metadata["functions"],
                                         require_owned_directory=True)
        if not hashes or state["ir_hashes"] != hashes:
            raise ValueError("shared IR module hashes changed")
        if run_game.ir_generation_inputs(elf, source_root) != sources:
            raise ValueError("lowering inputs changed during fingerprint migration")
        shared_ir.current_state(metadata_path, sources, hashes)
        changed = sum(state["ir_sources"][name] != value
                      for name, value in sources.items())
        shared_ir.publish(metadata_path, sources, hashes)
        print(f"EE fingerprint migration: {len(hashes)} verified modules; "
              f"{changed} source fingerprints migrated; "
              "no IR regeneration or native compilation/linking", flush=True)
        return changed


@metrics.measure("verified_ir_copy")
def stage_verified_ir(elf, source, work, manifest, ir_sources):
    """Copy a verified map and byte-identical modules, never re-lift them."""
    metadata_path = shared_ir.ir_directory(source) / "ee_functions.json"
    metadata = shared_ir.read_json(metadata_path)
    if shared_ir.elf_path(metadata, metadata_path.parent) != elf:
        raise ValueError("legacy source ELF location differs")
    hashes = run_game.hash_ir_modules(metadata_path.parent, metadata["functions"],
                                      require_owned_directory=True)
    order = manifest.get("known_entries", list(manifest["ir_hashes"]))
    if hashes != manifest["ir_hashes"] or list(hashes) != order:
        raise ValueError("legacy IR does not match selected ordered inventory")
    if shared_ir.active(metadata_path):
        shared_ir.current_state(metadata_path, ir_sources, hashes)
    elif manifest.get("metadata_sha256") != run_game.sha256_path(metadata_path):
        raise ValueError("legacy function map is not authenticated")
    if not shared_ir.matching_sources(manifest.get("ir_sources"), ir_sources):
        raise ValueError("legacy lowering inputs changed")
    destination = work / "ir"
    destination.mkdir(parents=True, exist_ok=False)
    for function in metadata["functions"]:
        name = Path(function["ir_path"]).name
        shutil.copyfile(metadata_path.parent / name, destination / name)
        function["ir_path"] = name
    metadata["elf"] = shared_ir.relative_path(elf, destination)
    metadata["path_format"] = "manifest-relative-v1"
    shared_ir.write_json(destination / "ee_functions.json", metadata)
    copied = run_game.hash_ir_modules(destination, metadata["functions"],
                                       require_owned_directory=True)
    if copied != hashes:
        raise ValueError("copied guest IR changed")
    return {"missing": [], "extra": [], "mismatched_sha256": [],
            "expected": len(hashes), "observed": len(hashes), "matching": len(hashes)}


@metrics.measure("cache_promotion")
def promote_canonical(base_work, work, validate, *, elf=None,
                      preserved_profiles=(), opt=0):
    """Promote a child staging cache, retaining old contents and legacy profiles.

    Locks remain in place. The only path that changes with nesting depth is
    the external ELF reference; rebase that field and its authenticated
    metadata digest after validation, without changing any guest IR.
    """
    if (work.parent != base_work or not work.name.startswith("recovery-") or
            work.is_symlink() or not work.is_dir()):
        raise ValueError("invalid recovery staging directory")
    backup = base_work / ("backup-" + work.name.removeprefix("recovery-"))
    if backup.exists():
        raise ValueError(f"backup already exists: {backup}")
    backup.mkdir()
    previous = []
    for source in sorted(base_work.iterdir()):
        if (source == work or source == backup or
                source.name.startswith(("recovery-", "backup-", "retained-")) or
                source in preserved_profiles or
                source.name.endswith(".ee-ir.lock")):
            continue
        previous.append((source, backup / source.name))
    # Preserve old sibling profiles separately, even if a nested profile
    # with the same name also exists. No old native cache is overwritten.
    for suffix in PROFILE_SUFFIXES:
        source = base_work.with_name(base_work.name + suffix)
        if source.exists():
            previous.append((source, backup / ("legacy" + suffix)))
    for source in legacy_recovery_directories(base_work):
        previous.append((source, backup / ("legacy" + source.name[len(base_work.name):])))
    moved = []
    promoted = []
    original = {}
    pointer = pointer_path(base_work)
    try:
        if elf is not None:
            meta = work / "ir" / "ee_functions.json"
            cache = native_profile(work, opt) / "translation-cache.json"
            original = {meta: meta.read_bytes(), cache: cache.read_bytes()}
            metadata = shared_ir.read_json(meta)
            snapshot = shared_ir.read_json(cache)
            if (snapshot["metadata_sha256"] != run_game.sha256_path(meta) or
                    shared_ir.elf_path(metadata, meta.parent) != elf):
                raise ValueError("staging metadata changed before promotion")
            provenance = meta.parent / shared_ir.PROVENANCE
            state = None
            if shared_ir.active(meta):
                state = shared_ir.current_state(
                    meta, snapshot["ir_sources"], snapshot["ir_hashes"])
                original[provenance] = provenance.read_bytes()
            elif opt != 0:
                raise ValueError("selected native profile requires shared provenance")
            metadata["elf"] = shared_ir.relative_path(elf, base_work / "ir")
            shared_ir.write_json(meta, metadata)
            snapshot["metadata_sha256"] = run_game.sha256_path(meta)
            shared_ir.write_json(cache, snapshot)
            if state is not None:
                # A fresh staging store has never been activated. Its map
                # digest includes the ELF path, so rebuild its provenance
                # after rebasing rather than claiming an append extension.
                provenance.unlink()
                shared_ir.publish(meta, state["ir_sources"], state["ir_hashes"])
        for source, target in previous:
            if target.exists():
                raise ValueError(f"backup collision: {target}")
            source.rename(target)
            moved.append((source, target))
        for source in sorted(work.iterdir()):
            target = base_work / source.name
            if target.exists():
                raise ValueError(f"promotion collision: {target}")
            source.rename(target)
            promoted.append((source, target))
        validate(base_work)
        pointer.unlink(missing_ok=True)
    except BaseException:
        for source, target in reversed(promoted):
            target.rename(source)
        for source, target in reversed(moved):
            target.rename(source)
        for path, data in original.items():
            path.write_bytes(data)
        backup.rmdir()
        raise
    work.rmdir()
    if not moved:
        backup.rmdir()
        return None
    return backup


def promote_pointer(base_work, work):
    """Legacy pointer writer retained for compatibility tests only."""
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


@metrics.measure("cache_validation")
def validate_portable_cache(directory, elf, inputs, ir_sources, library_name, opt=0):
    """Authenticate the relocated guest IR and native objects before publication."""
    from link_native_objects import load_inputs

    metadata_path = directory / "ir" / "ee_functions.json"
    profile = native_profile(directory, opt)
    cache_path = profile / "translation-cache.json"
    if opt and shared_ir.ir_directory(profile) != metadata_path.parent:
        raise ValueError("recovered profile is not attached to staged shared IR")
    metadata = shared_ir.read_json(metadata_path)
    if (metadata.get("path_format") != "manifest-relative-v1" or
            shared_ir.elf_path(metadata, metadata_path.parent) != elf or
            any(Path(fn["ir_path"]).name != fn["ir_path"]
                for fn in metadata["functions"])):
        raise ValueError("recovered function metadata is not portable")
    snapshot = shared_ir.read_json(cache_path)
    hashes = run_game.hash_ir_modules(metadata_path.parent, metadata["functions"],
                                      require_owned_directory=True)
    if (not shared_ir.matching_sources(snapshot.get("ir_sources"), ir_sources) or
            snapshot.get("ir_hashes") != hashes):
        raise ValueError("recovered guest IR differs from verified provenance")
    run_game.validate_translation_cache(cache_path, inputs, opt,
                                        metadata_path, profile / library_name,
                                        ir_sources=ir_sources)
    _, objects, _, _ = load_inputs(metadata_path, profile / "objects")
    from verify_ir import append_cached_objects, llvm
    append_cached_objects(profile / "objects", opt, llvm.get_default_triple(), [])
    if objects.get("path_format") != "manifest-relative-v1":
        raise ValueError("recovered object manifest is not portable")
    observed = {}
    for item in objects["objects"]:
        path = Path(item["input"])
        if path.is_absolute() or (profile / "objects" / path).resolve() != (
                metadata_path.parent / path.name).resolve():
            raise ValueError("recovered object references unexpected guest IR")
        observed[path.name] = item["ir_sha256"]
    if observed != hashes:
        raise ValueError("recovered native objects do not match guest IR")


def canonical_shared_profiles(base_work, work):
    """Only retain attached native profiles that own no independent IR."""
    profiles = []
    for suffix in PROFILE_SUFFIXES:
        profile = base_work / suffix[1:]
        if (not profile.is_dir() or profile.is_symlink() or
                (profile / "ir").exists() or (work / profile.name).exists()):
            continue
        try:
            if shared_ir.ir_directory(profile) == base_work / "ir":
                profiles.append(profile)
        except (OSError, ValueError):
            continue
    return profiles


def recover(elf, build, config, base_work, jobs, allow_ir_changes=False,
            *, full_native_backup=False, opt=0):
    """Reconstruct, link only the selected profile, then validate and promote."""
    if opt not in range(6):
        raise ValueError("invalid recovery native profile")
    source_root = Path(__file__).resolve().parents[1]
    recomp = run_game.native_executable(build, "ee_recomp", config)
    ir_sources = run_game.ir_generation_inputs(elf, source_root)
    candidate, manifest, entries, same_sources = select_manifest(
        base_work, elf, ir_sources, allow_ir_changes
    )
    metrics.details("selected_manifest", str(candidate))
    metrics.details("known_modules", len(entries))
    metrics.details("lowering_sources_match", same_sources)
    previous_cache = active_work(base_work)
    work = new_recovery_work(base_work)
    print(
        f"Cache recovery: {len(entries)} known entries from {candidate}; "
        f"lowering_sources_match={same_sources}", flush=True,
    )
    library_name = ("game-translated.dll" if os.name == "nt" else
                    "libgame-translated.dylib" if sys.platform == "darwin" else
                    "libgame-translated.so")
    inputs = run_game.translation_inputs(elf, recomp, source_root)
    metrics.details("source_fingerprints", dict(ir_sources))
    metrics.details("native_input_fingerprints", dict(inputs))
    metrics.details("known_manifest_metadata_sha256", manifest.get("metadata_sha256"))

    def validate(directory):
        validate_portable_cache(directory, elf, inputs, ir_sources, library_name, opt)

    with ExitStack() as locks:
        # Lock the canonical and legacy source stores and all extant native
        # profiles before moving any directory. Shared profiles reuse a lock.
        roots = list(dict.fromkeys((base_work, previous_cache,
                                    *legacy_recovery_directories(base_work))))
        if candidate.parent != history_dir(base_work):
            roots = list(dict.fromkeys((*roots, candidate.parent)))
        for root in roots:
            locks.enter_context(shared_ir.ir_lock(root))
            for suffix in PROFILE_SUFFIXES:
                for profile in (root / suffix[1:], root.with_name(root.name + suffix)):
                    if profile.exists():
                        locks.enter_context(shared_ir.ir_lock(profile))
        comparison = None
        if same_sources and candidate.parent != history_dir(base_work):
            try:
                comparison = stage_verified_ir(
                    elf, candidate.parent, work, manifest, ir_sources)
            except (OSError, ValueError, KeyError, TypeError) as error:
                # A failed copy is never published; rebuild in a fresh sibling.
                print(f"Verified migration unavailable: {error}", flush=True)
                work = new_recovery_work(base_work)
            else:
                print("Migrating verified guest IR without retranslation", flush=True)
        hashes = {}
        if comparison is None:
            comparison = run_known_replays.reconstruct(
                manifest, entries, elf, build, config, work, jobs, allow_ir_changes,
                ir_only=True, verified_hashes=hashes)
        else:
            # stage_verified_ir authenticated the copied bytes in order.
            hashes = {name: manifest["ir_hashes"][name] for name in
                      manifest.get("known_entries", list(manifest["ir_hashes"]))}
        if comparison["missing"] or comparison["extra"] or (
                comparison["mismatched_sha256"] and not allow_ir_changes):
            raise ValueError("reconstructed IR did not meet the recovery policy")
        metadata_path = work / "ir" / "ee_functions.json"
        if list(hashes) != manifest.get("known_entries", list(manifest["ir_hashes"])):
            raise ValueError("recovered ordered IR inventory changed")
        if not allow_ir_changes and hashes != manifest["ir_hashes"]:
            raise ValueError("recovered IR hash mismatch")
        # Both copied and reconstructed IR use the same native seeding path.
        # Use current hashes rather than the historical manifest: changed
        # lowering can still leave a large fraction of modules byte-identical.
        from build_optimized_cache import seed_candidate_objects
        from verify_ir import llvm, native_target_identity, ir_pipeline_id
        roots = [previous_cache, base_work, *legacy_recovery_directories(base_work)]
        candidates = [candidate.parent]
        for root in roots:
            candidates.append(native_profile(root, opt))
            if opt:
                candidates.append(root.with_name(root.name + PROFILE_SUFFIXES[opt - 1]))
        displaced = [*base_work.glob("retained-*"), *base_work.glob("backup-*")]
        for old in sorted(displaced, reverse=True):
            if not old.is_dir() or old.is_symlink() or old.name.endswith("-ir"):
                continue
            candidates.append(native_profile(old, opt))
            if opt:
                candidates.append(old / ("legacy" + PROFILE_SUFFIXES[opt - 1]))
                candidates.extend(old.glob("legacy-recovery-*" + PROFILE_SUFFIXES[opt - 1]))
        profile = native_profile(work, opt)
        if opt:
            shared_ir.publish(metadata_path, ir_sources, hashes)
            profile.mkdir()
            shared_ir.attach(profile, metadata_path.parent)
        with metrics.stage("native_object_seeding"):
            seed_candidate_objects(
                candidates, profile, metadata_path, hashes, opt,
                llvm, native_target_identity, ir_pipeline_id)
        run_game.build_linked_library(
            source_root, profile, metadata_path, profile / "objects",
            profile / library_name, opt, emit_jobs=jobs)
        run_game.write_translation_cache(
            profile / "translation-cache.json", inputs, opt, metadata_path,
            profile / library_name, ir_sources)
        metrics.details("ir_comparison", comparison)
        (work / "ir-comparison.json").write_text(
            json.dumps(comparison, indent=2) + "\n", encoding="utf-8")
        validate(work)
        preserved_profiles = ([] if full_native_backup else
                              canonical_shared_profiles(base_work, work))
        def validate_promoted(directory):
            validate(directory)
            # Retained shared profiles must still resolve their marker after
            # promotion. Publish only the newly authenticated guest map.
            if preserved_profiles and not opt:
                metadata = directory / "ir" / "ee_functions.json"
                receipt = shared_ir.read_json(directory / "translation-cache.json")
                shared_ir.publish(metadata, ir_sources, receipt["ir_hashes"])

        backup = promote_canonical(base_work, work, validate_promoted, elf=elf,
                                   preserved_profiles=preserved_profiles, opt=opt)
        if backup is not None:
            if not full_native_backup:
                try:
                    backup = cache_backup.partition_backup(backup, preserved_profiles)
                except (OSError, ValueError) as error:
                    # Promotion already succeeded. Retain all old files
                    # uncompressed rather than silently archiving native data.
                    print(f"WARNING: IR-only backup partition failed ({error}); "
                          f"uncompressed backup retained: {backup}", flush=True)
                else:
                    backup = cache_backup.compress_backup(backup)
            else:
                backup = cache_backup.compress_backup(backup, level=9)
        if preserved_profiles:
            print("Preserved shared native profiles in place: " +
                  ", ".join(profile.name for profile in preserved_profiles), flush=True)

    print(f"Cache recovery validated and activated: {base_work}", flush=True)
    if backup is not None:
        print(f"Previous cache recovery artifacts retained under: {backup}", flush=True)
    if previous_cache != base_work and backup is not None:
        print("Previous legacy active cache artifacts retained in backup/retention directories",
              flush=True)
    return base_work


def benchmark_reconstruction(elf, build, config, base_work, output, jobs,
                             allow_ir_changes=False, *, persistent=False,
                             allow_source_changes=False):
    """Cold IR-only reconstruction in a fresh directory outside active cache."""
    base_work = base_work.resolve()
    output = output.resolve()
    if (output.exists() or output.is_relative_to(base_work) or
            base_work.is_relative_to(output) or
            output.parent != base_work.parent or
            output.name.startswith((base_work.name + "-", "recovery-", "backup-", "retained-"))):
        raise ValueError("benchmark requires a fresh separate sibling work directory")
    source_root = Path(__file__).resolve().parents[1]
    with shared_ir.ir_lock(base_work):
        sources = run_game.ir_generation_inputs(elf, source_root)
        candidate, manifest, entries, same_sources = select_manifest(
            base_work, elf, sources, allow_ir_changes or allow_source_changes)
        metrics.details("selected_manifest", str(candidate))
        metrics.details("known_modules", len(entries))
        metrics.details("lowering_sources_match", same_sources)
        metrics.details("source_fingerprints", dict(sources))
        metrics.details("known_manifest_metadata_sha256", manifest.get("metadata_sha256"))
        # Retain the exact ordered input inventory for controlled repetitions.
        output.mkdir(parents=True, exist_ok=False)
        shared_ir.write_json(output / "benchmark-input.json", manifest)
    hashes = {}
    extra = {"persistent": True} if persistent else {}
    comparison = run_known_replays.reconstruct(
        manifest, entries, elf, build, config, output, jobs,
        allow_ir_changes, ir_only=True, verified_hashes=hashes, **extra)
    metadata_path = output / "ir" / "ee_functions.json"
    if (list(hashes) != manifest.get("known_entries", list(manifest["ir_hashes"])) or
            comparison["missing"] or comparison["extra"] or
            (not allow_ir_changes and hashes != manifest["ir_hashes"])):
        raise ValueError("benchmark reconstructed inventory differs")
    shared_ir.write_json(output / "benchmark-output.json", {
        "ir_hashes": hashes, "comparison": comparison,
        "metadata_sha256": run_game.sha256_path(metadata_path)})
    metrics.details("ir_comparison", comparison)
    print(f"Cold IR-only reconstruction complete: {len(hashes)} modules; "
          "no native emission, linking, promotion or guest execution", flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--benchmark-reconstruct", type=Path, metavar="FRESH_WORK_DIR",
                        help="cold IR-only reconstruction; never activate output")
    parser.add_argument("--persistent-recomp", action="store_true",
                        help="benchmark one persistent recompiler process")
    parser.add_argument("--benchmark-allow-source-changes", action="store_true",
                        help="benchmark only: permit changed generator fingerprints, not changed IR hashes")
    parser.add_argument("--print-active", action="store_true",
                        help="print only the selected active work directory")
    parser.add_argument("--check-active-source", action="store_true",
                        help="read-only current lowering and metadata preflight")
    parser.add_argument("--migrate-source-fingerprints", action="store_true",
                        help="authenticate canonical IR and migrate equivalent source fingerprints only")
    parser.add_argument("--require-portable", action="store_true",
                        help="also require canonical relative-path metadata")
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--config", choices=("Debug", "Release", "RelWithDebInfo",
                                            "MinSizeRel"), default="Release")
    parser.add_argument("--base-work-dir", type=Path,
                        default=Path("build/ps2-game-hle"))
    parser.add_argument("--emit-jobs", type=int, default=8)
    parser.add_argument("--emit-opt", type=int, choices=range(6), default=0,
                        help="recover only this native profile (default O0)")
    parser.add_argument("--timings-report", type=Path,
                        help="new JSON report path; default is beside the cache")
    parser.add_argument("--full-native-backup", action="store_true",
                        help="also archive displaced native objects, libraries and reports")
    parser.add_argument("--allow-ir-changes", action="store_true",
                        help="explicitly allow changed lowering/IR hashes")
    args = parser.parse_args(argv)
    if (args.persistent_recomp or args.benchmark_allow_source_changes) and not args.benchmark_reconstruct:
        parser.error("persistent recompiler/source-change experiment requires --benchmark-reconstruct")
    if args.benchmark_reconstruct and (
            args.print_active or args.check_active_source or
            args.migrate_source_fingerprints or args.full_native_backup):
        parser.error("--benchmark-reconstruct cannot be combined with other modes")
    if args.migrate_source_fingerprints and (
            args.print_active or args.check_active_source or args.allow_ir_changes):
        parser.error("--migrate-source-fingerprints cannot be combined with other modes or --allow-ir-changes")
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
        if args.benchmark_reconstruct:
            report = args.timings_report or metrics.default_report(base_work, "ir-benchmark")
            with metrics.report("ir-benchmark", report, cache=str(base_work),
                                elf=str(elf), jobs=args.emit_jobs,
                                work=str(args.benchmark_reconstruct.resolve()),
                                persistent_recomp=args.persistent_recomp,
                                allow_source_changes=args.benchmark_allow_source_changes,
                                allow_ir_changes=args.allow_ir_changes):
                benchmark_reconstruction(
                    elf, build, args.config, base_work, args.benchmark_reconstruct,
                    args.emit_jobs, args.allow_ir_changes,
                    persistent=args.persistent_recomp,
                    allow_source_changes=args.benchmark_allow_source_changes)
        elif args.migrate_source_fingerprints:
            migrate_source_fingerprints(elf, base_work, Path(__file__).resolve().parents[1])
        elif args.check_active_source:
            work = active_work(base_work)
            print(f"Active EE source: {work}", flush=True)
            source_root = Path(__file__).resolve().parents[1]
            sources = run_game.ir_generation_inputs(elf, source_root)
            count = check_source_header(elf, work, sources)
            if args.require_portable and not canonical_portable(base_work, work):
                raise ValueError("cache needs canonical relative-path migration")
            print(f"EE source preflight: {count} known functions; "
                  "current lowering and metadata match", flush=True)
        else:
            report = args.timings_report or metrics.default_report(base_work, "recovery")
            with metrics.report("recovery", report, cache=str(base_work),
                                elf=str(elf), jobs=args.emit_jobs, profile=args.emit_opt,
                                allow_ir_changes=args.allow_ir_changes,
                                full_native_backup=args.full_native_backup):
                recover(elf, build, args.config, base_work, args.emit_jobs,
                        args.allow_ir_changes, full_native_backup=args.full_native_backup,
                        opt=args.emit_opt)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        prefix = ("EE fingerprint migration refused" if args.migrate_source_fingerprints
                  else "EE source preflight refused" if args.check_active_source
                  else "Cache recovery stopped without changing the active cache")
        print(f"{prefix}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
