#!/usr/bin/env python3
"""Build and execute a bounded slice of a PS2 EE ELF through native code.

ee_recomp -> LLVM IR -> host objects -> link_native_objects -> ee_game_runner.
No BIOS, IOP, GS, DMA or general MMIO emulation is implied.

Examples:
  python tools/run_game.py SLUS_214.42 --build-dir build --trace
  python tools/run_game.py SLUS_214.42 --hle-startup --auto-expand=16 --trace
  python tools/run_game.py SLUS_214.42 --extra-functions=0x17f018,0x17edc8
  python tools/run_game.py SLUS_214.42 --functions=all --work-dir=build/sdbz
"""
import argparse
import functools
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

# This driver is also loaded by path using importlib in tests/tools.
# Resolve its sibling helpers for both script and module entry points.
_tools_path = str(Path(__file__).resolve().parent)
if _tools_path not in sys.path:
    sys.path.insert(0, _tools_path)
import shared_ee_ir as shared_ir


STOP_PATTERN = re.compile(
    r"EE stopped: dispatched=(\d+) pc=0x([0-9a-fA-F]{8}) "
    r"reason=DISPATCH\(3\) status=0"
)



CACHE_SCHEMA = "ee-translation-cache-v1"


def ir_pipeline_id(profile):
    # Keep this provenance identifier in sync with verify_ir.py.
    return "llvm-default-O2-v1" if profile == 4 else None


def native_target_identity(profile):
    if profile != 5:
        return None
    from verify_ir import native_target_identity as target_identity
    return target_identity(profile)


