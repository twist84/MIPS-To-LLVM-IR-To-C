#!/usr/bin/env python3
"""End-to-end capture, IR lifting, object generation and VU1 native dispatch."""
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    fixture, recomp, builder = map(Path, sys.argv[1:])
    clang = shutil.which("clang")
    if clang is None and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM/bin/clang.exe"
        if candidate.is_file():
            clang = str(candidate)
    if clang is None:
        print("SKIP: clang not installed")
        return 77
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        capture = root / "capture"
        subprocess.run([str(fixture), "capture", str(capture)], check=True)
        variants = [("0", "generic"), ("3", "generic")]
        if platform.machine().lower() in ("amd64", "x86_64"):
            variants.append(("3", "native"))
        for opt, march in variants:
            out = root / f"native-O{opt}-{march}"
            command = [sys.executable, str(builder), "--build-dir", str(recomp.parent),
                       "--capture", str(capture), "--out", str(out),
                       "--opt", opt, "--jobs", "2", "--march", march]
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode != 0:
                raise AssertionError(result.stdout + result.stderr)
            assert "3/4 captures compiled" in result.stdout, result.stdout
            assert (out / "vu1-build-target.txt").read_text().strip() == march
            replay = subprocess.run([str(fixture), "run", str(out)],
                                    check=True, capture_output=True, text=True)
            assert "VU1 AOT directory:" in replay.stderr, replay.stderr
            assert "fnv1a64=" in replay.stderr, replay.stderr
            other = "native" if march == "generic" else "generic"
            refused = subprocess.run(command[:-1] + [other],
                                     capture_output=True, text=True)
            assert refused.returncode != 0
            assert "VU1 target changed" in refused.stderr, refused.stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
