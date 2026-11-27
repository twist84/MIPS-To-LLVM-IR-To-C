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
import refresh_known_ir as refresh
import run_game
import shared_ee_ir as shared_ir
import cache_backup
import subprocess


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
            (work / "ir").mkdir(parents=True)
            metadata = work / "ir" / "ee_functions.json"
            shared_ir.write_json(metadata, {"elf": shared_ir.relative_path(elf, metadata.parent),
                                            "path_format": "manifest-relative-v1"})
            shared_ir.write_json(work / "translation-cache.json",
                                 {"metadata_sha256": run_game.sha256_path(metadata)})
            builds.append(work)
            return comparison

        with patch.object(recovery.run_game, "native_executable", return_value=build / "ee_recomp"), \
             patch.object(recovery.run_game, "ir_generation_inputs", return_value=sources), \
             patch.object(recovery.run_known_replays, "reconstruct", side_effect=fake_reconstruct), \
             patch.object(recovery.run_game, "translation_inputs", return_value={}), \
             patch.object(recovery, "validate_portable_cache", side_effect=ValueError("bad linked library")):
            try:
                recovery.recover(elf, build, "Release", base, 8)
            except ValueError as exc:
                assert "bad linked library" in str(exc)
            else:
                raise AssertionError("activated a failed reconstruction")
        assert saved.read_bytes() == b"original guest code"
        assert not recovery.pointer_path(base).exists()
        assert len(builds) == 1 and builds[0].is_dir()
        assert (builds[0] / "translation-cache.json").exists()

        with patch.object(recovery.run_game, "native_executable", return_value=build / "ee_recomp"), \
             patch.object(recovery.run_game, "ir_generation_inputs", return_value=sources), \
             patch.object(recovery.run_known_replays, "reconstruct", side_effect=fake_reconstruct), \
             patch.object(recovery.run_game, "translation_inputs", return_value={}), \
             patch.object(recovery, "validate_portable_cache"), \
             patch.object(cache_backup, "seven_zip", return_value=None):
            promoted = recovery.recover(elf, build, "Release", base, 8)
        assert promoted == base and recovery.active_work(base) == base
        assert not recovery.pointer_path(base).exists()
        assert not saved.exists()
        backups = list(base.glob("backup-*"))
        assert len(backups) == 1
        assert backups[0].name.endswith("-ir")
        cache_backup.verify_ir_backup(backups[0])
        retained = list(base.glob("retained-*"))
        assert len(retained) == 1
        assert (retained[0] / "sentinel.ll").read_bytes() == b"original guest code"
        assert (promoted / "ir-comparison.json").is_file()
        assert len(builds) == 2


def test_verified_legacy_migration_preserves_ir_and_relocates():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        elf = root / "SLUS_214.42"
        elf.write_bytes(b"fixture-elf")
        base = root / "ps2-game-hle"
        ir = base / "ir"
        ir.mkdir(parents=True)
        sources = {"ELF": run_game.sha256_path(elf)}
        manifest = manifest_for(elf, [0x100008, 0x100010], sources)
        functions = []
        for entry, name in zip((0x100008, 0x100010), manifest["ir_hashes"]):
            path = ir / name
            path.write_bytes(str(entry).encode())
            functions.append({"ir_path": str(path), "vaddr": hex(entry)})
        metadata = ir / "ee_functions.json"
        shared_ir.write_json(metadata, {"elf": str(elf), "functions": functions,
                                        "function_count": 2})
        manifest["metadata_sha256"] = run_game.sha256_path(metadata)
        stage = base / "recovery-123"
        report = recovery.stage_verified_ir(elf, base, stage, manifest, sources)
        assert report["matching"] == 2
        stage_metadata = stage / "ir" / "ee_functions.json"
        converted = shared_ir.read_json(stage_metadata)
        shared_ir.write_json(stage / "translation-cache.json",
                             {"metadata_sha256": run_game.sha256_path(stage_metadata)})
        assert all(Path(fn["ir_path"]).parent == Path(".")
                   for fn in converted["functions"])
        backup = recovery.promote_canonical(base, stage, lambda directory: None, elf=elf)
        promoted = shared_ir.read_json(base / "ir" / "ee_functions.json")
        assert promoted["functions"] == converted["functions"]
        assert shared_ir.elf_path(promoted, base / "ir") == elf
        assert run_game.translated_ir_hashes(base / "ir" / "ee_functions.json") == manifest["ir_hashes"]
        assert (backup / "ir" / "ee_functions.json").is_file()