def sha256_path(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def translation_inputs(elf, recomp, source_root):
    """Fingerprint the ELF and every local input that builds translated code."""
    paths = [
        ("ELF", elf),
        ("ee_recomp", recomp),
        ("tools/run_game.py", source_root / "tools" / "run_game.py"),
        ("tools/verify_ir.py", source_root / "tools" / "verify_ir.py"),
        ("tools/shared_ee_ir.py", source_root / "tools" / "shared_ee_ir.py"),
        ("tools/link_native_objects.py",
         source_root / "tools" / "link_native_objects.py"),
        ("src/ee/ee_dispatcher.cpp",
         source_root / "src" / "ee" / "ee_dispatcher.cpp"),
        ("src/ee/recompiled_functions.cpp",
         source_root / "src" / "ee" / "recompiled_functions.cpp"),
    ]
    for relative in ("include/ee", "src/runtime"):
        directory = source_root / relative
        for path in sorted(directory.rglob("*")):
            if path.is_file() and path.suffix in (".h", ".hpp", ".cpp"):
                paths.append((path.relative_to(source_root).as_posix(), path))
    return {name: sha256_path(path) for name, path in paths}



def ir_generation_inputs(elf, source_root):
    """Separate guest lowering source fingerprints from host-only runtime code."""
    paths = [("ELF", elf)]
    ir_ee_sources = {
        "decoder.cpp", "opcode_tables.cpp",
        "function_analyzer.cpp", "ee_recomp.cpp"
    }
    for directory in ("include/ee", "src/ee", "src/elf",
                      "src/lift", "src/llvm"):
        for item in sorted((source_root / directory).rglob("*")):
            if not item.is_file() or item.suffix not in (".h", ".hpp", ".cpp"):
                continue
            if directory == "src/ee" and item.suffix == ".cpp" and (
                item.name not in ir_ee_sources
            ):
                # Dispatch glue and runtime linking do not affect IR text.
                continue
            paths.append((item.relative_to(source_root).as_posix(), item))
    paths.append((
        "src/tools/ee_recomp_main.cpp",
        source_root / "src/tools/ee_recomp_main.cpp"
    ))
    return {name: sha256_path(item) for name, item in paths}


def hash_ir_modules(ir_dir, functions, *, require_owned_directory=False):
    """Hash every module, resolving the ordinary directory only once.

    Windows Path.resolve(strict=True) opens each individual file to
    obtain its final path. For regular files directly inside the already
    canonical directory, one scandir supplies the same ownership check.
    Unusual paths and symlinks retain strict resolution.
    """
    if not isinstance(functions, list) or not functions:
        raise ValueError("cached translation has no functions")
    ir_dir = Path(ir_dir).resolve(strict=True)
    with os.scandir(ir_dir) as scan:
        entries = {entry.name: entry for entry in scan
                   if entry.name.endswith(".ll")}
    hashes = {}
    for function in functions:
        module = Path(function["ir_path"])
        if not module.is_absolute() and len(module.parts) == 1:
            module = ir_dir / module
        entry = entries.get(module.name)
        if (module.is_absolute() and module.parent == ir_dir and
                entry is not None and entry.is_file(follow_symlinks=False)):
            # Windows DirEntry.stat does not require another syscall for
            # ordinary files. Resolve any reparse point conservatively.
            if os.name == "nt" and (
                    entry.stat(follow_symlinks=False).st_file_attributes & 0x400):
                module = module.resolve(strict=True)
            else:
                module = ir_dir / entry.name
        else:
            module = shared_ir.module_path(str(module), ir_dir).resolve(strict=True)
        if require_owned_directory and (
                module.parent != ir_dir or module.suffix != ".ll"):
            raise ValueError("function references an unexpected IR path")
        if module.name in hashes:
            raise ValueError(f"duplicate cached IR filename: {module.name}")
        # Full content authentication, never trust timestamps or size.
        hashes[module.name] = sha256_path(module)
    if require_owned_directory and set(hashes) != set(entries):
        raise ValueError("unowned IR modules in cache directory")
    return hashes


def translated_ir_hashes(metadata_path):
    """Record the exact translated modules owned by the function map."""
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    return hash_ir_modules(metadata_path.parent, metadata.get("functions"),
                           require_owned_directory=shared_ir.active(metadata_path))


def recover_append_only_metadata(snapshot, metadata_path):
    """Prove a crash left only an appended function-map suffix.

    Compare the SHA-256 of the *reconstructed previous JSON bytes* with
    the saved cache. This authenticates every original function entry,
    block ownership and all other metadata, not just module filenames.
    Never accept a changed map, reordered entry or unknown provenance.
    The recovered suffix must still be retranslated and verified before
    it is allowed into the native dispatcher.
    """
    old_ir = snapshot.get("ir_hashes")
    if not isinstance(old_ir, dict) or not old_ir:
        raise ValueError("preserved function metadata was modified")
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    functions = metadata.get("functions")
    previous_count = len(old_ir)
    if (not isinstance(functions, list) or
            not previous_count < len(functions) <= previous_count + 128 or
            metadata.get("function_count") != len(functions)):
        raise ValueError("preserved function metadata was modified")

    reconstructed = dict(metadata)
    reconstructed["functions"] = functions[:previous_count]
    reconstructed["function_count"] = previous_count
    # batch_append_functions() writes JSON using Path.write_text().
    # On Windows this text-mode write converts every LF to CRLF; on
    # Linux it leaves LF unchanged. The cache hashes on-disk bytes,
    # not normalized JSON, so reconstruct BOTH exact byte spellings.
    # Requiring the saved SHA-256 for either spelling keeps the
    # complete original map authenticated without relaxing provenance.
    original_text = json.dumps(reconstructed, indent=2) + "\n"
    original_lf = original_text.encode("utf-8")
    original_crlf = original_text.replace("\n", "\r\n").encode("utf-8")
    expected_digest = snapshot.get("metadata_sha256")
    if not any(
        hashlib.sha256(candidate).hexdigest() == expected_digest
        for candidate in (original_lf, original_crlf)
    ):
        raise ValueError(
            "preserved function metadata was modified "
            "(not a verified append-only extension; "
            "tried both LF and Windows CRLF JSON serialization)"
        )
    # A single appended transaction may be interrupted after publication
    # but before its object emission, link, or cache record is committed.
    suffix = functions[previous_count:]
    if len(set(item.get("host_symbol") for item in suffix)) != len(suffix):
        raise ValueError("interrupted append has duplicate host symbols")
    return suffix


def verify_recovered_append(recomp, elf, work, metadata_path, suffix):
    """Re-lift only appended guest entries and require bit-identical IR.

    The old function map is authenticated by its cached metadata hash;
    suffix entries are trusted only if a fresh translation of the same
    ELF produces the same symbol, function metadata, block ownership
    and SHA-256 of each .ll module. Never alter the original cached IR.
    """
    if not suffix:
        return
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    count = len(metadata["functions"]) - len(suffix)
    owned = covered_pcs({"functions": metadata["functions"][:count]})
    expected_addresses = []
    expected_symbols = set()
    destination_dir = metadata_path.parent.resolve()
    for function in suffix:
        address = int(function["vaddr"], 0)
        if (address == 0 or address & 3 or address in owned or
                address in expected_addresses or
                function["host_symbol"] in expected_symbols):
            raise ValueError("interrupted append contains invalid guest entry")
        expected_addresses.append(address)
        expected_symbols.add(function["host_symbol"])
        module = shared_ir.module_path(function["ir_path"], destination_dir).resolve(strict=True)
        if module.parent != destination_dir or module.suffix != ".ll":
            raise ValueError("interrupted append references unexpected IR path")

    with tempfile.TemporaryDirectory(
            prefix="ee-verify-interrupted-append-") as temporary:
        output = Path(temporary)
        run([
            recomp, elf, output,
            "--functions=" + ",".join(
                f"0x{address:08x}" for address in expected_addresses
            ),
        ])
        recovered = json.loads(
            (output / "ee_functions.json").read_text(encoding="utf-8")
        )
        generated = {
            int(item["vaddr"], 0): item
            for item in recovered.get("functions", [])
        }
        if len(generated) != len(suffix):
            raise ValueError("interrupted append retranslation count mismatch")
        for old in suffix:
            address = int(old["vaddr"], 0)
            fresh = generated.get(address)
            if fresh is None:
                raise ValueError(
                    f"interrupted append entry 0x{address:08x} not retranslated"
                )
            fresh = dict(fresh)
            fresh_path = shared_ir.module_path(fresh.pop("ir_path"), output).resolve(strict=True)
            expected_path = shared_ir.module_path(old["ir_path"], destination_dir).resolve(strict=True)
            fresh["block_pcs"] = [
                pc for pc in fresh["block_pcs"]
                if int(pc, 0) not in owned
            ]
            expected = {key: value for key, value in old.items()
                        if key != "ir_path"}
            if (fresh != expected or
                    fresh_path.name != expected_path.name or
                    sha256_path(fresh_path) != sha256_path(expected_path)):
                raise ValueError(
                    f"interrupted append entry 0x{address:08x} "
                    "does not match a fresh guest translation"
                )
            if address not in {int(pc, 0) for pc in fresh["block_pcs"]}:
                raise ValueError(
                    "interrupted append lost its guest function entry"
                )
            owned.update(int(pc, 0) for pc in fresh["block_pcs"])
    print(
        f"run_game: verified {len(suffix)} interrupted guest append(s) "
        "against fresh ELF translation; original IR preserved",
        flush=True,
    )


def validate_preserved_translation(path, inputs, ir_sources,
                                   emit_opt, metadata_path, object_dir):
    """Never relink modified guest IR or an untrusted function map.

    Historical v1 cache records can be migrated only when the exact ELF
    and original ee_recomp executable are unchanged.
    """
    if not path.is_file() or not metadata_path.is_file():
        raise ValueError("preserved translation metadata/cache missing")
    snapshot = json.loads(path.read_text(encoding="utf-8"))
    if snapshot.get("schema") != CACHE_SCHEMA:
        raise ValueError("preserved translation has unknown provenance")
    if (snapshot.get("emit_opt") != emit_opt or
            snapshot.get("ir_pipeline") != ir_pipeline_id(emit_opt)):
        raise ValueError("preserved translation uses a different --emit-opt or IR pipeline")
    if snapshot.get("native_target") != native_target_identity(emit_opt):
        raise ValueError("preserved translation uses a different host CPU target")
    metadata_changed = (
        snapshot.get("metadata_sha256") != sha256_path(metadata_path)
    )
    saved_sources = snapshot.get("ir_sources")
    if saved_sources is not None:
        # A host-only SDBZ pad-return observer was briefly installed in
        # include/ee and therefore mistakenly fingerprinted as a guest
        # lowering input. It now lives in src/runtime. Permit ONLY this
        # exact byte-identical relocation; all other guest source keys
        # must still match, and verified IR/object hashes are checked below.
        old_host_key = "include/ee/ee_sdbz_pad_return_probe.h"
        new_host_key = "src/runtime/ee_sdbz_pad_return_probe.h"
        migrated_host_header = (
            isinstance(saved_sources, dict)
            and old_host_key in saved_sources
            and old_host_key not in ir_sources
            and saved_sources[old_host_key] == inputs.get(new_host_key)
            and {
                name: digest for name, digest in saved_sources.items()
                if name != old_host_key
            } == ir_sources
        )
        # Dispatch hotspot observations are also host-only. The two exact
        # historical header revisions (LF and CRLF checkouts) below never
        # participated in guest lowering. Only remove that single stale
        # fingerprint; all remaining generator and IR/object hashes stay strict.
        old_diagnostics_key = "include/ee/ee_hot_loop_diagnostics.h"
        known_diagnostics_digests = {
            "6dd93590f16ee27ce21d26bc6545e80186a1f2869b058c0139e2ab99af3bbbee",
            "d5c9345285e8a93958113d7d24d16c9d8131ced874765bb2a06c8761423017ad",
            "0cfeffaa89a2b3dc9b282b0ff214e64197bb4ce166a76e2b4bfe6c5c0b87767d",
            "a6305c7c8567a91048d3e8e841e20e1782c36fab8256942689e0a15aaf92f8bf",
        }
        migrated_diagnostics_header = (
            isinstance(saved_sources, dict)
            and isinstance(snapshot.get("inputs"), dict)
            and saved_sources.get(old_diagnostics_key) in known_diagnostics_digests
            and snapshot["inputs"].get(old_diagnostics_key) ==
                saved_sources[old_diagnostics_key]
            and old_diagnostics_key not in ir_sources
            and "src/runtime/ee_hot_loop_diagnostics.h" in inputs
            and {
                name: digest for name, digest in saved_sources.items()
                if name != old_diagnostics_key
            } == ir_sources
        )
        if (not isinstance(saved_sources, dict) or
                (saved_sources != ir_sources and not migrated_host_header
                 and not migrated_diagnostics_header)):
            changed = next((
                name for name in sorted(
                    set(saved_sources if isinstance(saved_sources, dict) else {}) |
                    set(ir_sources)
                )
                if (saved_sources.get(name) if isinstance(saved_sources, dict) else None)
                   != ir_sources.get(name)
            ), "unknown")
            raise ValueError(f"guest IR generation input changed: {changed}")
        if migrated_host_header:
            print(
                "run_game: migrated misplaced host-only pad probe "
                "fingerprint; preserving verified guest IR",
                flush=True
            )
        if migrated_diagnostics_header:
            print(
                "run_game: migrated host-only dispatch diagnostics "
                "fingerprint; preserving verified guest IR",
                flush=True
            )
    else:
        # Older snapshots did not separately fingerprint guest-generator
        # sources. Byte equality of the complete generator binary plus the
        # input ELF is sufficient for an unchanged translated-code result.
        old_inputs = snapshot.get("inputs", {})
        for name in ("ELF", "ee_recomp"):
            if not isinstance(old_inputs, dict) or old_inputs.get(name) != inputs.get(name):
                raise ValueError(
                    f"legacy guest IR cache cannot be reused: {name} changed"
                )

    # Normal reuse still requires exact metadata bytes. Interrupted auto-
    # expansion is the only exception: a verified, appended function suffix
    # may have been published before the next cache record was committed.
    current_ir = translated_ir_hashes(metadata_path)
    recorded_ir = snapshot.get("ir_hashes")
    if metadata_changed and shared_ir.verified_extension(
            snapshot, metadata_path, ir_sources, current_ir):
        # Another profile committed this shared map. Emit missing objects
        # incrementally; do not re-lift its already verified guest IR.
        print("run_game: synchronizing native profile to shared EE IR", flush=True)
        return [], False
    suffix = (
        recover_append_only_metadata(snapshot, metadata_path)
        if metadata_changed else []
    )
    if suffix:
        if (not isinstance(recorded_ir, dict) or
                any(current_ir.get(key) != digest
                    for key, digest in recorded_ir.items()) or
                len(current_ir) != len(recorded_ir) + len(suffix)):
            raise ValueError("preserved LLVM IR module was modified")
    elif recorded_ir is not None and recorded_ir != current_ir:
        raise ValueError("preserved LLVM IR module was modified")
    manifest_path = object_dir / "objects.json"
    if not manifest_path.is_file():
        raise ValueError("preserved object manifest missing")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (not manifest.get("complete") or
            manifest.get("optimization_level") != emit_opt or
            manifest.get("ir_pipeline") != ir_pipeline_id(emit_opt) or
            manifest.get("native_target") != native_target_identity(emit_opt)):
        raise ValueError("preserved object manifest incomplete or incompatible")
    objects = manifest.get("objects")
    if not isinstance(objects, list):
        raise ValueError("preserved object manifest count mismatch")
    expected = {Path(item["input"]).name: item.get("ir_sha256")
                for item in objects}
    if len(expected) != len(objects):
        raise ValueError("preserved object manifest has duplicate IR input")
    if suffix:
        # A crash may have occurred either before or after append-only
        # object emission. No intermediate or inconsistent manifest accepted.
        old_complete = (len(objects) == len(recorded_ir) and
                        expected == recorded_ir)
        new_complete = (len(objects) == len(current_ir) and
                        expected == current_ir)
        if not old_complete and not new_complete:
            raise ValueError("preserved object manifest does not match append")
        return suffix, new_complete
    if len(objects) != len(current_ir):
        raise ValueError("preserved object manifest count mismatch")
    if expected != current_ir:
        raise ValueError("preserved IR does not match compiled object manifest")
    return [], True


def archive_known_function_manifest(path, snapshot):
    """Save verified entry/hash snapshots outside the disposable guest cache.

    Immutable content-addressed manifests allow reconstruction without
    replaying function-discovery passes. No game assets or IR source are stored.
    """
    hashes = snapshot.get("ir_hashes")
    inputs = snapshot.get("inputs")
    if (not isinstance(hashes, dict) or not hashes or
            not isinstance(inputs, dict) or
            not isinstance(inputs.get("ELF"), str) or
            snapshot.get("emit_opt") not in range(6)):
        return
    # Preserve guest discovery/batch order independently of JSON key sorting.
    # Reconstructing the same address SET in a different order can change
    # overlapping block ownership and emitted IR hashes.
    archived = dict(snapshot)
    archived["known_entries"] = list(hashes)
    serialized = json.dumps(archived, sort_keys=True, indent=2) + "\n"
    digest = hashlib.sha256(serialized.encode("utf-8")).hexdigest()
    owner = path.parent
    # Nested native profiles share the root IR inventory; stage recoveries
    # inside the root also archive outside the movable cache contents.
    if ((owner / shared_ir.MARKER).is_file()):
        owner = shared_ir.ir_directory(owner).parent
    if owner.name.startswith("recovery-"):
        owner = owner.parent
    folder = owner.parent / "ee-known-manifests"
    folder.mkdir(parents=True, exist_ok=True)
    destination = folder / (
        f"ee-{inputs['ELF'][:12]}-{len(hashes):06d}-{digest[:16]}.json"
    )
    if destination.exists():
        if destination.read_text(encoding="utf-8") != serialized:
            raise ValueError("known-function manifest digest collision")
        return
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", newline="\n",
        dir=folder, prefix=".snapshot-", suffix=".tmp", delete=False
    ) as output:
        temporary = Path(output.name)
        output.write(serialized)
    try:
        if destination.exists() and destination.read_text(encoding="utf-8") != serialized:
            raise ValueError("known-function manifest digest collision")
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def write_translation_cache(path, inputs, emit_opt, metadata_path, library,
                            ir_sources=None):
    """Commit provenance only after the complete translated library is linked."""
    snapshot = {
        "schema": CACHE_SCHEMA,
        "inputs": inputs,
        "emit_opt": emit_opt,
        "ir_pipeline": ir_pipeline_id(emit_opt),
        "native_target": native_target_identity(emit_opt),
        "metadata_sha256": sha256_path(metadata_path),
        "library_sha256": sha256_path(library),
        "ir_sources": ir_sources,
        "ir_hashes": translated_ir_hashes(metadata_path),
    }
    if shared_ir.active(metadata_path):
        shared_ir.publish(metadata_path, ir_sources, snapshot["ir_hashes"])
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_text(json.dumps(snapshot, indent=2) + "\n",
                             encoding="utf-8")
        temporary.replace(path)
        archive_known_function_manifest(path, snapshot)
    finally:
        if temporary.exists():
            temporary.unlink()


