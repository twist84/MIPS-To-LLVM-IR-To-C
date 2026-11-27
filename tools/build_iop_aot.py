#!/usr/bin/env python3
"""Build one ahead-of-time library per IOP IRX module from a runner capture.

1. Capture: run the game once with SDBZ_IOP_AOT_CAPTURE=CAPTURE_DIR. Every
   loaded IRX writes NAME@BASE.bin (relocated, linked .text) and .json.
2. This script: iop_recomp (C++) translates each module to LLVM IR
   (NAME@BASE.ll + NAME@BASE.table.ll), clang compiles the IR, and lld links
   OUT/modules/IRX_NAME.dll (.so elsewhere) -- one library per module.
   Multiple captured variants of one name receive collision suffixes.
   Modules are built in parallel.
3. Run the game with SDBZ_IOP_AOT=OUT/modules. The runtime loads every
   library in the directory; each module activates only when its bytes in
   IOP RAM match the capture, otherwise it is interpreted.

usage: build_iop_aot.py --build-dir build --capture DIR --out DIR [--opt 2]
"""

import argparse
import concurrent.futures
import collections
import json
import hashlib
import re
import struct
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from audit_iop_function_discovery import inventory


def safe_module_name(name):
    name = re.sub(r"[^A-Za-z0-9_]", "_", name) or "module"
    # Keep filenames portable across case-sensitive and Windows hosts.
    if re.fullmatch(r"CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]", name, re.I):
        name = "_" + name
    return name


def recover_export_library(text, metadata):
    """Recover the first validated library from an older address-only capture.

    Only accept a linked table whose nonzero entries are present in the
    loader's recorded exports. Do not treat arbitrary marker bytes as names.
    """
    exports = metadata.get("exports", [])
    if not isinstance(exports, list) or not all(
            isinstance(entry, int) and 0 < entry <= 0xffffffff
            for entry in exports):
        return ""
    known = set(exports)
    if not known:
        return ""
    for offset in range(0, len(text) - 23, 4):
        if text[offset:offset + 4] != b"\x00\x00\xc0\x41":
            continue
        raw = text[offset + 12:offset + 20].split(b"\x00", 1)[0]
        if not raw or any(ch < 0x21 or ch > 0x7e for ch in raw):
            continue
        entries = []
        for ordinal in range(1024):
            pos = offset + 20 + ordinal * 4
            if pos + 4 > len(text):
                break
            entry = struct.unpack_from("<I", text, pos)[0]
            if entry == 0:
                following = (struct.unpack_from("<I", text, pos + 4)[0]
                             if pos + 8 <= len(text) else 0)
                if following in known:
                    continue  # Same single-hole rule as the IRX loader.
                if entries:
                    return raw.decode("ascii")
                break
            if entry not in known or entry & 3:
                break
            entries.append(entry)
    return ""


def prepare_captures(capture, destination):
    """Stage named captures without modifying original linked bytes/metadata."""
    records = []
    for path in sorted(capture.glob("*.json")):
        metadata = json.loads(path.read_text(encoding="utf-8"))
        binary = path.with_suffix(".bin")
        text = binary.read_bytes()
        export = metadata.get("export_library")
        if not isinstance(export, str) or not export:
            export = recover_export_library(text, metadata)
        # Older captures did not record the LOADFILE filename. These two
        # exact internal titles correspond to the observed SDBZ disc IRXs.
        # This is a naming fallback only, never an activation identity.
        legacy_names = {
            "CRI_ADX_Driver": "CRI_ADXI",
            "ARIKA_IOP_Control_Driver": "ARKD_DVD",
        }
        source = metadata.get("source_name")
        if not isinstance(source, str):
            source = ""
        name = safe_module_name(source or legacy_names.get(metadata["name"])
                                or export or metadata["name"])
        base = metadata["base"]
        digest = metadata["hash"]
        if (not isinstance(base, int) or not 0 <= base <= 0xffffffff or
                not isinstance(digest, str) or
                not re.fullmatch(r"[0-9a-fA-F]{1,16}", digest)):
            raise ValueError(f"invalid IOP capture identity: {path}")
        records.append((path, binary, metadata, name, base, digest.lower()))

    counts = collections.Counter(row[3].casefold() for row in records)
    reserved = {row[3].casefold() for row in records}
    used = set()
    destination.mkdir(parents=True, exist_ok=True)
    for path, binary, metadata, name, base, digest in records:
        stem = name
        if counts[name.casefold()] > 1:
            stem = f"{name}@{base:08x}"
            if stem.casefold() in reserved or stem.casefold() in used:
                stem = f"{stem}-{digest}"
            candidate = stem
            serial = 2
            while stem.casefold() in reserved or stem.casefold() in used:
                stem = f"{candidate}-{serial}"
                serial += 1
        used.add(stem.casefold())
        # The exported descriptor receives the logical name, not the
        # collision suffix. Base, hash and executable text stay unchanged.
        metadata["name"] = name
        shutil.copyfile(binary, destination / f"{stem}.bin")
        (destination / f"{stem}.json").write_text(
            json.dumps(metadata) + "\n", encoding="utf-8")
    return len(records)