def test_canonical_promotion_rolls_back_all_names():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        base = root / "ps2-game-hle"
        profile = base / "O2-native"
        stage = base / "recovery-123"
        legacy = root / "ps2-game-hle-recovery-456"
        legacy_profile = root / "ps2-game-hle-IR-O2"
        for directory, data in ((base, "old"), (profile, "native"), (stage, "new"),
                                (legacy, "legacy"), (legacy_profile, "legacy-profile")):
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
        assert (legacy / "sentinel").read_text() == "legacy"
        assert (legacy_profile / "sentinel").read_text() == "legacy-profile"
        assert not list(base.glob("backup-*"))
        backup = recovery.promote_canonical(base, stage, lambda directory: None)
        assert (base / "sentinel").read_text() == "new"
        assert (backup / "sentinel").read_text() == "old"
        assert (backup / "O2-native" / "sentinel").read_text() == "native"
        assert (backup / "legacy-recovery-456" / "sentinel").read_text() == "legacy"
        assert (backup / "legacy-IR-O2" / "sentinel").read_text() == "legacy-profile"
        assert not stage.exists() and not profile.exists()
        assert not legacy.exists() and not legacy_profile.exists()


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


def test_source_fingerprint_migration_does_not_build_or_replace_ir():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp).resolve()
        elf = root / "SLUS_214.42"
        elf.write_bytes(b"fixture-elf")
        source = root / "test.cpp"
        source.write_bytes(b"int f();\r\n")
        legacy = {"ELF": run_game.sha256_path(elf),
                  "test.cpp": run_game.sha256_path(source)}
        source.write_bytes(b"int f();\n")
        sources = shared_ir.fingerprint_sources(
            [("ELF", elf), ("test.cpp", source)], binary_keys={"ELF"})
        work = root / "ps2-game-hle"
        ir = work / "ir"
        ir.mkdir(parents=True)
        module = ir / "sub_0x100008_0x100008.ll"
        module.write_bytes(b"; exact IR\r\n")
        meta = ir / "ee_functions.json"
        shared_ir.write_json(meta, {
            "elf": shared_ir.relative_path(elf, ir),
            "path_format": "manifest-relative-v1",
            "function_count": 1,
            "functions": [{"ir_path": module.name}],
        })
        hashes = {module.name: run_game.sha256_path(module)}
        shared_ir.publish(meta, legacy, hashes)
        provenance = ir / shared_ir.PROVENANCE
        original = provenance.read_bytes()
        metadata_bytes = meta.read_bytes()
        native = work / "O2-native"
        native.mkdir()
        receipt = native / "translation-cache.json"
        receipt.write_bytes(b"native receipt unchanged")
        with patch.object(run_game, "ir_generation_inputs", return_value=sources), \
             patch.object(run_game, "build_linked_library",
                          side_effect=AssertionError("must not build")), \
             patch.object(recovery.run_known_replays, "reconstruct",
                          side_effect=AssertionError("must not reconstruct")):
            module.write_bytes(b"corrupted")
            try:
                recovery.migrate_source_fingerprints(elf, work, root)
            except ValueError as error:
                assert "module hashes changed" in str(error)
            else:
                raise AssertionError("corrupt IR migrated")
            assert provenance.read_bytes() == original
            module.write_bytes(b"; exact IR\r\n")
            assert recovery.migrate_source_fingerprints(elf, work, root) == 1
            assert recovery.migrate_source_fingerprints(elf, work, root) == 0
        assert meta.read_bytes() == metadata_bytes
        assert module.read_bytes() == b"; exact IR\r\n"
        assert receipt.read_bytes() == b"native receipt unchanged"
        assert shared_ir.read_json(provenance)["source_fingerprint_migrations"] == [legacy]


