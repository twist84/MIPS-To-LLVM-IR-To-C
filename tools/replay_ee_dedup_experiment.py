#!/usr/bin/env python3
"""Matched baseline/deduplicated native runner replay; never updates EE caches."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def validate_libraries(profile, experiment, elf):
    name = ("game-translated.dll" if os.name == "nt" else
            "libgame-translated.dylib" if sys.platform == "darwin" else
            "libgame-translated.so")
    baseline, candidate = profile / name, experiment / name
    translation = read_json(profile / "translation-cache.json")
    plan = read_json(experiment / "dedup-plan.json")
    inputs = translation.get("inputs", {})
    if inputs.get("ELF") != sha(elf):
        raise ValueError("boot ELF differs from baseline translation provenance")
    if inputs.get("native_alu_certificate_mode") != hashlib.sha256(b"0").hexdigest():
        raise ValueError("baseline must have native ALU certificates disabled")
    if (plan.get("mode") != "experimental_owner_coalescing" or
            not plan.get("runtime_validation_required") or
            plan.get("metadata_sha256") != translation.get("metadata_sha256")):
        raise ValueError("experiment and normal translation metadata do not match")
    hashes = {"baseline": sha(baseline), "dedup": sha(candidate)}
    if (hashes["baseline"] != translation.get("library_sha256") or
            hashes["dedup"] != plan.get("linked_library_sha256") or
            candidate.stat().st_size != plan.get("linked_library_bytes")):
        raise ValueError("library identity differs from recorded manifest")
    if baseline.resolve() == candidate.resolve() or hashes["baseline"] == hashes["dedup"]:
        raise ValueError("two distinct verified native libraries required")
    return baseline, candidate, hashes


def record_count(path):
    count = 0
    footer = None
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if line.startswith("PAD0 input: "):
                fields = line.split()[2:]
                if (footer is not None or len(fields) != 10 or
                        not all(field.isdecimal() for field in fields) or
                        int(fields[0]) != count + 1):
                    raise ValueError("invalid PAD0 record sequence")
                count += 1
            elif line.startswith("PAD0 input summary: record "):
                match = re.fullmatch(r"PAD0 input summary: record (\d+)\s*", line)
                if not match or footer is not None:
                    raise ValueError("invalid PAD0 record footer")
                footer = int(match[1])
    if count == 0 or count != footer:
        raise ValueError("baseline did not finish a complete nonempty PAD0 recording")
    return count


def replay_summary(path):
    result = None
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = re.fullmatch(
                r"PAD0 input summary: replay consumed=(\d+) total=(\d+) valid=([01])\s*", line)
            if match:
                result = {"consumed": int(match[1]), "total": int(match[2]),
                          "valid": match[3] == "1"}
    return result


def replay_environment(root):
    env = os.environ.copy()
    # Match the P40 preset's core runtime options, while leaving opt-in
    # memory/IOP/diagnostic choices from the caller intact.
    env.update({
        "SDBZ_SPEED_PERCENT": "0", "SDBZ_HOST_AUDIO": "0",
        "SDBZ_TRACE_BUFFERED": "1", "SDBZ_REPORT_SNAPSHOT_MS": "2000",
        "SDBZ_REPORT_PROGRESS_EVERY": "8192", "SDBZ_DISPATCH_TRACE_EVERY": "1024",
        "SDBZ_WINDOW_PUMP_STRIDE": "64", "SDBZ_EE_DIRECT_DISPATCH": "1",
        "SDBZ_VU1_ASYNC": "1", "SDBZ_GS_ASYNC": "1", "SDBZ_GS_GPU_VRAM": "1",
        "SDBZ_GIF_IMAGE_DIGEST": "0", "SDBZ_NATIVE_ALU_CERTIFICATES": "0",
        "PS2_SCALE": "1",
    })
    env.setdefault("SDBZ_RUNTIME_PROFILE", "1")
    env.setdefault("SDBZ_IDLE_SKIP", "1")
    env.setdefault("SDBZ_RTC_FIXED_UTC", "1167609600")
    env.setdefault("SDBZ_VU1_AOT", str(root / "build" / "vu1-native-v20"))
    for key in ("SDBZ_PAD0_CROSS_FIRST_POLL", "SDBZ_PAD0_CROSS_POLL_COUNT",
                "SDBZ_PAD0_SCRIPT", "SDBZ_PAD0_RECORD", "SDBZ_PAD0_REPLAY",
                "SDBZ_VU1_AOT_CAPTURE", "SDBZ_VU1_AOT_CAPTURE_MISSES",
                "SDBZ_IOP_AOT_CAPTURE", "SDBZ_IOP_AOT_BUILD_OUT"):
        env.pop(key, None)
    return env


def run_one(args, library, directory, env):
    directory.mkdir()
    command = [str(args.runner), str(args.elf), str(library),
               "--disc-folder=" + str(args.disc),
               "--hle-startup", "--kseg-ram", "--memcard-discard-writes",
               "--ee-clock=reference-compat", "--loop-budget=1000000",
               "--loop-yield-resumes=16", "--sif-poll-limit=2048",
               f"--max-dispatches={args.max_dispatches}", "--trace",
               "--report=" + str(directory / "report.json"),
               "--gs-webgpu-snapshot=" + str(directory / "gs-webgpu.ppm"),
               "--gs-webgpu-window", "--speed-percent=0"]
    start = time.monotonic()
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        try:
            result = subprocess.run(command, env=env, stdout=out, stderr=err,
                                    timeout=args.timeout_seconds, check=False)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = None
    return {"command": command, "returncode": code, "wall_seconds": time.monotonic() - start,
            "report_present": (directory / "report.json").is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--disc", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--experiment", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-dispatches", type=int, default=40000000)
    parser.add_argument("--timeout-seconds", type=int, default=600)
    args = parser.parse_args()
    for key in ("runner", "elf", "disc", "profile", "experiment", "output"):
        setattr(args, key, getattr(args, key).resolve())
    try:
        if (args.output.exists() or args.output.with_suffix(".zip").exists() or
                any(args.output.is_relative_to(path) for path in
                    (args.profile, args.experiment, args.disc))):
            raise ValueError("use a fresh output directory outside the cache and disc")
        if not args.runner.is_file() or not args.elf.is_file() or not args.disc.is_dir():
            raise ValueError("runner, ELF or disc path missing")
        if not 1 <= args.max_dispatches <= 0xffffffff or args.timeout_seconds < 1:
            raise ValueError("invalid dispatch or timeout budget")
        baseline, candidate, hashes = validate_libraries(args.profile, args.experiment, args.elf)
        env = replay_environment(Path(__file__).resolve().parents[1])
        inputs = {"runner_sha256": sha(args.runner), "elf_sha256": sha(args.elf),
                  "library_sha256": hashes}
        card = env.get("SDBZ_MEMCARD")
        if card:
            inputs["memcard_sha256"] = sha(Path(card))
        receipt = {"inputs": inputs, "environment": {
            k: v for k, v in sorted(env.items()) if k.startswith(("SDBZ_", "EE_WEBGPU_")) or k == "PS2_SCALE"},
            "baseline": None, "dedup": None, "pad_replay": None,
            "error": None, "libraries_unchanged": False}
        args.output.mkdir(parents=True)
        try:
            print("Baseline: play to the dispatch limit; PAD0 input is recorded.", flush=True)
            receipt["baseline"] = run_one(args, baseline, args.output / "baseline",
                                          dict(env, SDBZ_PAD0_RECORD="1"))
            if not receipt["baseline"]["report_present"] or receipt["baseline"]["returncode"] is None:
                raise ValueError("baseline failed to produce a terminal report")
            recording = args.output / "baseline" / "stderr.log"
            receipt["recorded_polls"] = record_count(recording)
            print("Deduplicated: replaying the recorded PAD0 polls.", flush=True)
            receipt["dedup"] = run_one(args, candidate, args.output / "dedup",
                                       dict(env, SDBZ_PAD0_REPLAY=str(recording)))
            receipt["pad_replay"] = replay_summary(args.output / "dedup" / "stderr.log")
            replay = receipt["pad_replay"]
            if (not replay or not replay["valid"] or replay["consumed"] != receipt["recorded_polls"]
                    or replay["total"] != receipt["recorded_polls"]):
                raise ValueError("PAD0 replay diverged or ended early; not a matched comparison")
            if not receipt["dedup"]["report_present"] or receipt["dedup"]["returncode"] is None:
                raise ValueError("deduplicated replay failed to produce a terminal report")
        except (OSError, ValueError) as error:
            receipt["error"] = str(error)
        finally:
            receipt["libraries_unchanged"] = (
                sha(baseline) == hashes["baseline"] and sha(candidate) == hashes["dedup"])
            if card:
                receipt["memcard_unchanged"] = sha(Path(card)) == inputs["memcard_sha256"]
            (args.output / "comparison.json").write_text(
                json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
            archive = shutil.make_archive(str(args.output), "zip", args.output)
            print("Comparison archive:", archive, flush=True)
        if receipt["error"] or not receipt["libraries_unchanged"] or not receipt.get("memcard_unchanged", True):
            print("Replay requires investigation:", receipt["error"], file=sys.stderr)
            return 1
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(f"EE dedup replay refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