def validate_cached_library_identity(path, inputs, emit_opt, metadata_path, library):
    """Check the saved native identity; this alone does NOT authorize execution."""
    if not path.is_file():
        raise ValueError(
            "cached translation has no provenance record; "
            "rerun without --no-recompile"
        )
    snapshot = json.loads(path.read_text(encoding="utf-8"))
    if snapshot.get("schema") != CACHE_SCHEMA:
        raise ValueError(
            "cached translation has an unrecognized provenance schema; "
            "rerun without --no-recompile"
        )
    saved = snapshot.get("inputs")
    if not isinstance(saved, dict):
        raise ValueError("cached translation has invalid input fingerprints")
    for name, digest in inputs.items():
        if saved.get(name) != digest:
            raise ValueError(
                f"cached translation is stale: {name} changed; "
                "rerun without --no-recompile"
            )
    if set(saved) != set(inputs):
        raise ValueError(
            "cached translation input list changed; "
            "rerun without --no-recompile"
        )
    if (snapshot.get("emit_opt") != emit_opt or
            snapshot.get("ir_pipeline") != ir_pipeline_id(emit_opt)):
        raise ValueError(
            "cached translation uses a different --emit-opt or IR pipeline; "
            "rerun without --no-recompile"
        )
    if snapshot.get("native_target") != native_target_identity(emit_opt):
        raise ValueError("cached translation uses a different host CPU target")
    for name, file_path in (("metadata_sha256", metadata_path),
                            ("library_sha256", library)):
        if snapshot.get(name) != sha256_path(file_path):
            raise ValueError(
                f"cached translation {file_path.name} changed; "
                "rerun without --no-recompile"
            )
    return snapshot


def validate_translation_cache(path, inputs, emit_opt, metadata_path, library,
                               *, verified_ir_hashes=None):
    """Authenticate native identity and current IR before execution."""
    started = time.monotonic()
    snapshot = validate_cached_library_identity(
        path, inputs, emit_opt, metadata_path, library)
    library_checked = time.monotonic()
    if shared_ir.active(metadata_path):
        current = (translated_ir_hashes(metadata_path) if verified_ir_hashes is None
                   else verified_ir_hashes)
        shared_ir.current_state(metadata_path, snapshot.get("ir_sources"), current)
        if current != snapshot.get("ir_hashes"):
            raise ValueError("cached native profile IR does not match shared source")
    elif verified_ir_hashes is not None:
        raise ValueError("preverified hashes require a shared EE IR store")
    ir_checked = time.monotonic()
    # A valid cache created before snapshot support is also preserved.
    archive_known_function_manifest(path, snapshot)
    print(f"EE cache validation: profile={emit_opt} "
          f"metadata/library={library_checked - started:.3f}s "
          f"shared_ir={ir_checked - library_checked:.3f}s "
          f"archive={time.monotonic() - ir_checked:.3f}s "
          f"ir_scan={'fresh' if verified_ir_hashes is None else 'already-verified'}",
          flush=True)



def elf_entry(path):
    with path.open("rb") as source:
        header = source.read(52)
    if len(header) < 52 or header[:4] != b"\x7fELF" or header[4] != 1:
        raise ValueError(f"{path} is not ELF32")
    if header[5] != 1 or struct.unpack_from("<H", header, 18)[0] != 8:
        raise ValueError(f"{path} is not a little-endian MIPS ELF")
    return struct.unpack_from("<I", header, 24)[0]


def executable_segments(path):
    image = path.read_bytes()
    if len(image) < 52:
        raise ValueError("truncated ELF32 header")
    program_offset = struct.unpack_from("<I", image, 28)[0]
    program_size = struct.unpack_from("<H", image, 42)[0]
    program_count = struct.unpack_from("<H", image, 44)[0]
    if program_size < 32 or program_offset + program_size * program_count > len(image):
        raise ValueError("invalid ELF32 program header table")

    ranges = []
    for index in range(program_count):
        offset = program_offset + program_size * index
        kind, _, va, _, _, memory_size, flags, _ = struct.unpack_from(
            "<IIIIIIII", image, offset
        )
        if kind == 1 and flags & 1 and memory_size > 0:
            ranges.append((va, va + memory_size))
    return ranges


def _elf_cache_key(elf_path, executable_ranges):
    path = Path(elf_path).resolve()
    stat = path.stat()
    return (str(path), stat.st_size, stat.st_mtime_ns,
            tuple(tuple(item) for item in executable_ranges))


@functools.lru_cache(maxsize=4)
def _pointer_runs(path, size, mtime_ns, executable_ranges):
    """Every maximal run of executable pointer words with >= 4 distinct values.

    Returns (runs, {pc: [run index, ...]}) in scan order: exactly the
    candidates pointer_table_siblings() previously rebuilt on every call.
    """
    image = Path(path).read_bytes()
    if len(image) < 52 or image[:4] != b"\x7fELF" or image[4:6] != b"\x01\x01":
        raise ValueError("function-pointer discovery requires little-endian ELF32")
    program_offset = struct.unpack_from("<I", image, 28)[0]
    program_size = struct.unpack_from("<H", image, 42)[0]
    program_count = struct.unpack_from("<H", image, 44)[0]
    if program_size < 32 or program_offset + program_size * program_count > len(image):
        raise ValueError("invalid ELF32 program header table")

    def executable(pc):
        return pc != 0 and (pc & 3) == 0 and any(
            start <= pc < end for start, end in executable_ranges
        )

    runs = []
    by_pc = {}

    def consider(run):
        if len(run) < 4 or len(set(run)) < 4:
            return
        index = len(runs)
        runs.append(tuple(run))
        for pc in set(run):
            by_pc.setdefault(pc, []).append(index)

    for segment in range(program_count):
        header = program_offset + program_size * segment
        kind, file_offset, virtual_address, _, file_size, _, _, _ = (
            struct.unpack_from("<IIIIIIII", image, header)
        )
        if kind != 1 or file_size < 16:
            continue
        if file_offset + file_size > len(image):
            raise ValueError("ELF PT_LOAD exceeds file bounds")
        first = (-virtual_address) & 3
        words = struct.unpack_from(
            f"<{(file_size - first) // 4}I", image, file_offset + first
        )
        run = []
        for candidate in words:
            if executable(candidate):
                run.append(candidate)
            else:
                consider(run)
                run = []
        consider(run)
    return runs, by_pc


def pointer_table_siblings(elf_path, target, executable_ranges, limit):
    """Prefetch code pointers from a table containing a guest-reached PC.

    The ELF may mix data and code in one executable PT_LOAD segment. Do
    not treat every pointer-shaped word as a function. A table is trusted
    only after real guest execution reaches one of its entries and at
    least four distinct executable targets occur in a contiguous array.
    """
    if limit <= 0 or target & 3:
        return []

    # The whole-image run scan is identical for every miss in a session;
    # compute it once per (ELF, executable ranges), then look up the target.
    runs, by_pc = _pointer_runs(*_elf_cache_key(elf_path, executable_ranges))
    selected = None
    best_score = None
    for index in by_pc.get(target, ()):
        run = runs[index]
        score = (len(set(run)), -len(run))
        if best_score is None or score > best_score:
            selected = list(run)
            best_score = score

    if selected is None:
        return []

    # Prefer neighbors in the same runtime-confirmed pointer array,
    # expanding both directions before considering more distant entries.
    anchor = selected.index(target)
    order = sorted(range(len(selected)), key=lambda i: (abs(i - anchor), i))
    siblings = []
    seen = {target}
    for index in order:
        pc = selected[index]
        if pc in seen:
            continue
        seen.add(pc)
        siblings.append(pc)
        if len(siblings) == limit:
            break
    return siblings