def test_launcher_recovers_before_automatic_rebuild():
    batch = (ROOT / "scripts" / "run_ps2_game.bat").read_text(encoding="utf-8")
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


def test_ir_only_backup_partition_preserves_native_and_authenticates():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp).resolve()
        base = root / "ps2-game-hle"
        stage = base / "recovery-123"
        ir = base / "ir"
        ir.mkdir(parents=True)
        (ir / "example.ll").write_bytes(b"old IR")
        (ir / "ee_functions.json").write_bytes(b"old metadata")
        (ir / shared_ir.PROVENANCE).write_bytes(b"old provenance")
        (base / "translation-cache.json").write_bytes(b"old receipt")
        (base / "game-translated.dll").write_bytes(b"old DLL")
        profile = base / "O2-native"
        objects = profile / "objects"
        objects.mkdir(parents=True)
        (objects / "example.o").write_bytes(b"native")
        (objects / "objects.json").write_bytes(b"object receipt")
        (profile / "translation-cache.json").write_bytes(b"profile receipt")
        shared_ir.attach(profile, ir)
        (base / "report.json").write_bytes(b"old report")
        (stage / "ir").mkdir(parents=True)
        (stage / "ir" / "example.ll").write_bytes(b"new IR")
        (stage / "ir" / shared_ir.PROVENANCE).write_bytes(b"new provenance")
        existing_backup = base / "backup-existing"
        existing_backup.mkdir()
        (existing_backup / "sentinel").write_bytes(b"existing backup")
        profiles = recovery.canonical_shared_profiles(base, stage)
        assert profiles == [profile]
        backup = recovery.promote_canonical(base, stage, lambda _: None,
                                           preserved_profiles=profiles)
        result = cache_backup.partition_backup(backup, profiles)
        assert result.name == "backup-123-ir"
        inventory = cache_backup.verify_ir_backup(result)
        assert set(inventory["files"]) == {
            "ir/example.ll", "ir/ee_functions.json", "ir/.shared-ir-provenance.json",
            "translation-cache.json", "O2-native/translation-cache.json",
            "O2-native/shared-ir.json", "O2-native/objects/objects.json",
        }
        assert (result / "ir/example.ll").read_bytes() == b"old IR"
        assert (base / "ir/example.ll").read_bytes() == b"new IR"
        assert (objects / "example.o").read_bytes() == b"native"
        assert (base / "retained-123/game-translated.dll").read_bytes() == b"old DLL"
        assert (base / "retained-123/report.json").read_bytes() == b"old report"
        assert (existing_backup / "sentinel").read_bytes() == b"existing backup"
        (result / "ir/example.ll").write_bytes(b"tampered")
        try:
            cache_backup.verify_ir_backup(result)
        except ValueError as error:
            assert "inventory/hash mismatch" in str(error)
        else:
            raise AssertionError("modified backup accepted")


def test_verified_maximum_compression_backup():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        backup = root / "backup-123"
        objects = backup / "O5-native" / "objects"
        objects.mkdir(parents=True)
        (objects / "objects.json").write_text("manifest")
        (objects / "example.o").write_bytes(b"original")
        calls = []

        def fake_run(command, **kwargs):
            calls.append(command)
            if command[1] == "a":
                assert kwargs["cwd"] == backup
                assert all(flag in command for flag in ("-t7z", "-mx=9", "-m0=LZMA2", "-ms=on"))
                Path(command[-2]).write_bytes(b"archive")
            elif command[1] == "t":
                assert backup.is_dir()
                assert Path(command[-1]).is_file()
            elif command[1] == "x":
                assert "-i!O5-native/objects/*" in command
                target = Path(next(v[2:] for v in command if v.startswith("-o")))
                extracted = target / "O5-native" / "objects"
                extracted.mkdir(parents=True)
                (extracted / "example.o").write_bytes(b"original")
            return None

        with patch.object(cache_backup, "seven_zip", return_value="7z"), \
             patch.object(cache_backup.subprocess, "run", side_effect=fake_run):
            archive = cache_backup.compress_backup(backup)
            assert archive == root / "backup-123.7z"
            assert archive.is_file() and not backup.exists()
            with cache_backup.backup_object_candidates(root, "O5-native") as candidates:
                found = [p for p in candidates if (p / "objects" / "example.o").is_file()]
                assert len(found) == 1
                assert (found[0] / "objects" / "example.o").read_bytes() == b"original"
            assert not found[0].exists()
        assert [c[1] for c in calls] == ["a", "t", "x"]

        backup.mkdir()
        (backup / "sentinel").write_bytes(b"preserve")
        archive.unlink()
        def failed_test(command, **kwargs):
            if command[1] == "a":
                Path(command[-2]).write_bytes(b"invalid")
            else:
                raise subprocess.CalledProcessError(2, command)

        with patch.object(cache_backup, "seven_zip", return_value="7z"), \
             patch.object(cache_backup.subprocess, "run", side_effect=failed_test):
            assert cache_backup.compress_backup(backup) == backup
        assert (backup / "sentinel").read_bytes() == b"preserve"
        assert not archive.exists()