def prepare_split_pilot(staged, selected, pilot):
    """Select one canonical capture, re-audit bytes, reject ambiguous owners."""
    candidates = [path for path in staged.glob("*.json")
                  if path.stem.casefold() == selected.casefold()]
    if len(candidates) != 1:
        raise ValueError(f"split pilot must select one exact staged IRX stem: {selected}")
    meta = candidates[0]
    binary = meta.with_suffix(".bin")
    metadata = json.loads(meta.read_text(encoding="utf-8"))
    data = binary.read_bytes()
    report = inventory(metadata, data)
    if (report["shared_ordinary_pcs"] or report["ordinary_delay_overlap"] or
            report["rejected_seeds"] or len(data) % 4):
        raise ValueError("split pilot refuses ambiguous or invalid ownership")
    # A conservative classifier stop is not permission to execute that
    # encoding natively. Keep unsupported boundaries on the interpreter.
    rejected = {item["pc"] for item in report["unresolved"]
                if item["kind"] in ("unsupported", "unsupported_delay", "missing_delay")}
    regions = [[pc for pc in region["ordinary_pcs"] if pc not in rejected]
               for region in report["regions"]]
    regions = [region for region in regions if region]
    if not regions:
        raise ValueError("split pilot has no eligible regions")
    pilot.mkdir()
    shutil.copyfile(meta, pilot / meta.name)
    shutil.copyfile(binary, pilot / binary.name)
    plan = pilot / "ownership.regions"
    lines = [f"IOPREG1 {report['base']} {report['text_size']} "
             f"{int(report['hash'], 16)} {len(regions)}"]
    lines.extend(" ".join(map(str, [len(region), *region])) for region in regions)
    plan.write_text("\n".join(lines) + "\n", encoding="ascii")
    print(f"split pilot: {meta.stem}, {len(regions)} regions, "
          f"{sum(map(len, regions))} ordinary PCs")
    return plan


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def split_cache_key(source, provenance):
    payload = {"schema": 1, "ir_sha256": sha256_file(source),
               "provenance": provenance}
    return hashlib.sha256(json.dumps(payload, sort_keys=True,
                                    separators=(",", ":")).encode("utf-8")).hexdigest()