@functools.lru_cache(maxsize=4)
def _jump_tables(path, size, mtime_ns, executable_ranges):
    """Every guarded MIPS JR jump table whose words are all executable.

    Scan order is preserved; branch_anchored_jump_table_siblings() returns
    the first table containing the guest-reached target, as it always did.
    """
    tables = []
    image = Path(path).read_bytes()
    if len(image) < 52 or image[:6] != b"\x7fELF\x01\x01":
        raise ValueError("jump-table discovery requires little-endian ELF32")
    header_offset = struct.unpack_from("<I", image, 28)[0]
    header_size = struct.unpack_from("<H", image, 42)[0]
    header_count = struct.unpack_from("<H", image, 44)[0]
    if header_size < 32 or header_offset + header_size * header_count > len(image):
        raise ValueError("invalid ELF32 program header table")

    segments = []
    for index in range(header_count):
        header = header_offset + header_size * index
        kind, offset, vaddr, _, file_size, _, flags, _ = struct.unpack_from(
            "<IIIIIIII", image, header
        )
        if kind != 1 or not file_size:
            continue
        if offset + file_size > len(image):
            raise ValueError("ELF PT_LOAD exceeds file bounds")
        segments.append((offset, vaddr, file_size, flags))

    def table_words(address, count):
        for file_offset, vaddr, size, _ in segments:
            if address >= vaddr and count <= (size // 4) and (
                address - vaddr <= size - 4 * count
            ):
                offset = file_offset + address - vaddr
                return struct.unpack_from(f"<{count}I", image, offset)
        return None

    def executable(pc):
        return pc != 0 and not (pc & 3) and any(
            start <= pc < end for start, end in executable_ranges
        )

    def signed16(word):
        value = word & 0xffff
        return value - 0x10000 if value & 0x8000 else value

    for file_offset, vaddr, size, flags in segments:
        if not (flags & 1) or size < 36:
            continue
        # Include the guard and the MIPS JR, but never treat a delay-slot
        # instruction as a second instruction in the dispatch sequence.
        for offset in range((-vaddr) & 3, size - 3, 4):
            if offset < 32:
                continue
            jr = struct.unpack_from("<I", image, file_offset + offset)[0]
            if (jr & 0xfc1fffff) != 0x00000008:
                continue
            index_register = (jr >> 21) & 31
            if index_register in (0, 31):
                continue
            (bound, guard, delay, upper, shift, lower, add,
             load) = struct.unpack_from("<8I", image, file_offset + offset - 32)
            guard_register = (bound >> 16) & 31
            count = bound & 0xffff
            base_register = (upper >> 16) & 31
            # The bounds branch may save an argument in its delay slot.
            # DADDU rd, rs, zero is a nontrapping copy. Only the checked
            # index must survive until SLL; LUI initializes the table base.
            delay_copies_register = (
                (delay & 0xfc1f07ff) == 0x2d and
                ((delay >> 11) & 31) != index_register
            )
            if not (
                (bound >> 26) == 0x0b and
                ((bound >> 21) & 31) == index_register and
                guard_register not in (0, index_register) and 2 <= count <= 128 and
                (guard >> 26) == 0x04 and
                ((guard >> 21) & 31) == guard_register and
                ((guard >> 16) & 31) == 0 and
                (delay == 0 or delay_copies_register) and
                (upper >> 26) == 0x0f and base_register not in (0, index_register) and
                (shift >> 26) == 0 and (shift & 63) == 0 and
                ((shift >> 21) & 31) == 0 and
                ((shift >> 16) & 31) == index_register and
                ((shift >> 11) & 31) == index_register and
                ((shift >> 6) & 31) == 2 and
                (lower >> 26) == 0x09 and
                ((lower >> 21) & 31) == base_register and
                ((lower >> 16) & 31) == base_register and
                (add >> 26) == 0 and (add & 63) == 0x21 and
                ((add >> 11) & 31) == index_register and
                {((add >> 21) & 31), ((add >> 16) & 31)} ==
                {index_register, base_register} and
                (load >> 26) == 0x23 and
                ((load >> 21) & 31) == index_register and
                ((load >> 16) & 31) == index_register and
                (load & 0xffff) == 0
            ):
                continue

            jr_pc = vaddr + offset
            guard_pc = jr_pc - 28
            guard_target = (guard_pc + 4 + 4 * signed16(guard)) & 0xffffffff
            # The failed bounds check must bypass the indexed dispatch.
            if jr_pc - 20 <= guard_target <= jr_pc + 4:
                continue
            table_base = (((upper & 0xffff) << 16) +
                          signed16(lower)) & 0xffffffff
            words = table_words(table_base, count)
            if words is None or not all(
                executable(candidate) for candidate in words
            ):
                continue
            tables.append(words)
    return tuple(tables)


def branch_anchored_jump_table_siblings(elf_path, target, executable_ranges, limit):
    """Find a small bounded switch only through its actual MIPS JR consumer.

    Unlike generic function-pointer discovery, this requires the compiler's
    guarded indexed dispatch sequence: SLTIU/BEQ, LUI/SLL/ADDIU/ADDU,
    LW and JR. The SLTIU bound supplies the *exact* table size. A reachable
    target must occur inside that file-backed table; otherwise no pointers
    are promoted to native entries. Do not weaken pointer_table_siblings().
    """
    if limit <= 0 or target == 0 or target & 3 or not any(
        start <= target < end for start, end in executable_ranges
    ):
        return []

    for words in _jump_tables(*_elf_cache_key(elf_path, executable_ranges)):
        if target not in words:
            continue
        count = len(words)
        anchor = words.index(target)
        order = sorted(range(count), key=lambda i: (abs(i - anchor), i))
        siblings = []
        seen = {target}
        for index in order:
            candidate = words[index]
            if candidate not in seen:
                seen.add(candidate)
                siblings.append(candidate)
                if len(siblings) >= limit:
                    break
        return siblings
    return []


def parse_address(value):
    number = int(value, 0)
    if not 0 <= number <= 0xFFFFFFFF or number & 3:
        raise ValueError(f"unaligned or out-of-range guest PC: {value}")
    return number


def parse_trace_v0(value):
    parts = value.split(":")
    if len(parts) != 2:
        raise argparse.ArgumentTypeError("--trace-v0 requires START:END")
    try:
        first, last = (parse_address(part) for part in parts)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    if first > last or last - first > 0x2000:
        raise argparse.ArgumentTypeError("--trace-v0 needs an ordered range of at most 0x2000 bytes")
    return f"0x{first:08x}:0x{last:08x}"


def run(command, capture=False, timeout_seconds=None):
    print("+ " + subprocess.list2cmdline([str(item) for item in command]), flush=True)
    result = subprocess.run(
        [str(item) for item in command],
        text=True,
        capture_output=capture,
        check=not capture,
        timeout=timeout_seconds,
    )
    if capture:
        if result.stdout:
            print(result.stdout, end="", flush=True)
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr, flush=True)
    return result


def native_executable(build_dir, name, config="Release"):
    suffix = ".exe" if os.name == "nt" else ""
    cache = build_dir / "CMakeCache.txt"
    multiconfig = False
    if cache.is_file():
        multiconfig = any(
            line.startswith("CMAKE_CONFIGURATION_TYPES:")
            for line in cache.read_text(encoding="utf-8", errors="replace").splitlines()
        )

    # Visual Studio and other multi-config generators write to
    # build/Release (or build/Debug), not the build directory itself.
    # Never accidentally use an older root executable in that case.
    executable = (build_dir / config / (name + suffix) if multiconfig
                  else build_dir / (name + suffix))
    if not executable.is_file():
        raise ValueError(
            f"required build target not found: {executable}\n"
            f"Run cmake --build {build_dir} --config {config} "
            "--target ee_recomp ee_game_runner"
        )
    return executable


def covered_pcs(metadata):
    return {
        int(pc, 0)
        for item in metadata.get("functions", [])
        for pc in item.get("block_pcs", [])
    }


def batch_append_functions(recomp, elf, work, metadata_path, addresses):
    """Translate a group of newly reachable entries in one discovery pass.

    The EE CFG emitter writes one IR/metadata result per requested function.
    Publish a single merged metadata update only after validating all
    translations and preserving existing guest block-PC ownership.
    Return the exact NEW IR paths for incremental object emission.
    """
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    shared_state = None
    if shared_ir.active(metadata_path):
        recorded = shared_ir.read_json(metadata_path.parent / shared_ir.PROVENANCE)
        shared_state = shared_ir.current_state(metadata_path, recorded["ir_sources"])
    owned = covered_pcs(metadata)
    targets = [
        address for address in dict.fromkeys(addresses)
        if address not in owned
    ]
    if not targets:
        return []
    if len(targets) > 128:
        raise ValueError("batched expansion exceeds the 128-entry limit")
    if any(address == 0 or (address & 3) for address in targets):
        raise ValueError("batched expansion contains unaligned/zero PC")

    # Only guest-proven first entries are mandatory. Later speculative
    # table siblings may be rejected, but must never block the first.
    output_dir = work / "extra" / ("batch_" + f"{targets[0]:08x}")
    command = [
        recomp, elf, output_dir,
        "--functions=" + ",".join(f"0x{pc:08x}" for pc in targets),
    ]
    discovery_cache = metadata_path.parent / "ee_discovery_cache.bin"
    if discovery_cache.is_file():
        command.append(f"--discovery-cache={discovery_cache}")
    run(command)
    extra_metadata = json.loads(
        (output_dir / "ee_functions.json").read_text(encoding="utf-8")
    )
    raw_functions = extra_metadata.get("functions")
    if not isinstance(raw_functions, list):
        raise ValueError("batch recompiler emitted invalid function metadata")
    generated = {}
    for function in raw_functions:
        address = int(function["vaddr"], 0)
        if address not in targets or address in generated:
            raise ValueError(f"batch recompiler emitted unexpected/duplicate entry 0x{address:08x}")
        generated[address] = function

    pending = []
    published_owners = set(owned)
    published_names = {Path(item["ir_path"]).name
                       for item in metadata["functions"]}
    for address in targets:
        function = generated.get(address)
        if function is None:
            message = f"no unique translation for 0x{address:08x}"
            if address == targets[0]:
                raise ValueError(message)
            print(f"auto-expand: ignoring speculative table sibling "
                  f"0x{address:08x}: {message}", file=sys.stderr)
            continue
        blocks = function.get("block_pcs")
        if not isinstance(blocks, list):
            raise ValueError(f"function 0x{address:08x} has invalid block map")
        new_pcs = [int(pc, 0) for pc in blocks]
        if len(set(new_pcs)) != len(new_pcs) or address not in new_pcs:
            message = f"translation does not uniquely cover 0x{address:08x}"
            if address == targets[0]:
                raise ValueError(message)
            print(f"auto-expand: ignoring speculative table sibling "
                  f"0x{address:08x}: {message}", file=sys.stderr)
            continue
        overlap = set(new_pcs) & published_owners
        owned_blocks = [pc for pc in blocks if int(pc, 0) not in overlap]
        if address in overlap or not owned_blocks:
            message = f"entry 0x{address:08x} already owned by another function"
            if address == targets[0]:
                raise ValueError(message)
            print(f"auto-expand: ignoring speculative table sibling "
                  f"0x{address:08x}: {message}", file=sys.stderr)
            continue
        if overlap:
            print(
                f"auto-expand: preserving existing ownership of "
                f"{len(overlap)} overlapping guest block PC(s) "
                f"while translating entry 0x{address:08x}",
                flush=True,
            )
        source = shared_ir.module_path(function["ir_path"], output_dir)
        destination = metadata_path.parent / source.name
        if (destination.exists() or
            source.name in published_names):
            raise ValueError(f"IR module filename collision: {destination}")
        if not source.is_file():
            raise ValueError(f"missing batched IR module: {source}")
        function["block_pcs"] = owned_blocks
        function["ir_path"] = destination.name
        pending.append((source, destination, function))
        published_owners.update(int(pc, 0) for pc in owned_blocks)
        published_names.add(source.name)

    if not pending:
        raise ValueError("batched expansion yielded no dispatchable entries")

    # Stage all new modules before writing the authoritative function map.
    # A partial error cannot make metadata point at missing/misnamed IR.
    staged = []
    committed = []
    try:
        for source, destination, _ in pending:
            temporary = destination.with_name(destination.name + ".tmp")
            shutil.copyfile(source, temporary)
            staged.append((temporary, destination))
        for temporary, destination in staged:
            temporary.replace(destination)
            committed.append(destination)
        metadata["functions"].extend(
            function for _, _, function in pending
        )
        metadata["function_count"] = len(metadata["functions"])
        tmp_meta = metadata_path.with_name(metadata_path.name + ".tmp")
        tmp_meta.write_text(json.dumps(metadata, indent=2) + "\n",
                            encoding="utf-8")
        tmp_meta.replace(metadata_path)
    except (OSError, ValueError):
        for temporary, _ in staged:
            temporary.unlink(missing_ok=True)
        for destination in committed:
            destination.unlink(missing_ok=True)
        raise

    if shared_state is not None:
        hashes = dict(shared_state["ir_hashes"])
        hashes.update({path.name: sha256_path(path) for path in committed})
        shared_ir.publish(metadata_path, shared_state["ir_sources"], hashes)

    print(f"auto-expand: appended {len(pending)} function(s) in one "
          f"discovery and metadata transaction", flush=True)
    return [destination for _, destination, _ in pending]

