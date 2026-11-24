#!/usr/bin/env python3
"""Compile captured VU1 programs into opt-in native libraries.

Unsupported captures remain interpreter-only. Existing libraries are not
deleted; the runtime checks exact microcode bytes before every native entry.
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--opt", choices=("0", "1", "2", "3"), default="3")
    parser.add_argument("--march", choices=("generic", "native"), default="generic",
                        help="native tunes for this host only; use a separate output directory")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    suffix = ".dll" if os.name == "nt" else ".so"
    executable = "vu1_recomp.exe" if os.name == "nt" else "vu1_recomp"
    recomp = args.build_dir / executable
    if not recomp.is_file():
        recomp = args.build_dir / "Release" / executable
    clang = shutil.which("clang")
    if clang is None and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM/bin/clang.exe"
        if candidate.is_file():
            clang = str(candidate)
    if clang is None or not recomp.is_file():
        parser.error("clang and the built vu1_recomp executable are required")
    captures = sorted(args.capture.glob("*.bin"))
    if not captures:
        parser.error("no captured VU1 programs")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if args.jobs < 1:
        parser.error("--jobs must be positive")

    # A target-specific library can execute unsupported instructions on a
    # different host. Never mix portable and tuned libraries in one set.
    target_marker = out / "vu1-build-target.txt"
    if target_marker.exists():
        if target_marker.read_text(encoding="utf-8").strip() != args.march:
            parser.error("VU1 target changed; use a separate output directory")
    elif args.march == "native" and any(out.glob("*" + suffix)):
        parser.error("unidentified existing libraries; use a fresh native output directory")
    target_marker.write_text(args.march + "\n", encoding="utf-8")
    print(f"VU1 AOT target: {args.march}", flush=True)

    def build_one(capture):
        start = int(capture.stem.split("-", 1)[0], 16)
        with tempfile.TemporaryDirectory(prefix="vu1-", dir=out) as directory:
            ir = Path(directory) / "program.ll"
            result = subprocess.run([str(recomp.resolve()), str(capture.resolve()),
                                     hex(start), str(ir)],
                                    capture_output=True, text=True)
            if result.returncode == 3:
                return False
            result.check_returncode()
            library = Path(directory) / ("program" + suffix)
            command = [clang, f"-O{args.opt}", "-shared", "-fuse-ld=lld",
                       "-Wno-override-module", str(ir), "-o", str(library)]
            if args.march == "native":
                command.append("-march=native")
            if os.name == "nt":
                command += ["-nostdlib", "-Wl,/noentry"]
            else:
                command += ["-fPIC"]
            subprocess.run(command, check=True)
            library.replace(out / (capture.stem + suffix))
            return True

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(build_one, captures))
    print(f"VU1 AOT: {sum(results)}/{len(results)} captures compiled; "
          f"{len(results) - sum(results)} interpreter-only")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
