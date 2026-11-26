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


def build_library(clang, ir, library, opt, march):
    command = [clang, f"-O{opt}", "-shared", "-fuse-ld=lld",
               "-Wno-override-module", str(ir), "-o", str(library)]
    if march == "native":
        command.append("-march=native")
    if os.name == "nt":
        # Generated code can require __chkstk for register spills or other
        # large stack frames. Keep the Windows stack probes and link the
        # normal MSVC support libraries. /noentry still prevents CRT startup.
        command += ["-Wl,/noentry"]
    else:
        command += ["-fPIC"]
    subprocess.run(command, check=True)


def matching_captures(capture_dir, baseline, suffix, march):
    target = (baseline / "vu1-build-target.txt").read_text(encoding="utf-8").strip()
    if target != march:
        raise ValueError(f"baseline target {target!r} differs from requested target {march!r}")
    names = sorted(path.stem for path in baseline.glob("*" + suffix))
    if not names:
        raise ValueError("baseline VU1 library inventory is empty")
    captures = [capture_dir / (name + ".bin") for name in names]
    missing = [path.name for path in captures if not path.is_file()]
    if missing:
        raise ValueError(f"missing baseline capture files: {missing}")
    return captures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--opt", choices=("0", "1", "2", "3"), default="3")
    parser.add_argument("--march", choices=("generic", "native"), default="generic",
                        help="native tunes for this host only; use a separate output directory")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--match-inventory", type=Path,
                        help="rebuild exactly the captures represented in the baseline library directory; requires fresh output")
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
    out = args.out.resolve()
    if args.match_inventory is not None:
        baseline = args.match_inventory.resolve()
        if out == baseline or (out.exists() and any(out.iterdir())):
            parser.error("--match-inventory requires a fresh output directory")
        try:
            captures = matching_captures(args.capture, baseline, suffix, args.march)
        except (OSError, ValueError) as error:
            parser.error(str(error))
        print(f"Matched baseline VU1 inventory: {len(captures)} captures", flush=True)
    else:
        captures = sorted(args.capture.glob("*.bin"))
    if not captures:
        parser.error("no captured VU1 programs")
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
            build_library(clang, ir, library, args.opt, args.march)
            library.replace(out / (capture.stem + suffix))
            return True

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(build_one, captures))
    print(f"VU1 AOT: {sum(results)}/{len(results)} captures compiled; "
          f"{len(results) - sum(results)} interpreter-only")
    if args.match_inventory is not None and not all(results):
        missing = [capture.name for capture, ok in zip(captures, results) if not ok]
        parser.error(f"baseline captures no longer compile: {missing}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
