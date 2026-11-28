#!/usr/bin/env python3
"""Compile captured VU1 programs into opt-in native libraries.

Unsupported captures remain interpreter-only. Existing libraries are not
deleted; the runtime checks exact microcode bytes before every native entry.
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def instrument_mul_corrections(ir):
    counts = (
        "@vu_mul_profile_counts = internal global [3 x i64] zeroinitializer, align 8\n"
        "declare i4 @llvm.ctpop.i4(i4)\n")
    ir = ir.replace("define internal ", counts + "define internal ", 1)
    start = re.search(
        r"(?m)^define internal [^\n]* @vu_mul4\([^\n]*\) alwaysinline \{\nentry:\n",
        ir)
    if start is None:
        raise ValueError("generated VU1 IR is missing the expected vu_mul4 entry")
    call_counter = """  %vu_mul_profile_call_ptr = getelementptr inbounds [3 x i64], ptr @vu_mul_profile_counts, i64 0, i64 0
  %vu_mul_profile_call_old = load i64, ptr %vu_mul_profile_call_ptr, align 8
  %vu_mul_profile_call_new = add i64 %vu_mul_profile_call_old, 1
  store i64 %vu_mul_profile_call_new, ptr %vu_mul_profile_call_ptr, align 8
"""
    ir = ir[:start.end()] + call_counter + ir[start.end():]
    correct = re.search(r"(?m)^correct:\n", ir[start.start():])
    if correct is None:
        raise ValueError("generated VU1 IR is missing the vu_mul4 correction path")
    correct_offset = start.start() + correct.end()
    correction_counter = """  %vu_mul_profile_correct_ptr = getelementptr inbounds [3 x i64], ptr @vu_mul_profile_counts, i64 0, i64 1
  %vu_mul_profile_correct_old = load i64, ptr %vu_mul_profile_correct_ptr, align 8
  %vu_mul_profile_correct_new = add i64 %vu_mul_profile_correct_old, 1
  store i64 %vu_mul_profile_correct_new, ptr %vu_mul_profile_correct_ptr, align 8
  %vu_mul_profile_lane_count = call i4 @llvm.ctpop.i4(i4 %mask)
  %vu_mul_profile_lane_wide = zext i4 %vu_mul_profile_lane_count to i64
  %vu_mul_profile_lanes_ptr = getelementptr inbounds [3 x i64], ptr @vu_mul_profile_counts, i64 0, i64 2
  %vu_mul_profile_lanes_old = load i64, ptr %vu_mul_profile_lanes_ptr, align 8
  %vu_mul_profile_lanes_new = add i64 %vu_mul_profile_lanes_old, %vu_mul_profile_lane_wide
  store i64 %vu_mul_profile_lanes_new, ptr %vu_mul_profile_lanes_ptr, align 8
"""
    ir = ir[:correct_offset] + correction_counter + ir[correct_offset:]
    accessor = """
define dso_local dllexport void @ee_vu1_mul_profile(ptr %out) nounwind {
entry:
  %count_ptr = getelementptr inbounds [3 x i64], ptr @vu_mul_profile_counts, i64 0, i64 0
  %count = load i64, ptr %count_ptr, align 8
  store i64 %count, ptr %out, align 8
  %correction_ptr = getelementptr inbounds [3 x i64], ptr @vu_mul_profile_counts, i64 0, i64 1
  %corrections = load i64, ptr %correction_ptr, align 8
  %correction_out = getelementptr inbounds i64, ptr %out, i64 1
  store i64 %corrections, ptr %correction_out, align 8
  %lanes_ptr = getelementptr inbounds [3 x i64], ptr @vu_mul_profile_counts, i64 0, i64 2
  %lanes = load i64, ptr %lanes_ptr, align 8
  %lanes_out = getelementptr inbounds i64, ptr %out, i64 2
  store i64 %lanes, ptr %lanes_out, align 8
  ret void
}
"""
    info_export = re.search(r"(?m)^define dso_local dllexport ptr @ee_vu1_aot_info\(", ir)
    if info_export is None:
        ir += accessor
    else:
        ir = ir[:info_export.start()] + accessor + ir[info_export.start():]
    return ir


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
    parser.add_argument("--profile-mul-corrections", action="store_true",
                        help="instrument vector FMAC multiply and exact-correction counts in this separate diagnostic inventory")
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
    profile_marker = out / "vu1-profile-mode.txt"
    profile_mode = "mul-corrections" if args.profile_mul_corrections else "off"
    if (args.profile_mul_corrections and not profile_marker.exists() and
            any(out.glob("*" + suffix))):
        parser.error("profiled VU1 libraries require a fresh output directory")
    if profile_marker.exists() and profile_marker.read_text(encoding="utf-8").strip() != profile_mode:
        parser.error("VU1 profile mode changed; use a separate output directory")
    profile_marker.write_text(profile_mode + "\n", encoding="utf-8")
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
            if args.profile_mul_corrections:
                ir.write_text(instrument_mul_corrections(ir.read_text(encoding="utf-8")),
                              encoding="utf-8")
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
