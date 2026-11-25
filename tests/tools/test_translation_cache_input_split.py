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
        shared_fmac_key = "src/runtime/ee_vu_fmac4_ir.h"
        assert shared_fmac_key in host, "shared SIMD source must fingerprint the native host"
        assert shared_fmac_key in guest, "VU0 consumes the shared SIMD lowering template"
        assert guest[shared_fmac_key] == host[shared_fmac_key]
        assert "src/llvm/ee_vu_fmac4_ir.h" not in guest


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
            "tools/native_alu_certificate.py": b"certificate",
            "src/ee/ee_dispatcher.cpp": b"dispatch",
            "src/ee/recompiled_functions.cpp": b"dispatch map",
            "src/ee/decoder.cpp": b"decoder",
            "src/tools/ee_recomp_main.cpp": b"recomp front-end",
            "include/ee/guest_lifter_input.h": b"guest header",
            "src/runtime/ee_sdbz_pad_return_probe.h": b"first host probe",
            "src/runtime/ee_vu_fmac4_ir.h": b"shared FMAC lowering",
        }
        for name, content in paths.items():
            path = source_root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

        initial_host = run_game.translation_inputs(elf, recomp, source_root)
        initial_guest = run_game.ir_generation_inputs(elf, source_root)
        from unittest.mock import patch
        import os
        with patch.dict(os.environ, {"SDBZ_NATIVE_ALU_CERTIFICATES": "0"}):
            disabled = run_game.translation_inputs(elf, recomp, source_root)
        with patch.dict(os.environ, {"SDBZ_NATIVE_ALU_CERTIFICATES": "1"}):
            enabled = run_game.translation_inputs(elf, recomp, source_root)
            assert run_game.ir_generation_inputs(elf, source_root) == initial_guest
        assert {key for key in enabled if enabled[key] != disabled[key]} == {
            "native_alu_certificate_mode"
        }
        assert "tools/native_alu_certificate.py" in initial_host
        assert "tools/native_alu_certificate.py" not in initial_guest
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

        # This is the exact fallback used by scripts/run_ps2_game.bat:
        # the saved DLL needs a host relink, but valid IR/objects remain.
        run_game.validate_preserved_translation(
            cache, next_host, next_guest, 0, metadata_path, object_dir
        )
        assert ir.read_text(encoding="utf-8") == "; synthetic verified IR\n"

        # A shared runtime header actually consumed by the EE emitter
        # must invalidate guest IR, unlike an unrelated host-only probe.
        shared_fmac = source_root / "src/runtime/ee_vu_fmac4_ir.h"
        shared_fmac.write_bytes(b"changed shared FMAC lowering")
        changed_fmac = run_game.ir_generation_inputs(elf, source_root)
        assert changed_fmac != initial_guest
        try:
            run_game.validate_preserved_translation(
                cache, next_host, changed_fmac, 0, metadata_path, object_dir
            )
        except ValueError as error:
            assert "guest IR generation input changed:" in str(error)
            assert "ee_vu_fmac4_ir.h" in str(error)
        else:
            raise AssertionError("changed shared FMAC template unexpectedly reused")
        shared_fmac.write_bytes(paths["src/runtime/ee_vu_fmac4_ir.h"])

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


def verify_ir_directory_scan():
    from unittest.mock import patch

    with tempfile.TemporaryDirectory(prefix="ee-ir-scan-") as directory:
        root = Path(directory).resolve()
        ir = root / "ir"
        ir.mkdir()
        functions = []
        for i in range(128):
            path = ir / f"fn_{i:04d}.ll"
            path.write_bytes(f"; module {i}\n".encode())
            functions.append({"ir_path": str(path)})
        expected = {Path(fn["ir_path"]).name:
                    run_game.sha256_path(Path(fn["ir_path"])) for fn in functions}
        original_resolve = Path.resolve
        calls = []

        def counted_resolve(self, *args, **kwargs):
            calls.append(self)
            return original_resolve(self, *args, **kwargs)

        with patch.object(Path, "resolve", counted_resolve):
            assert run_game.hash_ir_modules(
                ir, functions, require_owned_directory=True) == expected
        assert calls == [ir], calls
        first = Path(functions[0]["ir_path"])
        first.write_bytes(b"changed IR")
        assert run_game.hash_ir_modules(
            ir, functions, require_owned_directory=True) != expected

        def rejected(entries):
            try:
                run_game.hash_ir_modules(ir, entries, require_owned_directory=True)
            except (ValueError, OSError):
                return
            raise AssertionError("invalid IR ownership accepted")

        rejected(functions + [functions[0]])
        extra = ir / "extra.ll"
        extra.write_bytes(b"unowned")
        rejected(functions)
        extra.unlink()
        outside = root / first.name
        outside.write_bytes(first.read_bytes())
        rejected([{"ir_path": str(outside)}, *functions[1:]])
        first.unlink()
        rejected(functions)
        try:
            first.symlink_to(outside)
        except (OSError, NotImplementedError):
            pass
        else:
            rejected(functions)


if __name__ == "__main__":
    verify_ir_directory_scan()
    verify_real_probe_location()
    verify_preservation_when_host_only_inputs_change()
    print("PASS: host-only probes relink without invalidating guest translation")