def can_relink_verified_objects(cache_path, inputs, ir_sources, emit_opt,
                                metadata_path, object_dir):
    """Avoid a redundant emission pass after full preserved-IR verification.

    The linker still reads and hashes every guest object before linking.
    Retain the old incremental path for legacy provenance, incompatible
    LLVM/emitter versions, or a changed function map.
    """
    try:
        snapshot = shared_ir.read_json(cache_path)
        if (snapshot.get("ir_sources") != ir_sources or
                snapshot.get("metadata_sha256") != sha256_path(metadata_path) or
                snapshot.get("inputs", {}).get("tools/verify_ir.py") !=
                    inputs.get("tools/verify_ir.py")):
            return False
        from verify_ir import append_cached_objects, llvm
        records = append_cached_objects(object_dir, emit_opt,
                                        llvm.get_default_triple(), [])
        metadata = shared_ir.read_json(metadata_path)
        # Exact path equality is sufficient for the common case. Only
        # canonicalize individual paths if their spellings differ.
        module_paths = {shared_ir.module_path(fn["ir_path"], metadata_path.parent)
                        for fn in metadata["functions"]}
        record_paths = {Path(name) for name in records}
        same_paths = module_paths == record_paths or (
            {p.resolve() for p in module_paths} ==
            {p.resolve() for p in record_paths})
        return (same_paths and
                {Path(name).name: item["ir_sha256"] for name, item in records.items()} ==
                snapshot.get("ir_hashes"))
    except (OSError, ValueError, KeyError, TypeError, ImportError):
        return False


def build_linked_library(source_root, work, metadata_path, object_dir, library,
                         emit_opt, pdb=False, reuse_objects=False, emit_jobs=2,
                         appended_ir=None, cache_base=None, emit_only=False):
    if not reuse_objects:
        # Full verification is mandatory on the initial build. An append
        # pass has already validated the existing provenance and only new
        # modules need LLVM parsing/emission. The native linker still hashes
        # ALL cached guest objects before linking the combined DLL.
        input_files = (list(appended_ir) if appended_ir is not None
                       else [str(metadata_path.parent / "*.ll")])
        command = [
            sys.executable, source_root / "tools" / "verify_ir.py",
            *input_files,
            f"--emit-dir={object_dir}",
            f"--emit-opt={emit_opt}",
            "--incremental",
            f"--jobs={emit_jobs}",
        ]
        if appended_ir is not None:
            if not appended_ir:
                raise ValueError("cannot append an empty IR batch")
            command.append("--append-only")
        run(command)
    if emit_only:
        return
    command = [
        sys.executable, source_root / "tools" / "link_native_objects.py",
        metadata_path, object_dir, library,
        "--source-root", source_root,
    ]
    if pdb:
        command.append("--pdb")
    if cache_base is not None:
        command.append(f"--cache-base={cache_base}")
    run(command)


def build_overlay_library(source_root, metadata_path, object_dir, new_ir,
                          overlay_dir, output, cache_base):
    """Link ONLY newly appended guest objects into a small overlay library.

    The objects were already emitted and recorded in object_dir/objects.json.
    A live runner keeps its current module and consults the overlay for PCs
    the base does not own, so a cache miss links a handful of objects instead
    of the whole (~8,000-object) image. The authoritative full library is
    relinked once when the live session ends.
    """
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    wanted = {Path(path).name for path in new_ir}
    functions = [
        function for function in metadata["functions"]
        if Path(function["ir_path"]).name in wanted
    ]
    if len(functions) != len(wanted):
        raise ValueError("overlay functions missing from metadata")
    manifest = json.loads((object_dir / "objects.json").read_text(encoding="utf-8"))
    by_input = {Path(item["input"]).name: item for item in manifest["objects"]}
    objects = []
    for function in functions:
        item = by_input.get(Path(function["ir_path"]).name)
        if item is None:
            raise ValueError(f"overlay object missing for {function['ir_path']}")
        item = dict(item)
        # Absolute paths: the overlay manifest lives outside object_dir.
        item["object"] = str((object_dir / item["object"]).resolve())
        objects.append(item)

    overlay_objects = overlay_dir / "objects"
    overlay_objects.mkdir(parents=True, exist_ok=False)
    overlay_metadata = {
        key: value for key, value in metadata.items()
        if not isinstance(value, (list, dict))
    }
    overlay_metadata["functions"] = functions
    overlay_metadata["function_count"] = len(functions)
    overlay_metadata_path = overlay_dir / "ee_functions.json"
    overlay_metadata_path.write_text(json.dumps(overlay_metadata) + "\n",
                                     encoding="utf-8")
    overlay_manifest = {
        key: value for key, value in manifest.items() if key != "objects"
    }
    overlay_manifest["objects"] = objects
    overlay_manifest["object_count"] = len(objects)
    (overlay_objects / "objects.json").write_text(
        json.dumps(overlay_manifest) + "\n", encoding="utf-8"
    )
    run([
        sys.executable, source_root / "tools" / "link_native_objects.py",
        overlay_metadata_path, overlay_objects, output,
        "--source-root", source_root, f"--cache-base={cache_base}",
        "--data-dispatch",
    ])


