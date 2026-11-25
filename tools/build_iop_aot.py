#!/usr/bin/env python3
"""Build one ahead-of-time library per IOP IRX module from a runner capture.

1. Capture: run the game once with SDBZ_IOP_AOT_CAPTURE=CAPTURE_DIR. Every
   loaded IRX writes NAME@BASE.bin (relocated, linked .text) and .json.
2. This script: iop_recomp (C++) translates each module to LLVM IR
   (NAME@BASE.ll + NAME@BASE.table.ll), clang compiles the IR, and lld links
   OUT/modules/EXPORT_NAME.dll (.so elsewhere) -- one library per module.
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
import re
import struct
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


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
        name = safe_module_name(export or metadata["name"])
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--opt", default="2", choices=["0", "1", "2", "3"])
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--bootstrap", action="store_true",
                        help="Build missing libraries without clearing existing output files")
    args = parser.parse_args()

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
    with tempfile.TemporaryDirectory(prefix="iop-named-captures-") as temporary:
        staged = Path(temporary)
        prepare_captures(args.capture, staged)
        subprocess.run([str(recomp), str(staged), str(ir_dir)], check=True)
    stems = sorted(p.name[:-len(".table.ll")] for p in ir_dir.glob("*.table.ll"))
    if not stems:
        sys.exit("build_iop_aot: no translated IRX modules")
    suffix = ".dll" if os.name == "nt" else ".so"

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