def cached_split_object(source, obj, cache, provenance, compile_object):
    """Validate a complete immutable entry; publish only a successful object."""
    if cache is None:
        compile_object(source, obj)
        return False
    key = split_cache_key(source, provenance)
    entry = cache / key

    def valid_entry():
        try:
            meta = json.loads((entry / "receipt.json").read_text(encoding="utf-8"))
            binary = entry / "object.o"
            return (meta["key"] == key and binary.stat().st_size > 0 and
                    meta["size"] == binary.stat().st_size and
                    meta["sha256"] == sha256_file(binary))
        except (OSError, ValueError, KeyError, TypeError):
            return False

    if entry.exists():
        if not valid_entry():
            raise ValueError(f"invalid split object cache entry: {entry}")
        shutil.copyfile(entry / "object.o", obj)
        return True

    compile_object(source, obj)
    if obj.stat().st_size == 0:
        raise ValueError("empty split object")
    # A temporary sibling directory makes the receipt and object visible
    # together. A competing writer may win; never replace an existing key.
    with tempfile.TemporaryDirectory(prefix=".pending-", dir=cache) as tmp:
        pending = Path(tmp) / "entry"
        pending.mkdir()
        shutil.copyfile(obj, pending / "object.o")
        (pending / "receipt.json").write_text(json.dumps({
            "key": key, "size": obj.stat().st_size, "sha256": sha256_file(obj)
        }) + "\n", encoding="utf-8")
        try:
            pending.rename(entry)
        except OSError:
            if not valid_entry():
                raise
    return False


def compile_split_pilot(args, clang, ir_dir, obj_dir, lib_dir, stem, suffix,
                        provenance=None):
    sources = sorted(ir_dir.glob(f"{stem}.region.*.ll"))
    if not sources:
        raise ValueError("split pilot emitted no region IR")
    sources += [ir_dir / f"{stem}.ll", ir_dir / f"{stem}.table.ll"]
    started = time.monotonic()

    def compile_object(source, obj):
        command = [clang, f"-O{args.opt}", "-c", "-Wno-override-module"]
        if args.region_cache is not None:
            command += ["--no-default-config"]
        subprocess.run([*command, str(source), "-o", str(obj)], check=True)

    def compile_one(source):
        obj = obj_dir / (source.stem + ".o")
        hit = cached_split_object(source, obj, args.region_cache, provenance,
                                  compile_object)
        return obj, hit

    objects = []
    hits = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for obj, hit in pool.map(compile_one, sources):
            objects.append(obj)
            hits += int(hit)
            print(f"IOP objects [{len(objects)}/{len(sources)}]: {obj.name}"
                  + (" (cached)" if hit else ""))
    print(f"split cache: {hits} reused, {len(objects) - hits} compiled")
    compiled = time.monotonic()
    library = lib_dir / f"{stem}{suffix}"
    command = [clang, "-shared", "-fuse-ld=lld", "-o", str(library),
               *map(str, objects)]
    if os.name == "nt":
        command += ["-nostdlib", "-Wl,/noentry"]
    subprocess.run(command, check=True)
    print(f"split compile {compiled - started:.3f}s, "
          f"link {time.monotonic() - compiled:.3f}s, "
          f"{sum(obj.stat().st_size for obj in objects)} object bytes, "
          f"{library.stat().st_size} library bytes")
    return library


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--opt", default="2", choices=["0", "1", "2", "3"])
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--bootstrap", action="store_true",
                        help="Build missing libraries without clearing existing output files")
    parser.add_argument("--split-module",
                        help="Experimental: compile one exact staged IRX stem in regions")
    parser.add_argument("--region-cache", type=Path,
                        help="Opt-in content-addressed split object cache outside inputs/output")
    args = parser.parse_args()
    if args.region_cache is not None:
        if not args.split_module:
            parser.error("--region-cache requires --split-module")
        args.region_cache = args.region_cache.resolve()
        for protected in (args.capture.resolve(), args.out.resolve()):
            if (args.region_cache.is_relative_to(protected) or
                    protected.is_relative_to(args.region_cache)):
                parser.error("region cache must be separate from captures and output")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.split_module and (args.bootstrap or args.out.exists()):
        parser.error("split pilot requires a fresh output directory, without --bootstrap")

    if args.bootstrap:
        # Generate into a private directory, then publish only completed
        # artifacts. Never delete unrelated files in the user's destination.
        args.out = args.out.resolve()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="iop-aot-", dir=args.out.parent) as temporary:
            staging = Path(temporary)
            build(args, staging)
            for subdir in ("ir", "obj", "modules"):
                destination = args.out / subdir
                destination.mkdir(parents=True, exist_ok=True)
                for source in (staging / subdir).iterdir():
                    source.replace(destination / source.name)
        print(f"build_iop_aot: libraries published to {args.out / 'modules'}")
        return 0
    return build(args, args.out)


