#!/usr/bin/env python3
"""Host-only SDBZ diagnostics must not invalidate preserved guest LLVM IR."""

import importlib.util
import json
from pathlib import Path
import tempfile


SOURCE_ROOT = Path(__file__).resolve().parents[2]
MODULE = SOURCE_ROOT / "tools" / "run_game.py"
SPEC = importlib.util.spec_from_file_location("run_game_input_split", MODULE)
run_game = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_game)


def verify_real_probe_location():
    """Protect the placement of the real native pad-return observer."""
    probe_name = "ee_sdbz_pad_return_probe.h"
    probe_path = SOURCE_ROOT / "src" / "runtime" / probe_name
    if not probe_path.is_file():
        raise AssertionError("SDBZ pad-return observer must live in src/runtime")
    if (SOURCE_ROOT / "include" / "ee" / probe_name).exists():
        raise AssertionError("host-only observer cannot live in guest IR include/ee")

    # A synthetic ELF/compiler is sufficient for the source fingerprint maps.
    with tempfile.TemporaryDirectory(prefix="ee-ir-fingerprint-") as directory:
        root = Path(directory)
        elf = root / "game.elf"
        recomp = root / "ee_recomp"
        elf.write_bytes(b"synthetic ELF marker")
        recomp.write_bytes(b"synthetic compiler marker")
        host = run_game.translation_inputs(elf, recomp, SOURCE_ROOT)
        guest = run_game.ir_generation_inputs(elf, SOURCE_ROOT)
        key = "src/runtime/" + probe_name
        assert key in host, "host relink must fingerprint the native diagnostic"
        assert key not in guest, "host-only diagnostic must not fingerprint guest IR"


def verify_preservation_when_host_only_inputs_change():
    with tempfile.TemporaryDirectory(prefix="ee-ir-preserve-") as directory:
        root = Path(directory)
        elf = root / "game.elf"
        recomp = root / "ee_recomp"
        elf.write_bytes(b"synthetic game")
        recomp.write_bytes(b"synthetic recompiler")
        source_root = root / "source"
        paths = {
            "tools/run_game.py": b"runner",
            "tools/verify_ir.py": b"verify",
            "tools/shared_ee_ir.py": b"shared source store",
            "tools/link_native_objects.py": b"link",
            "src/ee/ee_dispatcher.cpp": b"dispatch",
            "src/ee/recompiled_functions.cpp": b"dispatch map",
            "src/ee/decoder.cpp": b"decoder",
            "src/tools/ee_recomp_main.cpp": b"recomp front-end",
            "include/ee/guest_lifter_input.h": b"guest header",
            "src/runtime/ee_sdbz_pad_return_probe.h": b"first host probe",
        }
        for name, content in paths.items():
            path = source_root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

        initial_host = run_game.translation_inputs(elf, recomp, source_root)
        initial_guest = run_game.ir_generation_inputs(elf, source_root)
        work = root / "work"
        ir = work / "ir" / "function_00100000.ll"
        ir.parent.mkdir(parents=True, exist_ok=True)
        ir.write_text("; synthetic verified IR\n", encoding="utf-8")
        metadata_path = work / "ir" / "ee_functions.json"
        metadata_path.write_text(
            json.dumps({"functions": [{"ir_path": str(ir)}]}),
            encoding="utf-8"
        )
        object_dir = work / "objects"
        object_dir.mkdir(parents=True, exist_ok=True)
        (object_dir / "objects.json").write_text(json.dumps({
            "complete": True,
            "optimization_level": 0,
            "objects": [{
                "input": str(ir),
                "ir_sha256": run_game.sha256_path(ir)
            }]
        }), encoding="utf-8")

        cache = work / "translation-cache.json"
        cache.write_text(json.dumps({
            "schema": run_game.CACHE_SCHEMA,
            "emit_opt": 0,
            "metadata_sha256": run_game.sha256_path(metadata_path),
            "inputs": initial_host,
            "ir_sources": initial_guest,
            "ir_hashes": run_game.translated_ir_hashes(metadata_path)
        }), encoding="utf-8")

        # A cache produced during the short-lived misplaced-header
        # window may include this exact host-only key in ir_sources.
        # Permit migration only when the relocated header bytes match.
        original_cache = json.loads(cache.read_text(encoding="utf-8"))
        legacy_cache = dict(original_cache)
        legacy_cache["ir_sources"] = dict(initial_guest)
        legacy_cache["ir_sources"][
            "include/ee/ee_sdbz_pad_return_probe.h"
        ] = initial_host["src/runtime/ee_sdbz_pad_return_probe.h"]
        cache.write_text(json.dumps(legacy_cache), encoding="utf-8")
        run_game.validate_preserved_translation(
            cache, initial_host, initial_guest, 0, metadata_path, object_dir
        )

        probe = source_root / "src/runtime/ee_sdbz_pad_return_probe.h"
        probe.write_bytes(b"updated host diagnostics")
        extra = source_root / "src/runtime/ee_sdbz_other_diagnostic.h"
        extra.write_bytes(b"additional host-only probe")
        next_host = run_game.translation_inputs(elf, recomp, source_root)
        next_guest = run_game.ir_generation_inputs(elf, source_root)
        assert next_host != initial_host
        assert next_guest == initial_guest

        # Byte-mismatched relocation is NOT permitted to bypass guest
        # IR source provenance; only the exact saved host header may move.
        try:
            run_game.validate_preserved_translation(
                cache, next_host, next_guest, 0, metadata_path, object_dir
            )
        except ValueError as error:
            assert "guest IR generation input changed:" in str(error)
        else:
            raise AssertionError("modified relocated host header was accepted")
        cache.write_text(json.dumps(original_cache), encoding="utf-8")

        # This is the exact fallback used by run_ps2_game.bat:
        # the saved DLL needs a host relink, but valid IR/objects remain.
        run_game.validate_preserved_translation(
            cache, next_host, next_guest, 0, metadata_path, object_dir
        )
        assert ir.read_text(encoding="utf-8") == "; synthetic verified IR\n"

        # A genuinely changed guest lowering header must still fail closed.
        (source_root / "include/ee/guest_lifter_input.h").write_bytes(
            b"changed guest IR source"
        )
        changed_guest = run_game.ir_generation_inputs(elf, source_root)
        assert changed_guest != initial_guest
        try:
            run_game.validate_preserved_translation(
                cache, next_host, changed_guest, 0, metadata_path, object_dir
            )
        except ValueError as error:
            assert "guest IR generation input changed:" in str(error)
        else:
            raise AssertionError("changed guest IR source unexpectedly reused")


if __name__ == "__main__":
    verify_real_probe_location()
    verify_preservation_when_host_only_inputs_change()
    print("PASS: host-only probes relink without invalidating guest translation")
