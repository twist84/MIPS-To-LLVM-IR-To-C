#!/usr/bin/env python3
"""End-to-end capture, IR lifting, object generation and VU1 native dispatch."""
import importlib.util
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def verify_windows_stack_probe(builder, clang, root):
    if os.name != "nt":
        return
    spec = importlib.util.spec_from_file_location("vu1_aot_builder", builder)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    ir = root / "stack_probe.ll"
    ir.write_text("""
define dllexport i32 @test_stack_probe(i64 %size) {
entry:
  %buf = alloca i8, i64 %size, align 16
  %end = sub i64 %size, 1
  %last = getelementptr i8, ptr %buf, i64 %end
  store volatile i8 17, ptr %buf
  store volatile i8 25, ptr %last
  %a = load volatile i8, ptr %buf
  %b = load volatile i8, ptr %last
  %x = zext i8 %a to i32
  %y = zext i8 %b to i32
  %sum = add i32 %x, %y
  ret i32 %sum
}
""", encoding="utf-8")
    library = root / "stack_probe.dll"
    module.build_library(clang, ir, library, "3", "native")
    # Run in a subprocess so Windows can release the DLL before cleanup.
    subprocess.run([sys.executable, "-c",
                    "import ctypes,sys; d=ctypes.CDLL(sys.argv[1]); "
                    "f=d.test_stack_probe; f.argtypes=[ctypes.c_uint64]; "
                    "f.restype=ctypes.c_uint32; assert f(32768)==42",
                    str(library)], check=True)


def verify_program_profiles(stdout):
    def read_profile(label):
        prefix = f"VU1 AOT {label} profile: "
        report = json.loads(next(line[len(prefix):] for line in stdout.splitlines()
                                 if line.startswith(prefix)))
        rows = report["program_profiles"]
        assert len(rows) == report["programs"] == 3
        assert sorted(row["library_index"] for row in rows) == list(range(3))
        assert all(len(row["code_fnv1a64"]) == 16 for row in rows)
        for key in ("native_runs", "native_pairs"):
            assert sum(row[key] for row in rows) == report[key]
        assert all(row["attempts"] >= row["native_runs"] and
                   row["attempted_pairs"] >= row["native_pairs"] and
                   row["profiled_attempts"] <= row["attempts"] and
                   row["profiled_pairs"] <= row["attempted_pairs"]
                   for row in rows)
        return report, rows

    sync, sync_rows = read_profile("synchronous")
    assert sync["native_runs"] == 6
    assert sum(row["attempts"] for row in sync_rows) == 8
    assert all(row["profiled_attempts"] == row["profiled_pairs"] ==
               row["compiled_exclusive_ns"] == 0 for row in sync_rows)
    worker, worker_rows = read_profile("worker")
    assert worker["native_runs"] == 3
    assert sum(row["attempts"] for row in worker_rows) == 3
    assert all(row["profiled_attempts"] == row["attempts"] and
               row["profiled_pairs"] == row["attempted_pairs"] for row in worker_rows)
    assert sum(row["compiled_exclusive_ns"] for row in worker_rows) > 0
    assert {(row["pc"], row["code_fnv1a64"]) for row in sync_rows} == {
        (row["pc"], row["code_fnv1a64"]) for row in worker_rows}


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
        verify_windows_stack_probe(builder, clang, root)
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
            verify_program_profiles(replay.stdout)
            other = "native" if march == "generic" else "generic"
            refused = subprocess.run(command[:-1] + [other],
                                     capture_output=True, text=True)
            assert refused.returncode != 0
            assert "VU1 target changed" in refused.stderr, refused.stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