def test_real_7z_backup_if_available():
    if not cache_backup.seven_zip():
        return
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        backup = root / "backup-123"
        for profile in ("O5-native", "legacy-O5-native", "legacy-recovery-42-O5-native"):
            objects = backup / profile / "objects"
            objects.mkdir(parents=True)
            (objects / "example.o").write_bytes(profile.encode())
        archive = cache_backup.compress_backup(backup)
        assert archive.suffix == ".7z" and not backup.exists()
        with cache_backup.backup_object_candidates(root, "O5-native") as candidates:
            recovered = {(p / "objects" / "example.o").read_bytes()
                         for p in candidates if (p / "objects" / "example.o").is_file()}
            assert recovered == {b"O5-native", b"legacy-O5-native", b"legacy-recovery-42-O5-native"}


def test_overwrite_ir_uses_active_inventory_and_selected_profile():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        elf = root / "fixture.elf"
        elf.write_bytes(b"fixture-elf")
        base = root / "ps2-game-hle"
        ir = base / "ir"
        ir.mkdir(parents=True)
        old_sources = {"ELF": run_game.sha256_path(elf), "lowering": "old"}
        new_sources = dict(old_sources, lowering="new")
        entries = [0x100020, 0x100008]
        names = [f"sub_0x{pc:x}_0x{pc:x}.ll" for pc in entries]

        def write_ir(directory, payload):
            directory.mkdir(parents=True, exist_ok=True)
            metadata = {
                "elf": shared_ir.relative_path(elf, directory),
                "path_format": "manifest-relative-v1",
                "function_count": len(entries),
                "functions": [
                    {"ir_path": name, "vaddr": hex(pc), "block_pcs": [hex(pc)]}
                    for name, pc in zip(names, entries)
                ],
            }
            for name in names:
                (directory / name).write_bytes(payload)
            shared_ir.write_json(directory / "ee_functions.json", metadata)
            return {name: digest(payload) for name in names}

        old_hashes = write_ir(ir, b"old")
        shared_ir.publish(ir / "ee_functions.json", old_sources, old_hashes)
        profile = base / "O2-native"
        profile.mkdir()
        shared_ir.attach(profile, ir)
        (profile / "objects").mkdir()
        (profile / "objects" / "unchanged.o").write_bytes(b"retained")
        calls = []

        def fake_reconstruct(manifest, known_entries, elf_arg, build, config,
                             work, jobs, allow_mismatches, *, ir_only=False):
            assert known_entries == entries
            assert manifest["ir_hashes"] == old_hashes
            assert allow_mismatches and ir_only and elf_arg == elf
            write_ir(work / "ir", b"new")
            return {"missing": [], "extra": [], "mismatched_sha256": names}

        def fake_build(source, work, metadata, objects, library, opt, **kwargs):
            assert work == profile and metadata == ir / "ee_functions.json"
            assert opt == 5 and kwargs["emit_jobs"] == 3
            assert (objects / "unchanged.o").read_bytes() == b"retained"
            assert shared_ir.current_state(metadata, new_sources)["ir_hashes"] == {
                name: digest(b"new") for name in names}
            calls.append(opt)
            raise RuntimeError("synthetic link failure")

        with patch.object(run_game, "ir_generation_inputs", return_value=new_sources), \
             patch.object(run_game, "native_executable", return_value=root / "ee_recomp"), \
             patch.object(run_game, "translation_inputs", return_value={"ELF": old_sources["ELF"]}), \
             patch.object(refresh.known, "reconstruct", side_effect=fake_reconstruct), \
             patch.object(run_game, "build_linked_library", side_effect=fake_build):
            try:
                refresh.refresh(elf, root, "Release", base, 5, 3)
            except RuntimeError as error:
                assert "synthetic link failure" in str(error)
            else:
                raise AssertionError("failed link was accepted")
        assert calls == [5]
        assert shared_ir.current_state(ir / "ee_functions.json", old_sources)["ir_hashes"] == old_hashes
        assert all((ir / name).read_bytes() == b"old" for name in names)
        assert not list(base.glob(".overwrite-ir-*"))

        def successful_build(source, work, metadata, objects, library, opt, **kwargs):
            assert opt == 5 and work == profile
            library.write_bytes(b"new-native")

        with patch.object(run_game, "ir_generation_inputs", return_value=new_sources), \
             patch.object(run_game, "native_executable", return_value=root / "ee_recomp"), \
             patch.object(run_game, "translation_inputs", return_value={"ELF": old_sources["ELF"]}), \
             patch.object(refresh.known, "reconstruct", side_effect=fake_reconstruct), \
             patch.object(run_game, "build_linked_library", side_effect=successful_build), \
             patch.object(run_game, "validate_translation_cache") as validate:
            refresh.refresh(elf, root, "Release", base, 5, 3)
            validate.assert_called_once()
        assert all((ir / name).read_bytes() == b"new" for name in names)
        assert not list(base.glob(".overwrite-ir-*"))
        assert shared_ir.ir_directory(profile) == ir
        assert shared_ir.read_json(profile / "translation-cache.json")["emit_opt"] == 5
        assert not (base / "translation-cache.json").exists()
        assert (profile / "objects" / "unchanged.o").is_file()


