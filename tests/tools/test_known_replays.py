#!/usr/bin/env python3
"""Regression tests for known-IR reconstruction and matched CPU-only replay."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import run_known_replays as known
import recover_translation_cache as recovery


def digest(data):
    return hashlib.sha256(data).hexdigest()


def sample_manifest(entries, elf_bytes=b"fixture-elf"):
    hashes = {
        f"sub_0x{entry:x}_0x{entry:x}.ll": digest(f"; IR {entry}\n".encode())
        for entry in entries
    }
    return {
        "schema": "ee-translation-cache-v1",
        "inputs": {"ELF": digest(elf_bytes)},
        "ir_hashes": hashes,
    }


def test_read_manifest():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        manifest = sample_manifest([0x100008, 0x180000])
        json_path = root / "translation-cache.json"
        json_path.write_text(json.dumps(manifest), encoding="utf-8")
        assert known.read_manifest(json_path)[1] == [0x100008, 0x180000]
        # Snapshots use sorted JSON keys but retain original discovery order.
        reordered = dict(manifest)
        reordered["known_entries"] = [
            "sub_0x180000_0x180000.ll", "sub_0x100008_0x100008.ll",
        ]
        json_path.write_text(json.dumps(reordered, sort_keys=True), encoding="utf-8")
        assert known.read_manifest(json_path)[1] == [0x180000, 0x100008]
        json_path.write_text(json.dumps(manifest), encoding="utf-8")
        archive = root / "diagnostic.zip"
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("translation-cache.json", json.dumps(manifest))
        assert known.read_manifest(archive)[1] == [0x100008, 0x180000]
        invalid = [
            {"ir_hashes": {**manifest["ir_hashes"], "../evil.ll": "a" * 64}},
            {"ir_hashes": {"sub_0x100008_0x10000c.ll": "a" * 64}},
            {"ir_hashes": {"sub_0x100008_0x100008.ll": "not a SHA"}},
            {"inputs": {"ELF": "x"}},
            {"known_entries": ["sub_0x100008_0x100008.ll"]},
            {"known_entries": ["sub_0x100008_0x100008.ll"] * 2},
        ]
        for change in invalid:
            bad = {**manifest, **change}
            json_path.write_text(json.dumps(bad), encoding="utf-8")
            try:
                known.read_manifest(json_path)
            except ValueError:
                pass
            else:
                raise AssertionError(f"accepted malformed manifest: {change}")
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("translation-cache.json", json.dumps(manifest))
            output.writestr("nested/translation-cache.json", json.dumps(manifest))
        try:
            known.read_manifest(archive)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted ambiguous archive")


def test_hash_comparison():
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        ir = work / "ir"
        ir.mkdir()
        (ir / "sub_0x100008_0x100008.ll").write_bytes(b"one")
        (ir / "sub_0x180000_0x180000.ll").write_bytes(b"two")
        hashes = {
            "sub_0x100008_0x100008.ll": digest(b"one"),
            "sub_0x180000_0x180000.ll": digest(b"else"),
            "sub_0x190000_0x190000.ll": digest(b"third"),
        }
        compare = known.compare_ir(work, hashes)
        assert compare["expected"] == 3
        assert compare["observed"] == 2
        assert compare["matching"] == 1
        assert compare["missing"] == ["sub_0x190000_0x190000.ll"]
        assert compare["mismatched_sha256"] == ["sub_0x180000_0x180000.ll"]


def test_reconstruction_is_batched_and_linked_once():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        elf = root / "SLUS_214.42"
        elf.write_bytes(b"fixture-elf")
        entries = [0x100008 + 4 * i for i in range(260)]
        manifest = sample_manifest(entries)
        work = root / "isolated"
        original_name = lambda entry: f"sub_0x{entry:x}_0x{entry:x}.ll"
        calls = {"seed": 0, "batches": [], "link": 0, "write_cache": 0}

        def emit_one(entry):
            (work / "ir" / original_name(entry)).write_bytes(f"; IR {entry}\n".encode())

        def fake_seed(command):
            assert "--function=0x00100008" in command
            calls["seed"] += 1
            emit_one(entries[0])
            metadata = {
                "elf": str(elf), "entry_point": "0x00100008",
                "functions": [{
                    "vaddr": f"0x{entries[0]:08x}",
                    "ir_path": str(work / "ir" / original_name(entries[0])),
                }],
                "function_count": 1,
            }
            (work / "ir" / "ee_functions.json").write_text(json.dumps(metadata))

        def fake_append(recomp, elf_path, work_dir, metadata_path, addresses):
            assert work_dir == work and elf_path == elf
            calls["batches"].append(list(addresses))
            metadata = json.loads(metadata_path.read_text())
            for entry in addresses:
                emit_one(entry)
                metadata["functions"].append({
                    "vaddr": f"0x{entry:08x}",
                    "ir_path": str(work / "ir" / original_name(entry)),
                })
            metadata["function_count"] = len(metadata["functions"])
            metadata_path.write_text(json.dumps(metadata))
            return addresses

        def fake_link(*args, **kwargs):
            calls["link"] += 1
            args[4].write_bytes(b"dummy library")

        def fake_cache(*args, **kwargs):
            calls["write_cache"] += 1
            args[0].write_text("{}")

        with patch.object(known.run_game, "native_executable", return_value=root / "ee_recomp"), \
             patch.object(known.run_game, "run", side_effect=fake_seed), \
             patch.object(known.run_game, "batch_append_functions", side_effect=fake_append), \
             patch.object(known.run_game, "build_linked_library", side_effect=fake_link), \
             patch.object(known.run_game, "write_translation_cache", side_effect=fake_cache), \
             patch.object(known.run_game, "translation_inputs", return_value={}), \
             patch.object(known.run_game, "ir_generation_inputs", return_value={}):
            compare = known.reconstruct(manifest, entries, elf, root, "Release",
                                        work, 8, False)
            assert compare["matching"] == 260
            assert calls["seed"] == 1
            assert [len(batch) for batch in calls["batches"]] == [128, 128, 3]
            assert calls["link"] == calls["write_cache"] == 1
            try:
                known.reconstruct(manifest, entries, elf, root, "Release",
                                  work, 8, False)
            except ValueError as exc:
                assert "not fresh" in str(exc)
            else:
                raise AssertionError("overwrote prior guest IR")


def test_real_append_reserves_future_missing_entries():
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        ir = work / "ir"
        ir.mkdir()
        metadata_path = ir / "ee_functions.json"
        first, second, third = 0x100008, 0x100010, 0x100020
        metadata_path.write_text(json.dumps({
            "function_count": 1,
            "functions": [{"vaddr": hex(first), "block_pcs": [hex(first)],
                           "ir_path": "first.ll"}],
        }))

        def fake_recompiler(command):
            output = Path(command[2])
            output.mkdir(parents=True)
            entry = int(command[3].split("=")[1], 0)
            path = output / f"sub_{entry:x}.ll"
            path.write_text("; synthetic test IR")
            blocks = [hex(entry), hex(third)] if entry == second else [hex(entry)]
            (output / "ee_functions.json").write_text(json.dumps({
                "functions": [{"vaddr": hex(entry), "block_pcs": blocks,
                               "ir_path": str(path)}],
            }))

        with patch.object(known.run_game, "run", side_effect=fake_recompiler):
            known.run_game.batch_append_functions(
                "recomp", work / "elf", work, metadata_path, [second],
                reserved_entries={second, third})
            partial = json.loads(metadata_path.read_text())
            assert third not in known.run_game.covered_pcs(partial)
            known.run_game.batch_append_functions(
                "recomp", work / "elf", work, metadata_path, [third],
                reserved_entries={second, third})
        final = json.loads(metadata_path.read_text())
        assert final["function_count"] == 3
        assert known.run_game.covered_pcs(final) == {first, second, third}


def test_explicit_entry_restoration_keeps_all_entries_and_blocks():
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        metadata_path = work / "ee_functions.json"
        entries = [0x100008, 0x100010, 0x100020, 0x100030]
        first, second, third, fourth = entries
        original = {
            "functions": [
                {"vaddr": hex(first), "block_pcs": [hex(first), hex(second), "0x100014"]},
                {"vaddr": hex(third), "block_pcs": [hex(third), "0x100024"]},
            ],
            "function_count": 2,
        }
        metadata_path.write_text(json.dumps(original))
        try:
            known.restore_missing_entries("recomp", work / "elf", work,
                                          metadata_path, entries, False)
        except ValueError as exc:
            assert "0x00100010" in str(exc) and "0x00100030" in str(exc)
        else:
            raise AssertionError("strict recovery accepted missing entries")
        assert json.loads(metadata_path.read_text()) == original
        calls = []

        def append(recomp, elf, work_dir, path, addresses, *, reserved_entries):
            assert reserved_entries == {second, fourth}
            pc = addresses[0]
            calls.append(pc)
            metadata = json.loads(path.read_text())
            assert pc not in known.run_game.covered_pcs(metadata)
            metadata["functions"].append({
                "vaddr": hex(pc), "block_pcs": [hex(pc), hex(pc + 8)]})
            metadata["function_count"] = len(metadata["functions"])
            path.write_text(json.dumps(metadata))

        with patch.object(known.run_game, "batch_append_functions", side_effect=append):
            known.restore_missing_entries("recomp", work / "elf", work,
                                          metadata_path, entries, True)
        restored = json.loads(metadata_path.read_text())
        assert calls == [second, fourth]
        assert [int(fn["vaddr"], 0) for fn in restored["functions"]] == entries
        assert restored["function_count"] == len(entries)
        assert known.run_game.covered_pcs(original).issubset(
            known.run_game.covered_pcs(restored))
        assert restored["functions"][0]["block_pcs"] == [hex(first), "0x100014"]


def test_reconstruction_hash_mismatch_fails_closed():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        elf = root / "SLUS_214.42"
        elf.write_bytes(b"fixture-elf")
        manifest = sample_manifest([0x100008])
        work = root / "isolated"
        def fake_seed(command):
            (work / "ir" / "sub_0x100008_0x100008.ll").write_bytes(b"; different IR")
            (work / "ir" / "ee_functions.json").write_text(json.dumps({
                "functions": [{"vaddr": "0x100008"}], "function_count": 1,
            }))
        with patch.object(known.run_game, "native_executable", return_value=root / "ee_recomp"), \
             patch.object(known.run_game, "run", side_effect=fake_seed), \
             patch.object(known.run_game, "build_linked_library") as link:
            try:
                known.reconstruct(manifest, [0x100008], elf, root, "Release",
                                  work, 8, False)
            except ValueError as exc:
                assert "IR SHA-256 mismatch" in str(exc)
            else:
                raise AssertionError("allowed mismatched IR without opt-in")
            link.assert_not_called()


def test_replay_pair_is_cpu_only_and_uses_one_library():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        elf = root / "SLUS_214.42"
        disc = root / "PS2_GAME"
        disc.mkdir()
        work = root / "existing"
        work.mkdir()
        cache = work / "translation-cache.json"
        cache.write_bytes(b"verified unchanged cache")
        calls = []
        def fake_run(command, env=None, check=False):
            assert "--no-recompile" in command and "--auto-expand=0" in command
            assert "--memcard-discard-writes" in command
            assert not any("--auto-expand=" in c and c != "--auto-expand=0" for c in command)
            assert not any("--gs-webgpu" in c or "--gs-snapshot" in c for c in command)
            assert "--disc-folder" in command
            report = Path(command[command.index("--report") + 1])
            trace = Path(command[command.index("--trace-instructions") + 1])
            cross = env.get("SDBZ_PAD0_CROSS_FIRST_POLL")
            if cross:
                assert cross == "200" and env["SDBZ_PAD0_CROSS_POLL_COUNT"] == "8"
            else:
                assert "SDBZ_PAD0_CROSS_POLL_COUNT" not in env
            report.write_text(json.dumps({"dispatches": 6000000, "final_pc": 0x100008}))
            trace.write_text(
                "attempt,pc,opcode_word,kind,reference_slot,hle_service\n" +
                ("1,0x00109d50,0x00000000,0,0,0\n" * (8 if cross else 0)) +
                "# truncated=false range=0x00109d50:0x00109d50\n"
            )
            calls.append(cross)
            return type("Result", (), {"returncode": 4})()
        with patch.object(known.subprocess, "run", side_effect=fake_run), \
             patch.dict(known.os.environ, {
                 "SDBZ_PAD0_CROSS_FIRST_POLL": "999",
                 "SDBZ_PAD0_CROSS_POLL_COUNT": "99",
             }):
            result = known.replay_pair(elf, disc, root, "Release", work,
                                       root / "output", 6000000, 600, 8)
        assert calls == [None, "200"]
        assert result["results"]["neutral"]["original_branch_0x00109d50"] == 0
        assert result["results"]["cross"]["original_branch_0x00109d50"] == 8
        assert cache.read_bytes() == b"verified unchanged cache"


def test_incomplete_branch_trace_is_not_evidence():
    with tempfile.TemporaryDirectory() as temp:
        trace = Path(temp) / "branch.csv"
        trace.write_text(
            "attempt,pc,opcode_word,kind,reference_slot,hle_service\n"
            "1,0x00109d50,0x00000000,0,0,0\n"
            "# truncated=true range=0x00109d50:0x00109d50\n"
        )
        try:
            known.trace_branch_count(trace)
        except ValueError as exc:
            assert "truncated" in str(exc)
        else:
            raise AssertionError("accepted partial guest branch evidence")


def test_existing_cache_uses_only_safe_relink():
    with tempfile.TemporaryDirectory() as temp:
        calls = []
        def fake_run(command, check=False):
            calls.append(command)
            return type("Result", (), {
                "returncode": 1 if "--no-recompile" in command else 0,
            })()
        with patch.object(known.subprocess, "run", side_effect=fake_run):
            known.prepare_existing(Path("SLUS_214.42"), Path("build"),
                                   "Release", Path(temp), 8)
        assert len(calls) == 2
        assert "--no-recompile" in calls[0] and "--link-only" in calls[0]
        assert "--relink-existing" in calls[1] and "--link-only" in calls[1]
        assert all("--auto-expand=0" not in cmd for cmd in calls)
        assert all("--functions=all" not in cmd for cmd in calls)


def test_matched_launcher_never_falls_back_to_guest_rebuild():
    launcher = (ROOT / "scripts" / "run_ps2_game.bat").read_text(encoding="utf-8")
    assert 'if /I "%~1"=="--matched" (' in launcher
    assert 'if /I "%REQUESTED_SESSION_MODE%"=="matched" (' in launcher
    assert 'python tools\\run_known_replays.py ^' in launcher
    # Mode must branch BEFORE the normal automatic IR rebuild fallback.
    matched = launcher.index(
        'rem A paired experiment MUST use the existing guest IR'
    )
    automatic = launcher.index(
        'if not defined CACHE_FLAG (\n'
        '    echo Guest translation cache is not reusable; regenerating IR automatically.'
    )
    assert matched < automatic
    section = launcher[matched:automatic]
    assert "call :confirm_guest_cache_rebuild" not in launcher
    assert "choice /C YN" not in launcher
    assert 'if not defined CACHE_FLAG (' in section
    assert 'exit /b 1' in section
    assert 'exit /b\n)' in section
    assert 'exit /b %ERRORLEVEL%' not in section
    assert '--max-dispatches=%MAX_REPLAY_DISPATCHES%' in section
    assert 'if not defined MAX_REPLAY_DISPATCHES set "MAX_REPLAY_DISPATCHES=%MAX_DISPATCHES%"' in section
    assert section.index('if not defined MAX_REPLAY_DISPATCHES') < section.index(
        'if /I "%REQUESTED_SESSION_MODE%"=="matched" ('
    )
    assert '--timeout-seconds=%BOOT_TIMEOUT_SECONDS%' in section
    assert '--auto-expand' not in section
    assert 'run_boot_session.py' not in section


def test_recovery_includes_native_profile_inventory():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        elf = root / "SLUS_214.42"
        elf.write_bytes(b"fixture-elf")
        base = root / "ps2-game-hle"
        current_sources = {"lowering": "new"}
        old_sources = {"lowering": "old"}

        def save(work, entries, profile, sources, pipeline=None, elf_bytes=b"fixture-elf"):
            work.mkdir(exist_ok=True)
            manifest = sample_manifest(entries, elf_bytes)
            manifest.update(emit_opt=profile, ir_sources=sources, ir_pipeline=pipeline)
            path = work / "translation-cache.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            return path

        base_path = save(base, [0x100008], 0, current_sources)
        optimized = base.with_name(base.name + "-O2-native")
        optimized_path = save(optimized, [0x180000, 0x100008], 5, old_sources)
        assert recovery.select_manifest(base, elf, current_sources)[0] == base_path
        chosen = recovery.select_manifest(base, elf, current_sources, True)
        assert chosen[0] == optimized_path
        assert chosen[2] == [0x180000, 0x100008]
        assert chosen[3] is False

        # Source-change consent never relaxes the ELF guard or accepts
        # an unknown IR pipeline.
        save(base.with_name(base.name + "-O2"), [4, 8, 12], 2,
             current_sources, elf_bytes=b"different-elf")
        save(base.with_name(base.name + "-O3"), [4, 8, 12], 3,
             current_sources, pipeline="unknown")
        assert recovery.select_manifest(base, elf, current_sources, True)[0] == optimized_path
        assert optimized_path.is_file() and base_path.is_file()

        # Profile 4 records the same raw guest IR despite its O2 pipeline.
        ir_o2 = save(base.with_name(base.name + "-IR-O2"), [4, 8, 12],
                     4, old_sources, pipeline="llvm-default-O2-v1")
        assert recovery.select_manifest(base, elf, current_sources, True)[0] == ir_o2
        retired = base.with_name(base.name + "-recovery-20261124-O2-native")
        retired_path = save(retired, [4, 8, 12, 16], 5, old_sources)
        retired.with_name(base.name + "-recovery-20261124").mkdir()
        assert recovery.select_manifest(base, elf, current_sources, True)[0] == retired_path
        nested = base / "O2-native"
        nested_path = save(nested, [4, 8, 12, 16, 20], 5, old_sources)
        assert recovery.select_manifest(base, elf, current_sources, True)[0] == nested_path


def main():
    test_recovery_includes_native_profile_inventory()
    test_matched_launcher_never_falls_back_to_guest_rebuild()
    test_read_manifest()
    test_hash_comparison()
    test_reconstruction_is_batched_and_linked_once()
    test_real_append_reserves_future_missing_entries()
    test_explicit_entry_restoration_keeps_all_entries_and_blocks()
    test_reconstruction_hash_mismatch_fails_closed()
    test_replay_pair_is_cpu_only_and_uses_one_library()
    test_incomplete_branch_trace_is_not_evidence()
    test_existing_cache_uses_only_safe_relink()
    print("known-function matched replays: PASS")


if __name__ == "__main__":
    main()