def run_hot_expand(command, directory, timeout_seconds, compile_missing):
    """Hold one native process alive while the host recompiles cache misses.

    The runner is paused at a zero-dispatch boundary. Guest CPU/IOP, device
    state, controller and memory card objects remain in that process; this is
    deliberately *not* a persistent cross-process savestate.
    """
    print("+ " + subprocess.list2cmdline([str(item) for item in command]), flush=True)
    command = [str(item) for item in command]
    sequence = 1
    active_start = time.monotonic()
    timed_out = False
    with tempfile.TemporaryFile(mode="w+b") as output:
        process = subprocess.Popen(
            command, stdout=output, stderr=subprocess.STDOUT,
        )
        try:
            while process.poll() is None:
                request = directory / f"request-{sequence}.txt"
                if request.is_file():
                    response = directory / f"response-{sequence}.txt"
                    try:
                        parts = request.read_text(encoding="utf-8").splitlines()
                        if len(parts) != 2 or int(parts[0]) != sequence:
                            raise ValueError("invalid live cache-miss request")
                        pc = int(parts[1], 0)
                        if pc < 0 or pc > 0xffffffff or pc & 3:
                            raise ValueError("invalid live cache-miss PC")
                        print(f"live cache-miss: PC=0x{pc:08x}, request={sequence}",
                              flush=True)
                        compile_start = time.monotonic()
                        try:
                            replacement = compile_missing(pc, sequence)
                        finally:
                            # Compiler time is not charged to the guest replay
                            # wall clock. The 30-minute native handshake timeout
                            # remains an independent fail-safe.
                            active_start += time.monotonic() - compile_start
                        if replacement is None:
                            result = "STOP"
                        elif isinstance(replacement, str):
                            result = replacement  # e.g. "overlay=<absolute path>"
                        else:
                            result = str(replacement.resolve())
                    except Exception as exc:
                        result = "ERROR: " + str(exc).replace("\r", " ").replace("\n", " ")
                        print(f"run_game: hot expansion failed: {exc}",
                              file=sys.stderr, flush=True)
                    pending = response.with_suffix(".tmp")
                    # Never use Windows text-mode newline translation here: 
                    # the native binary reader expects an exact two-line protocol.
                    with pending.open("w", encoding="utf-8", newline="\n") as response_output:
                        response_output.write(f"{sequence}\n{result}\n")
                    pending.replace(response)
                    sequence += 1

                if time.monotonic() - active_start > timeout_seconds:
                    print("run_game: live replay exceeded guest wall-clock timeout",
                          file=sys.stderr, flush=True)
                    process.kill()
                    timed_out = True
                    break
                # Overlay links answer a miss in well under a second; a coarse
                # poll would add up to 100 ms of pure latency per miss.
                time.sleep(0.005)
            process.wait()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        output.seek(0)
        captured = output.read().decode("utf-8", errors="replace")
    if captured:
        print(captured, end="", flush=True)
    return subprocess.CompletedProcess(command, 124 if timed_out else process.returncode,
                                       captured, "")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--config", default="Release", choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"),
                        help="CMake configuration for multi-config generators (default: Release)")
    parser.add_argument("--work-dir", type=Path, default=Path("build/game-run"))
    parser.add_argument(
        "--functions", default="entry",
        help="'entry' (default), a positive function cap, or 'all'",
    )
    parser.add_argument(
        "--extra-functions", default="",
        help="comma-separated additional guest function PCs, e.g. 0x17f018",
    )
    parser.add_argument(
        "--auto-expand", type=int, default=0,
        help="relink/replay with up to N newly translated executable entry PCs",
    )
    parser.add_argument("--replay-after-expand", action="store_true",
                        help="Use legacy reboot/replay after each new IR function")
    parser.add_argument("--live-full-relink", action="store_true",
                        help="Relink the whole library on each live cache miss "
                             "instead of loading a small overlay of new functions")
    parser.add_argument(
        "--auto-expand-table", type=int, default=24,
        help="prefetch up to N sibling function pointers per replay when anchored by a guest-reached entry (0=off)",
    )
    parser.add_argument("--pdb", action="store_true",
                        help="Generate game-translated.pdb using Windows Clang/LLD")
    parser.add_argument("--emit-opt", type=int, choices=range(6), default=0,
                        metavar="{0,1,2,3,4,5}",
                        help="LLVM backend O0..O3; 4=O2 IR + O2 backend; 5=host-native O2 backend")
    parser.add_argument("--emit-jobs", type=int, default=2,
                        help="parallel LLVM object-emission processes (default: 2)")
    parser.add_argument("--max-dispatches", type=int, default=10000)
    parser.add_argument(
        "--loop-budget", type=int, default=0,
        help="Optional translated CFG loop-header watchdog limit per native dispatch (0=off)",
    )
    parser.add_argument(
        "--loop-yield-resumes", type=int, default=0,
        help="Opt-in bounded native host safepoint resumes; requires --loop-budget > 0",
    )
    parser.add_argument(
        "--sif-poll-limit", type=int, default=0,
        help="Stop after N consecutive unchanged IOP SMFLAG reads at the same syscall (0=off)",
    )
    parser.add_argument(
        "--iop-irx", type=Path, action="append", default=[],
        help="Repeat in provider-before-consumer order; last module runs by default",
    )
    parser.add_argument(
        "--iop-irx-list", type=Path,
        help="UTF-8 file of ordered IRX paths, one per line (# comments allowed)",
    )
    parser.add_argument(
        "--iop-start-all", action="store_true",
        help="Start each loaded IRX; default is the final module only",
    )
    parser.add_argument(
        "--iop-quantum", type=int, default=256,
        help="Bounded R3000 instructions per SIF poll, when --iop-irx is provided",
    )
    parser.add_argument(
        "--disc-image", type=Path,
        help="Raw 2048-byte-sector ISO attached to CDVD (legacy ROM0 mode)",
    )
    parser.add_argument(
        "--disc-folder", type=Path,
        help="Extracted disc directory for HLE IOP module loading (no ISO or ROM0)",
    )
    parser.add_argument(
        "--timeout-seconds", type=int, default=10,
        help="hard wall-time limit for each guest replay (default: 10)",
    )
    parser.add_argument("--entry", type=lambda value: int(value, 0))
    parser.add_argument("--stack", type=lambda value: int(value, 0))
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--trace-instructions", type=Path,
                        help="Save a bounded native guest PC stream (NOT EE cycles)")
    parser.add_argument("--trace-instructions-window", type=str,
                        help="Optional inclusive guest PC window START:END")
    parser.add_argument("--trace-entry", type=lambda value: int(value, 0),
                        help="Dump guest arguments and backed pointer words on entry to a specified PC")
    parser.add_argument("--watch-word", type=lambda value: int(value, 0),
                        help="Log changes to a 32-bit guest RAM word between native dispatches")
    parser.add_argument("--trace-v0", type=parse_trace_v0,
                        help="Trace native guest V0 changes within an aligned guest PC interval START:END")
    parser.add_argument("--trace-store", type=lambda value: int(value, 0),
                        help="Trace guest store PCs and operands overlapping a 32-bit guest word")
    parser.add_argument("--memcard-discard-writes", action="store_true",
                        help="Keep guest memory card writes in memory only")
    parser.add_argument("--gs-snapshot", type=Path,
                        help="Optional PPM display VRAM readback (not a completed frame)")
    parser.add_argument("--gs-upload-snapshot", type=Path,
                        help="Optional completed GS IMAGE transfer PPM (not a rendered frame)")
    parser.add_argument("--gs-webgpu-snapshot", type=Path,
                        help="Optional native WebGPU rasterized GS display PPM (requires EE_ENABLE_WEBGPU)")
    parser.add_argument("--speed-percent", type=int, choices=range(1001), metavar="0..1000",
                        help="Window pacing target (0=unlimited, default 100)")
    parser.add_argument("--gs-webgpu-window", action="store_true",
                        help="Present native WebGPU GS scanout in a raylib window (Windows)")
    parser.add_argument("--report", type=Path,
                        help="Save structured EE stop state and last 64 native dispatches as JSON")
    parser.add_argument("--kernel-ram", type=Path,
                        help="512 KiB raw EE kernel RAM snapshot captured at game entry (not BIOS ROM)")
    parser.add_argument("--kseg-ram", action="store_true",
                        help="Map EE KSEG0/KSEG1 RAM mirrors (not MMIO or TLB)")
    parser.add_argument("--hle-startup", action="store_true",
                        help="Experimental SetupThread and SetupHeap HLE")
    parser.add_argument("--ee-clock", choices=("reference-compat", "off"),
                        default="reference-compat",
                        help="HLE event clock: ps2-web-recomp-style budget or disabled")
    parser.add_argument("--no-recompile", action="store_true",
                        help="Reuse existing metadata and linked library; --auto-expand may still add functions")
    parser.add_argument("--relink-existing", action="store_true",
                        help="Rebuild host DLL from verified, preserved guest IR/objects")
    parser.add_argument("--link-only", action="store_true",
                        help="Stop after generating/linking the native DLL (and PDB if requested)")
    args = parser.parse_args(argv)

    if not 1 <= args.emit_jobs <= 32:
        parser.error("--emit-jobs must be between 1 and 32")
    if args.max_dispatches <= 0 or args.max_dispatches > 0xFFFFFFFF:
        parser.error("--max-dispatches must fit a positive uint32")
    if args.loop_budget < 0 or args.loop_budget > 0xFFFFFFFF:
        parser.error("--loop-budget must fit a uint32")
    if args.loop_yield_resumes < 0 or args.loop_yield_resumes > 0xFFFFFFFF:
        parser.error("--loop-yield-resumes must fit a uint32")
    if args.loop_yield_resumes and not args.loop_budget:
        parser.error("--loop-yield-resumes requires --loop-budget > 0")
    if args.sif_poll_limit < 0 or args.sif_poll_limit > 0xFFFFFFFF:
        parser.error("--sif-poll-limit must fit a uint32")
    if args.iop_quantum < 1 or args.iop_quantum > 65536:
        parser.error("--iop-quantum must be in [1, 65536]")
    if args.iop_irx_list is not None and args.iop_irx:
        parser.error("--iop-irx-list and --iop-irx are mutually exclusive")
    iop_paths = list(args.iop_irx)
    if args.iop_irx_list is not None:
        try:
            manifest = args.iop_irx_list.resolve(strict=True)
            for line in manifest.read_text(encoding="utf-8-sig").splitlines():
                entry = line.strip()
                if not entry or entry.startswith("#"):
                    continue
                item = Path(entry)
                iop_paths.append(item if item.is_absolute()
                                 else manifest.parent / item)
        except (OSError, UnicodeError) as exc:
            parser.error(f"cannot read IOP IRX list: {exc}")
    if len(iop_paths) > 32:
        parser.error("a maximum of 32 IOP IRX modules may be loaded")
    if args.iop_irx_list is not None and not iop_paths:
        parser.error("--iop-irx-list contains no IRX paths")
    if args.iop_start_all and not iop_paths:
        parser.error("--iop-start-all requires at least one IOP IRX")
    for irx in iop_paths:
        if not irx.is_file():
            parser.error(f"IOP IRX file not found: {irx}")
    for name in ("trace_entry", "watch_word", "trace_store"):
        address = getattr(args, name)
        if address is not None and not (0 <= address <= 0xFFFFFFFF and address % 4 == 0):
            parser.error(f"--{name.replace('_', '-')} must be an aligned uint32")
    if args.timeout_seconds < 1 or args.timeout_seconds > 3600:
        parser.error("--timeout-seconds must be in [1, 3600]")
    if args.auto_expand < 0 or args.auto_expand > 1024:
        parser.error("--auto-expand must be between 0 and 1024")
    if args.auto_expand_table < 0 or args.auto_expand_table > 128:
        parser.error("--auto-expand-table must be between 0 and 128")
    if args.pdb and os.name != "nt":
        parser.error("--pdb requires a Windows Clang/LLD host toolchain")
    if args.no_recompile and args.extra_functions:
        parser.error("--no-recompile cannot combine with --extra-functions")
    if args.no_recompile and args.relink_existing:
        parser.error("--relink-existing cannot be combined with --no-recompile")
    if args.relink_existing and args.extra_functions:
        parser.error("--relink-existing cannot be combined with --extra-functions")
    if args.link_only and args.auto_expand:
        parser.error("--link-only cannot be combined with --auto-expand")
    lock = None
    try:
        functions = args.functions
        if functions not in ("entry", "all") and (
            not functions.isdecimal() or int(functions) <= 0 or int(functions) > 0xFFFFFFFF
        ):
            parser.error("--functions must be entry, all or a positive integer")

        extra = [
            parse_address(item.strip())
            for item in args.extra_functions.split(",") if item.strip()
        ]
        elf = args.elf.resolve(strict=True)
        build = args.build_dir.resolve()
        work = args.work_dir.resolve()
        acquired_lock = shared_ir.ir_lock(work)
        acquired_lock.__enter__()
        lock = acquired_lock
        entry = elf_entry(elf) if args.entry is None else args.entry
        if not 0 <= entry <= 0xFFFFFFFF or entry & 3:
            parser.error("--entry must be an aligned uint32")
        if args.stack is not None and not 0 <= args.stack <= 0xFFFFFFFF:
            parser.error("--stack must fit uint32")

        source_root = Path(__file__).resolve().parents[1]
        runner = native_executable(build, "ee_game_runner", args.config)
        metadata_path = shared_ir.ir_directory(work) / "ee_functions.json"
        object_dir = work / "objects"
        library_name = "game-translated.dll" if os.name == "nt" else (
            "libgame-translated.dylib" if sys.platform == "darwin" else
            "libgame-translated.so"
        )
        library = work / library_name
        recomp = native_executable(build, "ee_recomp", args.config)
        cache_path = work / "translation-cache.json"
        fingerprints_started = time.monotonic()
        inputs = translation_inputs(elf, recomp, source_root)
        ir_sources = ir_generation_inputs(elf, source_root)
        print(f"EE input fingerprints: {time.monotonic() - fingerprints_started:.3f}s",
              flush=True)

        if args.no_recompile and shared_ir.active(metadata_path) and cache_path.is_file():
            snapshot = shared_ir.read_json(cache_path)
            if snapshot.get("metadata_sha256") != sha256_path(metadata_path):
                # Shared discovery can advance between boot-session passes.
                # Preserved-translation validation proves the extension.
                args.no_recompile = False
                args.relink_existing = True

        if not (args.no_recompile or args.relink_existing):
            if shared_ir.active(metadata_path):
                raise ValueError("refusing to regenerate shared EE IR; use a new work directory")
            (work / "ir").mkdir(parents=True, exist_ok=True)
            for stale in (work / "ir").glob("*.ll"):
                stale.unlink()

            command = [recomp, elf, work / "ir"]
            if functions == "entry":
                command.append(f"--function=0x{entry:08x}")
            elif functions != "all":
                command.append(f"--max-functions={functions}")
            run(command)
            shared_ir.normalize_metadata(metadata_path, elf)

            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            if entry not in covered_pcs(metadata):
                raise ValueError(
                    f"ELF entry 0x{entry:08x} is not compiled. "
                    "Use --functions=entry or --functions=all."
                )

            # One discovery transaction accepts at most 128 entries. Seeding a
            # cold --functions=all build with previously reached live PCs can
            # exceed that, so append in ordered 128-entry batches.
            for start in range(0, len(extra), 128):
                batch_append_functions(
                    recomp, elf, work, metadata_path, extra[start:start + 128]
                )
            build_linked_library(
                source_root, work, metadata_path, object_dir, library,
                args.emit_opt, pdb=args.pdb, emit_jobs=args.emit_jobs
            )
            write_translation_cache(cache_path, inputs, args.emit_opt,
                                    metadata_path, library, ir_sources)
        else:
            if not metadata_path.is_file() or (
                args.no_recompile and not library.is_file()
            ):
                raise ValueError(
                    "reuse requires existing function metadata and "
                    "(for --no-recompile) the linked guest library"
                )
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            source_elf = shared_ir.elf_path(metadata, metadata_path.parent)
            if source_elf != elf:
                raise ValueError(
                    "--no-recompile ELF does not match existing translation: "
                    f"{source_elf} != {elf}"
                )
            if entry not in covered_pcs(metadata):
                raise ValueError(
                    f"--no-recompile entry 0x{entry:08x} has no translated block"
                )
            if args.relink_existing:
                pending_append, objects_complete = validate_preserved_translation(
                    cache_path, inputs, ir_sources,
                    args.emit_opt, metadata_path, object_dir
                )
                if pending_append:
                    verify_recovered_append(
                        recomp, elf, work, metadata_path, pending_append
                    )
                    print(
                        f"run_game: recovering {len(pending_append)} "
                        "genuine interrupted guest function(s), "
                        "without retranslating cached IR", flush=True
                    )
                else:
                    print(
                        "run_game: preserving guest IR and objects; "
                        "rebuilding host DLL only", flush=True
                    )
                direct_relink = (not pending_append and objects_complete and
                    can_relink_verified_objects(
                        cache_path, inputs, ir_sources, args.emit_opt,
                        metadata_path, object_dir))
                if direct_relink:
                    print("run_game: complete verified object manifest; "
                          "skipping redundant emission scan", flush=True)
                try:
                    build_linked_library(
                        source_root, work, metadata_path, object_dir, library,
                        args.emit_opt, pdb=args.pdb, emit_jobs=args.emit_jobs,
                        reuse_objects=direct_relink or bool(pending_append and objects_complete),
                        appended_ir=(
                            [shared_ir.module_path(item["ir_path"], metadata_path.parent)
                             for item in pending_append]
                            if pending_append and not objects_complete else None
                        ),
                    )
                except subprocess.CalledProcessError:
                    if not direct_relink:
                        raise
                    # A missing/corrupt object is detected by the linker's
                    # full object hash validation. Repair via the existing
                    # incremental emitter before trying the link again.
                    print("run_game: direct relink failed; verifying and repairing objects",
                          flush=True)
                    build_linked_library(
                        source_root, work, metadata_path, object_dir, library,
                        args.emit_opt, pdb=args.pdb, emit_jobs=args.emit_jobs)
                write_translation_cache(
                    cache_path, inputs, args.emit_opt,
                    metadata_path, library, ir_sources
                )
            else:
                validate_translation_cache(
                    cache_path, inputs, args.emit_opt, metadata_path, library
                )
                if args.pdb:
                    build_linked_library(
                        source_root, work, metadata_path, object_dir, library,
                        args.emit_opt, pdb=True, reuse_objects=True, emit_jobs=args.emit_jobs
                    )
                    write_translation_cache(
                        cache_path, inputs, args.emit_opt,
                        metadata_path, library, ir_sources
                    )
            if args.auto_expand:
                recomp = native_executable(build, "ee_recomp", args.config)
                if not object_dir.is_dir():
                    raise ValueError(
                        f"--no-recompile --auto-expand requires object directory: {object_dir}"
                    )

        if args.link_only:
            print(f"run_game: native library ready: {library}", flush=True)
            if args.pdb:
                print(f"run_game: PDB ready: {library.with_suffix('.pdb')}", flush=True)
            return 0

        command = [
            runner, elf, library,
            f"--max-dispatches={args.max_dispatches}",
            f"--loop-budget={args.loop_budget}",
            f"--loop-yield-resumes={args.loop_yield_resumes}",
            f"--ee-clock={args.ee_clock}",
            f"--sif-poll-limit={args.sif_poll_limit}",
            f"--entry=0x{entry:08x}",
        ]
        if args.stack is not None:
            command.append(f"--stack=0x{args.stack:08x}")
        if iop_paths:
            for irx in iop_paths:
                command.append(f"--iop-irx={irx.resolve(strict=True)}")
            command.append(f"--iop-quantum={args.iop_quantum}")
            if args.iop_start_all:
                command.append("--iop-start-all")
        if args.disc_folder is not None:
            folder = args.disc_folder.resolve(strict=True)
            if not folder.is_dir():
                parser.error(f"--disc-folder is not a directory: {folder}")
            if args.disc_image is not None:
                parser.error("--disc-folder and --disc-image are mutually exclusive")
            command.append(f"--disc-folder={folder}")
        if args.disc_image is not None:
            command.append(
                f"--disc-image={args.disc_image.resolve(strict=True)}"
            )
        if args.trace:
            command.append("--trace")
        if args.trace_instructions is not None:
            instruction_trace_path = args.trace_instructions.resolve()
            instruction_trace_path.parent.mkdir(parents=True, exist_ok=True)
            command.append(f"--trace-instructions={instruction_trace_path}")
        if args.trace_instructions_window is not None:
            if args.trace_instructions is None:
                parser.error("--trace-instructions-window requires --trace-instructions")
            command.append(f"--trace-instructions-window={args.trace_instructions_window}")
        if args.trace_entry is not None:
            command.append(f"--trace-entry=0x{args.trace_entry:08x}")
        if args.watch_word is not None:
            command.append(f"--watch-word=0x{args.watch_word:08x}")
        if args.trace_store is not None:
            command.append(f"--trace-store=0x{args.trace_store:08x}")
        if args.trace_v0 is not None:
            command.append(f"--trace-v0={args.trace_v0}")
        if args.gs_snapshot is not None:
            command.append(f"--gs-snapshot={args.gs_snapshot.resolve()}")
        if args.gs_upload_snapshot is not None:
            command.append(
                f"--gs-upload-snapshot={args.gs_upload_snapshot.resolve()}"
            )
        if args.gs_webgpu_snapshot is not None:
            image_path = args.gs_webgpu_snapshot.resolve()
            image_path.parent.mkdir(parents=True, exist_ok=True)
            command.append(f"--gs-webgpu-snapshot={image_path}")
        if args.gs_webgpu_window:
            command.append("--gs-webgpu-window")
            if args.speed_percent is not None:
                command.append(f"--speed-percent={args.speed_percent}")
        if args.report is not None:
            report_path = args.report.resolve()
            report_path.parent.mkdir(parents=True, exist_ok=True)
            command.append(f"--report={report_path}")
        if args.hle_startup:
            command.append("--hle-startup")
        if args.kseg_ram:
            command.append("--kseg-ram")
        if args.kernel_ram is not None:
            if not args.kseg_ram:
                parser.error("--kernel-ram requires --kseg-ram")
            snapshot = args.kernel_ram.resolve(strict=True)
            if snapshot.stat().st_size != 0x80000:
                raise ValueError(
                    f"EE kernel RAM snapshot must be exactly 524288 bytes: {snapshot}"
                )
            with snapshot.open("rb") as snapshot_file:
                signature = snapshot_file.read(4)
            if signature in (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08"):
                raise ValueError(
                    "kernel RAM snapshot contains ZIP/savestate bytes, not raw EE RAM; "
                    "extract and decompress the internal eeMemory.bin member first"
                )
            command.append(f"--kernel-ram={snapshot}")

        # Every discovery replay starts with the same on-disk card. The
        # final replay without auto-expansion can persist guest changes.
        if args.auto_expand or args.memcard_discard_writes:
            command.append("--memcard-discard-writes")

        # By default, auto-expansion preserves the *live process*. On a
        # zero-dispatch cache miss the native runner stops all guest activity,
        # requests a newly linked module, hot-loads it, and retries that PC.
        # No CPU, IOP, DMA, GS, controller, or card state is reconstructed.
        if args.auto_expand and not args.replay_after_expand:
            ranges = executable_segments(elf)
            expanded_count = 0
            overlay_count = 0
            use_overlays = not args.live_full_relink
            last_library = None
            with tempfile.TemporaryDirectory(
                prefix="ee-live-expand-", dir=work
            ) as temporary:
                session = Path(temporary)
                hot_command = [*command, f"--hot-expand-dir={session}"]

                def compile_live_miss(address, sequence):
                    nonlocal expanded_count, last_library, overlay_count
                    if expanded_count >= args.auto_expand:
                        print("live cache-miss: expansion limit reached; "
                              "preserving stopped replay report", flush=True)
                        return None
                    if (translation_inputs(elf, recomp, source_root) != inputs or
                            ir_generation_inputs(elf, source_root) != ir_sources):
                        raise ValueError(
                            "ELF or translation sources changed during live "
                            "checkpoint; refusing to load incompatible code"
                        )
                    if not any(start <= address < end for start, end in ranges):
                        raise ValueError(
                            f"guest cache miss 0x{address:08x} is outside "
                            "ELF executable segments"
                        )
                    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
                    existing = covered_pcs(metadata)
                    if address in existing:
                        raise ValueError(
                            f"guest cache miss 0x{address:08x} is already compiled"
                        )
                    available = args.auto_expand - expanded_count
                    siblings = branch_anchored_jump_table_siblings(
                        elf, address, ranges,
                        min(args.auto_expand_table, available - 1),
                    )
                    if not siblings:
                        siblings = pointer_table_siblings(
                            elf, address, ranges,
                            min(args.auto_expand_table, available - 1),
                        )
                    targets = [address] + [
                        candidate for candidate in siblings
                        if candidate not in existing and candidate != address
                    ]
                    targets = targets[:available]
                    print(f"auto-expand: compiling reachable PC 0x{address:08x} "
                          "(live cache-miss)", flush=True)
                    for candidate in targets[1:]:
                        print(f"auto-expand: compiling table sibling PC "
                              f"0x{candidate:08x}", flush=True)
                    new_modules = batch_append_functions(
                        recomp, elf, work, metadata_path, targets
                    )
                    if not new_modules:
                        raise ValueError(
                            f"no translated modules produced for 0x{address:08x}"
                        )
                    new_metadata = json.loads(
                        metadata_path.read_text(encoding="utf-8")
                    )
                    if address not in covered_pcs(new_metadata):
                        raise ValueError(
                            f"new native library does not cover 0x{address:08x}"
                        )
                    suffix = ".dll" if os.name == "nt" else (
                        ".dylib" if sys.platform == "darwin" else ".so"
                    )
                    replacement = session / f"game-translated-live-{sequence:04d}{suffix}"
                    # Never overwrite the DLL loaded by a running Windows
                    # process. Each new generation is a unique host library.
                    if use_overlays:
                        # Emit only the new objects, then link them alone.
                        build_linked_library(
                            source_root, work, metadata_path, object_dir,
                            None, args.emit_opt, emit_jobs=args.emit_jobs,
                            appended_ir=new_modules, emit_only=True,
                        )
                        build_overlay_library(
                            source_root, metadata_path, object_dir,
                            new_modules, session / f"overlay-{sequence:04d}",
                            replacement, library,
                        )
                    else:
                        build_linked_library(
                            source_root, work, metadata_path, object_dir,
                            replacement, args.emit_opt, pdb=args.pdb,
                            emit_jobs=args.emit_jobs, appended_ir=new_modules,
                            cache_base=library
                        )
                    if (translation_inputs(elf, recomp, source_root) != inputs or
                            ir_generation_inputs(elf, source_root) != ir_sources):
                        raise ValueError(
                            "ELF or translation sources changed during hot relink"
                        )
                    expanded_count += len(new_modules)
                    if use_overlays:
                        overlay_count += 1
                        return "overlay=" + str(replacement.resolve())
                    last_library = replacement
                    return replacement

                # run_boot_session.py measures a fresh pass's baseline from
                # links printed before this header. Live relinks follow it.
                print("\n== Executing translated EE code "
                      "(live cache-miss resume) ==", flush=True)
                execution_started = time.monotonic()
                result = run_hot_expand(
                    hot_command, session, args.timeout_seconds,
                    compile_live_miss
                )
                execution_finished = time.monotonic()
                print(f"EE native process wall: {execution_finished - execution_started:.3f}s",
                      flush=True)
                if overlay_count and result.returncode != 1:
                    # The runner has exited and unloaded every module. Fold
                    # the overlays into one full library exactly once; all of
                    # its objects are re-verified against objects.json.
                    final_library = session / f"game-translated-live-final{library.suffix}"
                    build_linked_library(
                        source_root, work, metadata_path, object_dir,
                        final_library, args.emit_opt, pdb=args.pdb,
                        reuse_objects=True, cache_base=library,
                    )
                    last_library = final_library
                if last_library is not None and result.returncode != 1:
                    # The runner has exited and unloaded its modules. Publish
                    # the latest linked library to the normal cache atomically.
                    staged = library.with_name(library.name + ".hot-new")
                    try:
                        shutil.copy2(last_library, staged)
                        staged.replace(library)
                    finally:
                        staged.unlink(missing_ok=True)
                    if args.pdb:
                        hot_pdb = last_library.with_suffix(".pdb")
                        if not hot_pdb.is_file():
                            raise ValueError("live link did not produce requested PDB")
                        pdb_destination = library.with_suffix(".pdb")
                        pdb_staged = pdb_destination.with_suffix(".pdb.hot-new")
                        try:
                            shutil.copy2(hot_pdb, pdb_staged)
                            pdb_staged.replace(pdb_destination)
                        finally:
                            pdb_staged.unlink(missing_ok=True)
                    write_translation_cache(
                        cache_path, inputs, args.emit_opt,
                        metadata_path, library, ir_sources
                    )
                    print(f"live cache-miss: published {expanded_count} "
                          "new guest module(s) to translation cache",
                          flush=True)
                    print(f"EE post-replay consolidation and cache publication: "
                          f"{time.monotonic() - execution_finished:.3f}s",
                          flush=True)
                if (last_library is not None or overlay_count) and result.returncode == 1:
                    print("run_game: not publishing failed live expansion; "
                          "use cache recovery/relink on the next run",
                          file=sys.stderr, flush=True)
                return result.returncode

        ranges = executable_segments(elf) if args.auto_expand else []
        expanded_count = 0
        for replay in range(args.auto_expand + 1):
            print(
                f"\n== Executing translated EE code (replay {replay}) ==",
                flush=True,
            )
            try:
                result = run(
                    command, capture=True,
                    timeout_seconds=args.timeout_seconds,
                )
            except subprocess.TimeoutExpired as error:
                if error.stdout:
                    print(
                        error.stdout.decode(errors="replace")
                        if isinstance(error.stdout, bytes) else error.stdout,
                        end="",
                    )
                if error.stderr:
                    print(
                        error.stderr.decode(errors="replace")
                        if isinstance(error.stderr, bytes) else error.stderr,
                        end="", file=sys.stderr,
                    )
                print(
                    "run_game: replay exceeded its wall-clock timeout; "
                    "this may be slow tracing, guest polling, or an internal loop",
                    file=sys.stderr,
                )
                if args.report is not None and report_path.is_file():
                    try:
                        snapshot = json.loads(report_path.read_text(encoding="utf-8"))
                        recent = snapshot.get("history", [])
                        latest = recent[-1] if recent else None
                        print(
                            "run_game: last saved snapshot "
                            f"kind={snapshot.get('snapshot_kind', 'unknown')} "
                            f"dispatches={snapshot.get('dispatches')} "
                            f"pc=0x{snapshot['final_pc']:08x} "
                            f"replay={replay}",
                            file=sys.stderr,
                        )
                        if latest:
                            print(
                                "run_game: last recorded dispatch "
                                f"pc=0x{latest['from_pc']:08x} -> "
                                f"0x{latest['to_pc']:08x} "
                                f"reason={latest['reason']}",
                                file=sys.stderr,
                            )
                    except (OSError, ValueError, KeyError, TypeError) as snapshot_error:
                        print(
                            f"run_game: could not read timeout snapshot: {snapshot_error}",
                            file=sys.stderr,
                        )
                return 124
            if expanded_count >= args.auto_expand or result.returncode not in (3, 4):
                return result.returncode

            match = STOP_PATTERN.search(result.stdout)
            if not match or int(match.group(1)) == 0:
                return result.returncode
            address = int(match.group(2), 16)
            if not any(start <= address < end for start, end in ranges):
                print(
                    f"auto-expand stopped at non-executable PC 0x{address:08x}",
                    file=sys.stderr,
                )
                return result.returncode
            if address in covered_pcs(
                json.loads(metadata_path.read_text(encoding="utf-8"))
            ):
                print(
                    f"auto-expand stopped at already-compiled PC 0x{address:08x}",
                    file=sys.stderr,
                )
                return result.returncode

            remaining = args.auto_expand - expanded_count
            table_limit = min(args.auto_expand_table, remaining - 1)
            siblings = branch_anchored_jump_table_siblings(
                elf, address, ranges, table_limit,
            )
            if not siblings:
                siblings = pointer_table_siblings(
                    elf, address, ranges, table_limit,
                )
            if siblings:
                print(
                    f"auto-expand: guest-reached PC 0x{address:08x} anchors "
                    f"{len(siblings)} additional table entries; "
                    "batching one native relink",
                    flush=True,
                )
            targets = [address, *siblings]
            owners = covered_pcs(
                json.loads(metadata_path.read_text(encoding="utf-8"))
            )
            for candidate in targets:
                if candidate in owners:
                    continue
                if candidate == address:
                    print(
                        f"auto-expand: compiling reachable PC "
                        f"0x{candidate:08x}", flush=True,
                    )
                else:
                    print(
                        f"auto-expand: compiling table sibling PC "
                        f"0x{candidate:08x}", flush=True,
                    )
            new_modules = batch_append_functions(
                recomp, elf, work, metadata_path, targets
            )
            if not new_modules:
                return result.returncode
            expanded_count += len(new_modules)
            build_linked_library(
                source_root, work, metadata_path, object_dir, library,
                args.emit_opt, pdb=args.pdb, emit_jobs=args.emit_jobs,
                appended_ir=new_modules
            )
            write_translation_cache(cache_path, inputs, args.emit_opt,
                                    metadata_path, library, ir_sources)
        return 3
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"run_game: {error}", file=sys.stderr)
        return 1
    finally:
        if lock is not None:
            lock.__exit__(None, None, None)


if __name__ == "__main__":
    raise SystemExit(main())
