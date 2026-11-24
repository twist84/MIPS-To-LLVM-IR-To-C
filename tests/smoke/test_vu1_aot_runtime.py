#!/usr/bin/env python3
"""End-to-end capture, IR lifting, object generation and VU1 native dispatch."""
import os
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
        for opt in ("0", "3"):
            out = root / f"native-O{opt}"
            result = subprocess.run(
                [sys.executable, str(builder), "--build-dir", str(recomp.parent),
                 "--capture", str(capture), "--out", str(out),
                 "--opt", opt, "--jobs", "2"], capture_output=True, text=True)
            if result.returncode != 0:
                raise AssertionError(result.stdout + result.stderr)
            assert "3/4 captures compiled" in result.stdout, result.stdout
            subprocess.run([str(fixture), "run", str(out)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
