#!/usr/bin/env python3
"""Synthetic safety tests for no-replay SDBZ cache recovery."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import recover_translation_cache as recovery
import run_game
import shared_ee_ir as shared_ir


def digest(data):
    return hashlib.sha256(data).hexdigest()


def manifest_for(elf, entries, ir_sources):
    return {
        "schema": run_game.CACHE_SCHEMA,
        "inputs": {"ELF": run_game.sha256_path(elf)},
        "emit_opt": 0,
        "ir_sources": ir_sources,
        "ir_hashes": {
            f"sub_0x{entry:x}_0x{entry:x}.ll": digest(str(entry).encode())
            for entry in entries
        },
    }


def test_snapshots_are_content_addressed_and_reusable():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        elf = root / "SLUS_214.42"
        elf.write_bytes(b"fixture-elf")
        base = root / "build" / "ps2-game-hle"
        base.mkdir(parents=True)
        source = {"ELF": run_game.sha256_path(elf), "src/lift/test.cpp": "1" * 64}
        early = manifest_for(elf, [0x100008, 0x100010], source)
        # Preserve original discovery order even if sorted JSON keys differ.
        later = manifest_for(elf, [0x100020, 0x100008, 0x100010], source)
        cache = base / "translation-cache.json"
        run_game.archive_known_function_manifest(cache, early)
        run_game.archive_known_function_manifest(cache, later)
        run_game.archive_known_function_manifest(cache, later)
        assert len(list(recovery.history_dir(base).glob("*.json"))) == 2
        chosen, selected, entries, same = recovery.select_manifest(
            base, elf, source
        )
        assert chosen.exists() and entries == [0x100020, 0x100008, 0x100010]
        assert selected["ir_hashes"] == later["ir_hashes"] and same

        # Reject older sorted-key snapshots: they lost original entry order.
        legacy = manifest_for(
            elf, [0x100040, 0x100020, 0x100008, 0x100010], source
        )
        legacy_bytes = (json.dumps(legacy, sort_keys=True, indent=2) +
                        "\n").encode("utf-8")
        legacy_digest = hashlib.sha256(legacy_bytes).hexdigest()[:16]
        legacy_path = recovery.history_dir(base) / (
            f"ee-{run_game.sha256_path(elf)[:12]}-000004-{legacy_digest}.json"
        )
        legacy_path.write_bytes(legacy_bytes)
        _, _, retained, _ = recovery.select_manifest(base, elf, source)
        assert retained == [0x100020, 0x100008, 0x100010]

        # Altering a content-addressed historical snapshot invalidates it.
        chosen.write_text(chosen.read_text() + " ", encoding="utf-8")
        _, _, entries, same = recovery.select_manifest(base, elf, source)
        assert entries == [0x100008, 0x100010] and same

        # A different ELF cannot consume the saved entry set.
        elf.write_bytes(b"changed-elf")
        try:
            recovery.select_manifest(base, elf, source, True)
        except ValueError as exc:
            assert "no known-function manifest" in str(exc)
        else:
            raise AssertionError("accepted a manifest for a changed game ELF")


def test_changed_lowering_requires_explicit_opt_in():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        elf = root / "SLUS_214.42"
        elf.write_bytes(b"fixture-elf")
        base = root / "build" / "ps2-game-hle"
        base.mkdir(parents=True)
        old = {"ELF": run_game.sha256_path(elf), "src/llvm/module_builder.cpp": "a" * 64}
        new = {**old, "src/llvm/module_builder.cpp": "b" * 64}
        run_game.archive_known_function_manifest(
            base / "translation-cache.json",
            manifest_for(elf, [0x100008], old),
        )
        try:
            recovery.select_manifest(base, elf, new)
        except ValueError as exc:
            assert "--allow-ir-changes" in str(exc)
        else:
            raise AssertionError("silently accepted changed lowering inputs")
        _, _, entries, same = recovery.select_manifest(base, elf, new, True)
        assert entries == [0x100008] and not same


def test_pointer_rejects_unsafe_paths_and_rollback():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        base = root / "build" / "ps2-game-hle"
        base.mkdir(parents=True)
        sibling = root / "build" / "ps2-game-hle-recovery-123"
        sibling.mkdir()
        unrelated = root / "unrelated"
        unrelated.mkdir()
        pointer = recovery.pointer_path(base)

        pointer.write_text(str(unrelated) + "\n", encoding="utf-8")
        assert recovery.active_work(base) == base
        pointer.write_text(str(base) + "\n", encoding="utf-8")
        assert recovery.active_work(base) == base
        recovery.promote_pointer(base, sibling)
        assert recovery.active_work(base) == sibling
        try:
            recovery.promote_pointer(base, unrelated)
        except ValueError:
            pass
        else:
            raise AssertionError("promoted an external cache")
        assert recovery.active_work(base) == sibling
        pointer.unlink()
        assert recovery.active_work(base) == base


def test_link_or_validation_failure_does_not_activate():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        build = root / "build"
        build.mkdir()
        base = build / "ps2-game-hle"
        base.mkdir()
        saved = base / "sentinel.ll"
        saved.write_bytes(b"original guest code")
        elf = root / "SLUS_214.42"
        elf.write_bytes(b"fixture-elf")
        sources = {"ELF": run_game.sha256_path(elf)}
        run_game.archive_known_function_manifest(
            base / "translation-cache.json",
            manifest_for(elf, [0x100008, 0x100010], sources),
        )
        comparison = {
            "missing": [], "extra": [], "mismatched_sha256": [],
            "matching": 2, "expected": 2, "observed": 2,
        }
        builds = []

        def fake_reconstruct(manifest, entries, elf_path, build_dir, config,
                             work, jobs, allow_mismatches):
            assert entries == [0x100008, 0x100010]
            assert not allow_mismatches
            assert saved.read_bytes() == b"original guest code"
            assert not recovery.pointer_path(base).exists()
            work.mkdir(parents=True)
            builds.append(work)
            return comparison

        with patch.object(recovery.run_game, "native_executable", return_value=build / "ee_recomp"), \
             patch.object(recovery.run_game, "ir_generation_inputs", return_value=sources), \
             patch.object(recovery.run_known_replays, "reconstruct", side_effect=fake_reconstruct), \
             patch.object(recovery.run_game, "translation_inputs", return_value={}), \
             patch.object(recovery.run_game, "validate_translation_cache", side_effect=ValueError("bad linked library")):
            try:
                recovery.recover(elf, build, "Release", base, 8)
            except ValueError as exc:
                assert "bad linked library" in str(exc)
            else:
                raise AssertionError("activated a failed reconstruction")
        assert saved.read_bytes() == b"original guest code"
        assert not recovery.pointer_path(base).exists()
        assert len(builds) == 1 and builds[0].is_dir()
        assert not (builds[0] / "translation-cache.json").exists()

        with patch.object(recovery.run_game, "native_executable", return_value=build / "ee_recomp"), \
             patch.object(recovery.run_game, "ir_generation_inputs", return_value=sources), \
             patch.object(recovery.run_known_replays, "reconstruct", side_effect=fake_reconstruct), \
             patch.object(recovery.run_game, "translation_inputs", return_value={}), \
             patch.object(recovery.run_game, "validate_translation_cache"):
            promoted = recovery.recover(elf, build, "Release", base, 8)
        assert promoted == base and recovery.active_work(base) == base
        assert not recovery.pointer_path(base).exists()
        assert not saved.exists()
        backups = list(build.glob("ps2-game-hle-backup-*"))
        assert len(backups) == 1
        assert (backups[0] / "sentinel.ll").read_bytes() == b"original guest code"
        assert (promoted / "ir-comparison.json").is_file()
        assert len(builds) == 2


def test_canonical_promotion_rolls_back_all_names():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        base = root / "ps2-game-hle"
        profile = root / "ps2-game-hle-O2-native"
        stage = root / "ps2-game-hle-recovery-123"
        for directory, data in ((base, "old"), (profile, "native"), (stage, "new")):
            directory.mkdir()
            (directory / "sentinel").write_text(data)
        def fail(directory):
            assert directory == base
            assert (directory / "sentinel").read_text() == "new"
            raise ValueError("post-promotion validation failed")
        try:
            recovery.promote_canonical(base, stage, fail)
        except ValueError as exc:
            assert "post-promotion" in str(exc)
        else:
            raise AssertionError("accepted failed promotion")
        assert (base / "sentinel").read_text() == "old"
        assert (profile / "sentinel").read_text() == "native"
        assert (stage / "sentinel").read_text() == "new"
        assert not list(root.glob("*-backup-*"))
        backup = recovery.promote_canonical(base, stage, lambda directory: None)
        assert (base / "sentinel").read_text() == "new"
        assert (backup / "sentinel").read_text() == "old"
        assert (root / (backup.name + "-O2-native") / "sentinel").read_text() == "native"
        assert not stage.exists() and not profile.exists()


def test_active_source_preflight_rejects_stale_shared_provenance():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        elf = root / "SLUS_214.42"
        elf.write_bytes(b"fixture-elf")
        work = root / "ps2-game-hle"
        ir = work / "ir"
        ir.mkdir(parents=True)
        metadata = ir / "ee_functions.json"
        metadata.write_text("{}", encoding="utf-8")
        sources = {"ELF": digest(b"fixture-elf"), "src/llvm/module_builder.cpp": "new"}
        state = {
            "schema": shared_ir.SCHEMA,
            "ir_sources": sources,
            "metadata_sha256": run_game.sha256_path(metadata),
            "ir_hashes": {"sub_0x100008_0x100008.ll": "a" * 64},
        }
        provenance = ir / shared_ir.PROVENANCE
        shared_ir.write_json(provenance, state)
        original = provenance.read_bytes()
        assert recovery.check_source_header(elf, work, sources) == 1
        for changed, expected in (
            ({**sources, "src/llvm/module_builder.cpp": "old"}, "module_builder.cpp"),
            ({**sources, "ELF": "different"}, "ELF"),
        ):
            try:
                recovery.check_source_header(elf, work, changed)
            except ValueError as exc:
                assert expected in str(exc)
            else:
                raise AssertionError("accepted stale source")
        metadata.write_text('{"modified": true}', encoding="utf-8")
        try:
            recovery.check_source_header(elf, work, sources)
        except ValueError as exc:
            assert "metadata hash changed" in str(exc)
        else:
            raise AssertionError("accepted changed function metadata")
        assert provenance.read_bytes() == original


def test_launcher_recovers_before_automatic_rebuild():
    batch = (ROOT / "run_ps2_game.bat").read_text(encoding="utf-8")
    assert 'if /I "%~1"=="--recover-cache" (' in batch
    assert 'if /I "%~1"=="--accept-new-ir" (' in batch
    assert "--print-active" in batch
    preflight = batch.index("=== Validating shared EE IR")
    flag = batch.index('set "CACHE_FLAG=--no-recompile"', preflight)
    assert "--check-active-source" in batch[preflight:flag]
    assert "exit /b 1" in batch[preflight:flag]
    recovery_guard = batch.index('rem Recovery also migrates valid legacy caches')
    assert "--check-active-source" in batch[recovery_guard:batch.index("rem Strict unattended recovery")]
    assert 'if errorlevel 1 set "CACHE_FLAG="' in batch[recovery_guard:batch.index("rem Strict unattended recovery")]
    assert 'set "BASE_WORK_DIR=build\\ps2-game-hle"' in batch
    strict = batch.index("rem Strict unattended recovery")
    automatic = batch.index(
        'if not defined CACHE_FLAG (\n'
        '    echo Guest translation cache is not reusable; regenerating IR automatically.'
    )
    assert strict < automatic
    section = batch[strict:automatic]
    assert "recover_translation_cache.py --elf" in section
    assert "if not errorlevel 1 (" in section
    assert "set \"CACHE_FLAG=--no-recompile\"" in section
    assert "--accept-new-ir" not in section
    assert "--allow-ir-changes" not in section
    recover = batch.index('if /I "%REQUESTED_SESSION_MODE%"=="recover" (\n    if defined CACHE_FLAG')
    assert recover < strict
    assert "exit /b\n)" in batch[recover:strict]
    assert "run_boot_session.py" not in batch[recover:strict]
    assert "call :confirm_guest_cache_rebuild" not in batch
    assert "choice /C YN" not in batch


def main():
    test_snapshots_are_content_addressed_and_reusable()
    test_changed_lowering_requires_explicit_opt_in()
    test_pointer_rejects_unsafe_paths_and_rollback()
    test_link_or_validation_failure_does_not_activate()
    test_canonical_promotion_rolls_back_all_names()
    test_active_source_preflight_rejects_stale_shared_provenance()
    test_launcher_recovers_before_automatic_rebuild()
    print("safe known-function cache recovery: PASS")


if __name__ == "__main__":
    main()
