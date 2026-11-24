#!/usr/bin/env python3
"""Build one ahead-of-time library per IOP IRX module from a runner capture.

1. Capture: run the game once with SDBZ_IOP_AOT_CAPTURE=CAPTURE_DIR. Every
   loaded IRX writes NAME@BASE.bin (relocated, linked .text) and .json.
2. This script: iop_recomp (C++) translates each module to LLVM IR
   (NAME@BASE.ll + NAME@BASE.table.ll), clang compiles the IR, and lld links
   OUT/modules/NAME@BASE.dll (.so elsewhere) -- one library per module.
   Modules are built in parallel.
3. Run the game with SDBZ_IOP_AOT=OUT/modules. The runtime loads every
   library in the directory; each module activates only when its bytes in
   IOP RAM match the capture, otherwise it is interpreted.

usage: build_iop_aot.py --build-dir build --capture DIR --out DIR [--opt 2]
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--opt", default="2", choices=["0", "1", "2", "3"])
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    exe = ".exe" if os.name == "nt" else ""
    recomp = args.build_dir / f"iop_recomp{exe}"
    if not recomp.is_file():
        recomp = args.build_dir / "Release" / f"iop_recomp{exe}"
    clang = shutil.which("clang")
    if clang is None:
        sys.exit("build_iop_aot: clang not found on PATH")

    ir_dir = args.out / "ir"
    obj_dir = args.out / "obj"
    lib_dir = args.out / "modules"
    for folder in (ir_dir, obj_dir, lib_dir):
        folder.mkdir(parents=True, exist_ok=True)
        for stale in folder.iterdir():
            if stale.is_file():
                stale.unlink()
    started = time.monotonic()
    subprocess.run([str(recomp), str(args.capture), str(ir_dir)], check=True)
    stems = sorted(p.name[:-len(".table.ll")] for p in ir_dir.glob("*.table.ll"))
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