def test_overwrite_launcher_routes_before_preflight():
    launcher = (ROOT / "scripts" / "run_ps2_game.bat").read_text(encoding="utf-8")
    assert 'if /I "%~1"=="--overwrite-ir" (' in launcher
    assert 'if defined OVERWRITE_IR if /I not "%REQUESTED_SESSION_MODE%"=="full"' in launcher
    start = launcher.index("python tools\\refresh_known_ir.py")
    end = launcher.index("=== Validating shared EE IR")
    assert start < end
    assert "--emit-opt=%SDBZ_EE_OPT%" in launcher[start:end]
    assert 'set "CACHE_FLAG=--no-recompile"' in launcher[start:end]
    assert 'if errorlevel 1 exit /b 1' in launcher[start:end]
    assert 'if not defined CACHE_FLAG if not "%SDBZ_EE_OPT%"=="0"' in launcher[start:end]


def main():
    test_overwrite_ir_uses_active_inventory_and_selected_profile()
    test_overwrite_launcher_routes_before_preflight()
    test_ir_only_backup_partition_preserves_native_and_authenticates()
    test_verified_maximum_compression_backup()
    test_real_7z_backup_if_available()
    test_snapshots_are_content_addressed_and_reusable()
    test_changed_lowering_requires_explicit_opt_in()
    test_pointer_rejects_unsafe_paths_and_rollback()
    test_link_or_validation_failure_does_not_activate()
    test_verified_legacy_migration_preserves_ir_and_relocates()
    test_canonical_promotion_rolls_back_all_names()
    test_active_source_preflight_rejects_stale_shared_provenance()
    test_source_fingerprint_migration_does_not_build_or_replace_ir()
    test_launcher_recovers_before_automatic_rebuild()
    print("safe known-function cache recovery: PASS")


if __name__ == "__main__":
    main()