def build(args, out):
    exe = ".exe" if os.name == "nt" else ""
    recomp = args.build_dir / f"iop_recomp{exe}"
    if not recomp.is_file():
        recomp = args.build_dir / "Release" / f"iop_recomp{exe}"
    clang = shutil.which("clang")
    if clang is None and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / "clang.exe"
        if candidate.is_file():
            clang = str(candidate)
    if clang is None:
        sys.exit("build_iop_aot: clang not found on PATH")
    if not recomp.is_file():
        sys.exit("build_iop_aot: iop_recomp has not been built")

    ir_dir = out / "ir"
    obj_dir = out / "obj"
    lib_dir = out / "modules"
    for folder in (ir_dir, obj_dir, lib_dir):
        folder.mkdir(parents=True, exist_ok=True)
        for stale in folder.iterdir():
            if stale.is_file():
                stale.unlink()
    started = time.monotonic()
    # The C++ lifter names its output from the capture stem and exports
    # the descriptor from metadata. Stage canonical names for both.
    provenance = None
    with tempfile.TemporaryDirectory(prefix="iop-named-captures-") as temporary:
        staged = Path(temporary)
        prepare_captures(args.capture, staged)
        if args.split_module:
            pilot = staged / "pilot"
            plan = prepare_split_pilot(staged, args.split_module, pilot)
            subprocess.run([str(recomp), str(pilot), str(ir_dir), str(plan)], check=True)
            if args.region_cache is not None:
                args.region_cache.mkdir(parents=True, exist_ok=True)
                provenance = {
                    "plan_sha256": sha256_file(plan),
                    "capture_sha256": sha256_file(next(pilot.glob("*.bin"))),
                    "metadata_sha256": sha256_file(next(pilot.glob("*.json"))),
                    "lifter_sha256": sha256_file(recomp),
                    "compiler_sha256": sha256_file(clang),
                    "compiler_version": subprocess.check_output(
                        [clang, "--no-default-config", "--version"], text=True),
                    "target": subprocess.check_output(
                        [clang, "--no-default-config", "-dumpmachine"], text=True).strip(),
                    "options": [f"-O{args.opt}", "-c", "-Wno-override-module",
                                "--no-default-config"],
                    "platform": os.name,
                }
        else:
            subprocess.run([str(recomp), str(staged), str(ir_dir)], check=True)
    stems = sorted(p.name[:-len(".table.ll")] for p in ir_dir.glob("*.table.ll"))
    if not stems:
        sys.exit("build_iop_aot: no translated IRX modules")
    suffix = ".dll" if os.name == "nt" else ".so"

    if args.split_module:
        if len(stems) != 1:
            raise ValueError("split pilot emitted multiple module tables")
        library = compile_split_pilot(args, clang, ir_dir, obj_dir, lib_dir,
                                      stems[0], suffix, provenance)
        print(f"build_iop_aot: {library} ({time.monotonic() - started:.1f}s total)")
        return 0

    def build_one(stem):
        objects = []
        for source in (ir_dir / f"{stem}.ll", ir_dir / f"{stem}.table.ll"):
            obj = obj_dir / (source.name[:-len(".ll")] + ".o")
            subprocess.run([clang, f"-O{args.opt}", "-c", "-Wno-override-module",
                            str(source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        library = lib_dir / f"{stem}{suffix}"
        command = [clang, "-shared", "-fuse-ld=lld", "-o", str(library), *objects]
        if os.name == "nt":
            command += ["-nostdlib", "-Wl,/noentry"]
        subprocess.run(command, check=True)
        return library

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        libraries = list(pool.map(build_one, stems))
    for library in libraries:
        print(f"build_iop_aot: {library}")
    print(f"build_iop_aot: {len(libraries)} module librar"
          f"{'y' if len(libraries) == 1 else 'ies'} in {lib_dir} "
          f"({time.monotonic() - started:.1f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
