#!/usr/bin/env python3
"""An interrupted EE auto-append must not invalidate verified old guest IR."""

import importlib.util
import hashlib
import json
from pathlib import Path
import tempfile
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "ee_run_game_append", ROOT / "tools" / "run_game.py"
)
GAME = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GAME)


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def expect_failure(callback, message):
    try:
        callback()
    except ValueError as error:
        assert message in str(error), (message, str(error))
    else:
        raise AssertionError(f"expected safety rejection: {message}")


def exercise():
    with tempfile.TemporaryDirectory(prefix="ee-interrupted-append-") as temp:
        work = Path(temp)
        ir_dir = work / "ir"
        ir_dir.mkdir()
        obj_dir = work / "objects"
        obj_dir.mkdir()
        elf = work / "game.elf"
        elf.write_bytes(b"game fixture")

        first_path = ir_dir / "function_00100000.ll"
        appended_path = ir_dir / "function_00100010.ll"
        first_path.write_bytes(b"; verified initial guest IR\n")
        appended_path.write_bytes(b"; interrupted genuine guest IR\n")

        first = {
            "name": "function_00100000",
            "host_symbol": "function_00100000",
            "vaddr": "0x00100000",
            "block_pcs": ["0x00100000", "0x00100004"],
            "ir_path": str(first_path),
        }
        appended = {
            "name": "function_00100010",
            "host_symbol": "function_00100010",
            "vaddr": "0x00100010",
            "block_pcs": ["0x00100010", "0x00100014"],
            "ir_path": str(appended_path),
        }

        metadata_path = ir_dir / "ee_functions.json"
        original = {
            "elf": str(elf),
            "function_count": 1,
            "functions": [first],
        }
        write_json(metadata_path, original)
        original_sha = GAME.sha256_path(metadata_path)
        updated = dict(original)
        updated["function_count"] = 2
        updated["functions"] = [first, appended]
        write_json(metadata_path, updated)

        existing_ir_hash = GAME.sha256_path(first_path)
        appended_ir_hash = GAME.sha256_path(appended_path)
        guest_sources = {"ELF": GAME.sha256_path(elf)}
        host_inputs = {
            "ELF": GAME.sha256_path(elf),
            "tools/run_game.py": "host changed",
        }
        cache = work / "translation-cache.json"
        snapshot = {
            "schema": GAME.CACHE_SCHEMA,
            "emit_opt": 0,
            "metadata_sha256": original_sha,
            "ir_sources": guest_sources,
            "ir_hashes": {first_path.name: existing_ir_hash},
        }
        write_json(cache, snapshot)

        old_manifest = {
            "complete": True,
            "optimization_level": 0,
            "objects": [{
                "input": str(first_path),
                "ir_sha256": existing_ir_hash,
            }]
        }
        manifest = obj_dir / "objects.json"
        write_json(manifest, old_manifest)

        def verify():
            return GAME.validate_preserved_translation(
                cache, host_inputs, guest_sources, 0, metadata_path, obj_dir
            )

        suffix, objects_complete = verify()
        assert len(suffix) == 1 and not objects_complete
        assert suffix[0]["vaddr"] == "0x00100010"

        # The SDBZ user is on Windows: Path.write_text() translates
        # LF to CRLF, so the saved metadata_sha256 authenticates CRLF
        # bytes. The old recovery checker reconstructed LF-only bytes
        # and ALWAYS rejected an otherwise-valid Windows append.
        original_crlf = (
            json.dumps(original, indent=2) + "\n"
        ).replace("\n", "\r\n").encode("utf-8")
        updated_crlf = (
            json.dumps(updated, indent=2) + "\n"
        ).replace("\n", "\r\n").encode("utf-8")
        windows_snapshot = dict(snapshot)
        windows_snapshot["metadata_sha256"] = hashlib.sha256(
            original_crlf
        ).hexdigest()
        write_json(cache, windows_snapshot)
        metadata_path.write_bytes(updated_crlf)
        suffix, objects_complete = verify()
        assert len(suffix) == 1 and not objects_complete
        assert suffix[0]["vaddr"] == "0x00100010"
        # Assert both Windows + POSIX cases remain byte-exact; never
        # accept a modified prefix merely by normalizing JSON.
        changed_windows = dict(updated)
        changed_windows["functions"] = [
            {**first, "block_pcs": ["0x00100008"]}, appended
        ]
        metadata_path.write_bytes(
            (json.dumps(changed_windows, indent=2) + "\n"
             ).replace("\n", "\r\n").encode("utf-8")
        )
        expect_failure(verify, "not a verified append-only extension")
        # Restore the original POSIX scenario for subsequent tests.
        write_json(cache, snapshot)
        write_json(metadata_path, updated)

        # A fresh retranslation must prove the saved appended IR and
        # ownership match the original guest ELF before object emission.
        fake_recomp_calls = []

        def fake_recomp(command):
            fake_recomp_calls.append(list(map(str, command)))
            output = Path(command[2])
            assert command[3] == "--functions=0x00100010"
            output.mkdir(parents=True, exist_ok=True)
            generated_path = output / appended_path.name
            generated_path.write_bytes(appended_path.read_bytes())
            rebuilt = dict(appended)
            rebuilt["ir_path"] = str(generated_path)
            write_json(output / "ee_functions.json", {"functions": [rebuilt]})

        with patch.object(GAME, "run", side_effect=fake_recomp):
            GAME.verify_recovered_append(
                "fake-ee-recomp", elf, work, metadata_path, suffix
            )
        assert len(fake_recomp_calls) == 1
        assert GAME.sha256_path(first_path) == existing_ir_hash
        assert GAME.sha256_path(appended_path) == appended_ir_hash

        # If object emission completed before the interrupt, relinking
        # can reuse both objects; if not, only the new IR needs emission.
        whole_manifest = dict(old_manifest)
        whole_manifest["objects"] = [
            *old_manifest["objects"],
            {"input": str(appended_path), "ir_sha256": appended_ir_hash},
        ]
        write_json(manifest, whole_manifest)
        suffix, objects_complete = verify()
        assert len(suffix) == 1 and objects_complete

        # Reject partial object manifests: do not relink an ambiguous
        # interrupted append or silently regenerate original guest IR.
        write_json(manifest, {
            **old_manifest,
            "objects": [*old_manifest["objects"], {
                "input": str(ir_dir / "unexpected.ll"),
                "ir_sha256": "0" * 64,
            }],
        })
        expect_failure(verify, "preserved object manifest does not match append")
        write_json(manifest, old_manifest)

        # In a damaged metadata file, the previous prefix hash MUST
        # detect even a single changed block address or reorder.
        changed = json.loads(metadata_path.read_text(encoding="utf-8"))
        changed["functions"][0]["block_pcs"] = ["0x00100008"]
        write_json(metadata_path, changed)
        expect_failure(verify, "not a verified append-only extension")
        write_json(metadata_path, updated)

        # A changed guest generator/ELF cannot reuse this recovery path.
        expect_failure(
            lambda: GAME.validate_preserved_translation(
                cache, host_inputs, {"ELF": "0" * 64},
                0, metadata_path, obj_dir
            ),
            "guest IR generation input changed: ELF"
        )

        # A mismatching translated suffix must never be accepted.
        def fake_bad_recomp(command):
            fake_recomp(command)
            path = Path(command[2]) / appended_path.name
            path.write_bytes(b"; altered guest instructions\n")
        with patch.object(GAME, "run", side_effect=fake_bad_recomp):
            expect_failure(
                lambda: GAME.verify_recovered_append(
                    "fake-ee-recomp", elf, work, metadata_path, [appended]
                ),
                "does not match a fresh guest translation"
            )

        # A hash mismatch without any new functions is NOT safe
        # to reinterpret as a recovered append.
        current = json.loads(metadata_path.read_text(encoding="utf-8"))
        current["function_count"] = 1
        current["functions"] = [first]
        current["untrusted_field"] = True
        write_json(metadata_path, current)
        expect_failure(verify, "preserved function metadata was modified")

    print("PASS: only authenticated append-only guest IR can be recovered")


if __name__ == "__main__":
    exercise()
