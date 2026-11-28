#!/usr/bin/env python3
"""Export exact hot VU1 captures and regenerated IR without running retail."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile


def fnv1a64(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return f"{value:016x}"


def hot_programs(archive):
    with zipfile.ZipFile(archive) as z:
        receipt = json.loads(z.read("comparison.json"))
        report = json.loads(z.read("profiled/report.json"))
        log = z.read("profiled/stderr.log").decode("utf-8", errors="replace")
    if not receipt.get("inputs_unchanged"):
        raise ValueError("profile inputs were not authenticated")
    inventory = receipt["inputs"]["vu1"]
    libraries = re.findall(r"^VU1 AOT library: (\S+) fnv1a64=([a-f0-9]{16})\s*$", log, re.M)
    if (len(libraries) != len(inventory["libraries"]) or
            set(name for name, _ in libraries) != set(inventory["libraries"])):
        raise ValueError("loaded library inventory differs from receipt")
    snapshots = [(int(d), int(f), json.loads(a)) for d, f, a in re.findall(
        r"^\[vu1\] worker profile snapshot dispatch=(\d+) fields=(\d+) aot=(\{[^\r\n]*\})\s*$",
        log, re.M)]
    if len(snapshots) not in (1, 2):
        raise ValueError("missing or ambiguous VU1 profile snapshots")
    begin, first_field, before = snapshots[0]
    if len(snapshots) == 2:
        end, last_field, after = snapshots[1]
    else:
        end, last_field, after = report["dispatches"], report["gs_fields_started"], report["vu1_aot"]
    if end <= begin or last_field <= first_field or after["interpreter_fallbacks"] != before["interpreter_fallbacks"]:
        raise ValueError("invalid recovered VU1 interval")
    def indexed(aot):
        rows = aot["program_profiles"]
        result = {(r["pc"], r["library_index"], r["code_fnv1a64"]): r for r in rows}
        if len(result) != len(rows):
            raise ValueError("duplicate program identity")
        return result
    old, new = indexed(before), indexed(after)
    if old.keys() != new.keys():
        raise ValueError("program identity changed")
    hot = []
    for key, row in new.items():
        pc, idx, code_hash = key
        pairs = row["profiled_pairs"] - old[key]["profiled_pairs"]
        native = row["native_pairs"] - old[key]["native_pairs"]
        elapsed = row["compiled_exclusive_ns"] - old[key]["compiled_exclusive_ns"]
        if pairs != native or min(pairs, elapsed) < 0 or bool(pairs) != bool(elapsed):
            raise ValueError("invalid profile coverage")
        if pairs:
            name, fingerprint = libraries[idx]
            if int(name.split("-", 1)[0], 16) != pc:
                raise ValueError("library entry PC differs from profile")
            hot.append(dict(pc=pc, code_hash=code_hash, library=name,
                            library_fnv1a64=fingerprint, pairs=pairs,
                            compiled_ns=elapsed, ns_per_pair=elapsed / pairs))
    if not hot:
        raise ValueError("no timed native VU1 programs")
    return inventory, sorted(hot, key=lambda r: r["compiled_ns"], reverse=True), {
        "begin_dispatch": begin, "end_dispatch": end, "begin_field": first_field,
        "end_field": last_field, "recovered_periodic_report": len(snapshots) == 1,
        "original_replay_error": receipt.get("error")}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--profile-archive", type=Path, required=True)
    p.add_argument("--capture", type=Path, action="append", required=True)
    p.add_argument("--inventory", type=Path, required=True)
    p.add_argument("--build-dir", type=Path, default=Path("build"))
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--top", type=int, default=2)
    args = p.parse_args()
    try:
        inventory, hot, interval = hot_programs(args.profile_archive)
        if not 1 <= args.top <= len(hot):
            raise ValueError("invalid hot program count")
        suffix = ".dll" if os.name == "nt" else ".so"
        files = sorted(args.inventory.glob("*" + suffix))
        if {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in files} != inventory["libraries"]:
            raise ValueError("native inventory has changed since profile")
        target = (args.inventory / "vu1-build-target.txt").read_text().strip()
        if target != inventory["target"] or target not in ("native", "generic"):
            raise ValueError("native target differs from profile")
        exe = "vu1_recomp.exe" if os.name == "nt" else "vu1_recomp"
        recomp = args.build_dir / exe
        if not recomp.is_file():
            recomp = args.build_dir / "Release" / exe
        clang = shutil.which("clang")
        if clang is None and os.name == "nt":
            candidate = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM/bin/clang.exe"
            if candidate.is_file():
                clang = str(candidate)
        if not clang or not recomp.is_file():
            raise ValueError("clang and built vu1_recomp are required")
        if args.out.exists() or args.out.with_suffix(".zip").exists():
            raise ValueError("output already exists")
        selected = []
        for row in hot[:args.top]:
            stem = Path(row["library"]).stem
            if not re.fullmatch(r"[0-9a-f]{4}-[0-9a-f]{16}-\d+", stem):
                raise ValueError("unexpected capture filename")
            sources = [directory / (stem + ".bin") for directory in args.capture]
            sources = [f for f in sources if f.is_file()]
            if not sources:
                raise ValueError("missing capture: " + stem + ".bin")
            data = sources[0].read_bytes()
            if len(data) != 16384 or fnv1a64(data) != stem.split("-")[1] or any(f.read_bytes() != data for f in sources):
                raise ValueError("capture identity mismatch: " + stem)
            selected.append((row, stem, data))
        args.out.mkdir(parents=True)
        result = {"interval": interval, "target": target, "programs": [],
                  "recomp_sha256": hashlib.sha256(recomp.read_bytes()).hexdigest(),
                  "clang_version": subprocess.check_output([clang, "--version"], text=True)}
        for row, stem, data in selected:
            directory = args.out / stem
            directory.mkdir()
            capture = directory / "code.bin"
            capture.write_bytes(data)
            ir = directory / "program.ll"
            subprocess.run([str(recomp.resolve()), str(capture.resolve()), hex(row["pc"]), str(ir.resolve())], check=True)
            flags = ["-O3", "-Wno-override-module"]
            if target == "native":
                flags += ["-march=native"]
            subprocess.run([clang, *flags, "-S", "-emit-llvm", str(ir), "-o", str(directory / "optimized.ll")], check=True)
            subprocess.run([clang, *flags, "-S", str(ir), "-o", str(directory / "native.s")], check=True)
            result["programs"].append(dict(row, capture_sha256=hashlib.sha256(data).hexdigest()))
        (args.out / "export.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print("Hot IR archive:", shutil.make_archive(str(args.out), "zip", args.out))
        return 0
    except (OSError, ValueError, KeyError, IndexError, subprocess.CalledProcessError) as error:
        print("Hot VU1 export refused:", error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
