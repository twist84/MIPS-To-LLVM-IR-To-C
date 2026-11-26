#!/usr/bin/env python3
"""G1A: matched synchronous profiling on/off on one validated graphics image."""
import argparse
import json
import os
from pathlib import Path
import shutil
import statistics
import sys

from replay_ee_dedup_experiment import (
    read_json, record_count, replay_environment, run_one, sha,
    terminal_budget, validate_source_libraries,
)
from replay_ee_graphics_sweep import aot_identity, validate_run


# All fields are emitted exclusively by host-side diagnostic collectors.
PROGRAM_PROFILE_FIELDS = ("profiled_attempts", "profiled_pairs", "compiled_exclusive_ns")
WORKER_TIMING_FIELDS = (
    "worker_execution_ns", "worker_queue_ns", "compiled_exclusive_ns",
    "xgkick_exclusive_ns", "controller_wait_ns", "ee_native_overlap_wall_ns",
)
INTERVAL_PREFIX = "EE profile interval: "


def normalize(report):
    # Round trip to avoid mutating any archived data.
    report = json.loads(json.dumps(report))
    profile = report.pop("runtime_profile")
    if profile["schema"] != "ee-runtime-profile-v1":
        raise ValueError("unknown runtime profile schema")
    for program in report["vu1_aot"]["program_profiles"]:
        for field in PROGRAM_PROFILE_FIELDS:
            program.pop(field)
    worker = report["vu1_worker"]
    for field in WORKER_TIMING_FIELDS:
        worker.pop(field)
    return report


def require_match(actual, expected):
    actual, expected = normalize(actual), normalize(expected)
    changed = sorted(key for key in actual.keys() | expected.keys()
                     if key not in actual or key not in expected or actual[key] != expected[key])
    if changed:
        raise ValueError("terminal architectural report differs: " + ", ".join(changed))


def check_accounting(profile):
    if sum(row["exclusive_ns"] for row in profile["subsystems"].values()) != profile["covered_ns"]:
        raise ValueError("exclusive scope sum differs from covered time")
    if profile["wall_ns"] != profile["covered_ns"] + profile["uninstrumented_ns"]:
        raise ValueError("covered and uninstrumented time do not sum to wall")
    for name, row in profile["subsystems"].items():
        if any(type(row[key]) is not int or row[key] < 0
               for key in ("calls", "inclusive_ns", "exclusive_ns", "work")):
            raise ValueError("invalid counters: " + name)
        if row["exclusive_ns"] > row["inclusive_ns"]:
            raise ValueError("exclusive exceeds inclusive: " + name)
        if not row["calls"] and any(row[key] for key in ("inclusive_ns", "exclusive_ns", "work")):
            raise ValueError("nonzero measurements without calls: " + name)


def extract_profile(report, stderr, enabled):
    p = report["runtime_profile"]
    if p["schema"] != "ee-runtime-profile-v1" or p["enabled"] is not enabled:
        raise ValueError("unexpected runtime profile mode")
    with stderr.open(encoding="utf-8", errors="replace") as stream:
        intervals = [json.loads(line[len(INTERVAL_PREFIX):]) for line in stream
                     if line.startswith(INTERVAL_PREFIX)]
    if not enabled:
        if intervals:
            raise ValueError("unexpected profile intervals in control")
        return None
    if p["scope"] != "runner_thread_completed_scopes" or p["clock"] != "steady_clock":
        raise ValueError("unknown timing domain")
    if p["dispatches"] != report["dispatches"] or p["guest_fields"] != report["gs_fields_started"]:
        raise ValueError("profile endpoints differ from terminal report")
    check_accounting(p)
    if not intervals:
        raise ValueError("missing interval records")
    previous = dict(dispatch=0, cycle=0, fields=0, presentations=0)
    totals = {name: dict(calls=0, inclusive_ns=0, exclusive_ns=0, work=0)
              for name in p["subsystems"]}
    for interval in intervals:
        if interval["schema"] != "ee-runtime-profile-interval-v1":
            raise ValueError("unknown interval schema")
        for key, value in previous.items():
            if interval["start_" + key] != value:
                raise ValueError("discontinuous interval: " + key)
            previous[key] = interval["end_" + key]
        check_accounting(interval)
        if interval["subsystems"].keys() != totals.keys():
            raise ValueError("interval subsystem inventory differs")
        for name, row in interval["subsystems"].items():
            for key in totals[name]:
                totals[name][key] += row[key]
    if previous != dict(dispatch=report["dispatches"], cycle=report["ee_cycles"],
                        fields=report["gs_fields_started"], presentations=p["presentations"]):
        raise ValueError("profile intervals do not reach terminal report")
    for name, row in totals.items():
        if any(value != p["subsystems"][name][key] for key, value in row.items()):
            raise ValueError("interval totals differ: " + name)
    programs = report["vu1_aot"]["program_profiles"]
    if not any(row["profiled_attempts"] for row in programs):
        raise ValueError("no profiled VU1 program attempts")
    return {
        "runner_thread": p,
        "vu1_programs": programs,
        "vu1_worker": report["vu1_worker"],
        "intervals": intervals,
        "unattributed": ["vu0_sync", "cop1", "mmi", "vu1_dependency_scheduling"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("runner", "elf", "disc", "profile", "source", "source-receipt",
                   "vu1", "recording", "reference", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--max-dispatches", type=int, default=40000000)
    parser.add_argument("--timeout-seconds", type=int, default=600)
    parser.add_argument("--repeats", type=int, default=2)
    args = parser.parse_args()
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.resolve())
    try:
        if not 1 <= args.repeats <= 10 or not 1 <= args.max_dispatches <= 0xffffffff:
            raise ValueError("invalid repetitions or dispatch limit")
        protected = (args.profile, args.source, args.vu1, args.disc, args.reference)
        if (args.output.exists() or args.output.with_suffix(".zip").exists() or
                any(args.output.is_relative_to(path) or path.is_relative_to(args.output)
                    for path in protected)):
            raise ValueError("use a fresh output outside protected inputs")
        original, library, _ = validate_source_libraries(
            args.profile, args.source, args.elf, args.source_receipt)
        inventory = aot_identity(args.vu1)
        if not terminal_budget(args.reference, args.max_dispatches):
            raise ValueError("reference did not reach terminal dispatch budget")
        reference = read_json(args.reference / "report.json")
        polls = record_count(args.recording)
        env = replay_environment(Path(__file__).resolve().parents[1])
        env.update(SDBZ_VU1_ASYNC="0", SDBZ_GS_ASYNC="0",
                   SDBZ_PAD0_REPLAY=str(args.recording), SDBZ_VU1_AOT=str(args.vu1),
                   SDBZ_PROFILE_PCS="0", SDBZ_REPORT_BOUNDARY_TIMING="0",
                   SDBZ_REPORT_HOTSPOTS="0")
        paths = {"runner": args.runner, "elf": args.elf, "library": library,
                 "original_library": original, "source_receipt": args.source_receipt,
                 "recording": args.recording, "reference_report": args.reference / "report.json",
                 "reference_trace": args.reference / "stdout.log",
                 "reference_screen": args.reference / "gs-webgpu.ppm"}
        if env.get("SDBZ_MEMCARD"):
            paths["memcard"] = Path(env["SDBZ_MEMCARD"])
        hashes = {key: sha(path) for key, path in paths.items()}
        receipt = {"schema": "ee-graphics-profile-receipt-v1",
                   "inputs": hashes, "vu1_inventory": inventory,
                   "environment": {k: v for k, v in sorted(env.items())
                                   if k.startswith(("SDBZ_", "EE_WEBGPU_")) or k == "PS2_SCALE"},
                   "recorded_polls": polls, "max_dispatches": args.max_dispatches,
                   "mode": "synchronous_runner_thread",
                   "runs": {"control": [], "profiled": []},
                   "correctness_accepted": False, "performance_accepted": False,
                   "error": None}
        args.output.mkdir(parents=True)
        try:
            for repetition in range(args.repeats):
                order = ("control", "profiled") if repetition % 2 == 0 else ("profiled", "control")
                for name in order:
                    enabled = name == "profiled"
                    directory = args.output / f"{name}-{repetition + 1}"
                    print(f"G1A {name} {repetition + 1}/{args.repeats}", flush=True)
                    run = run_one(args, library, directory,
                                  dict(env, SDBZ_RUNTIME_PROFILE="1" if enabled else "0"))
                    receipt["runs"][name].append(run)
                    run["directory"] = directory.name
                    run["profile_enabled"] = enabled
                    run.update(validate_run(args, directory, args.reference, args.recording, polls, run))
                    report = read_json(directory / "report.json")
                    require_match(report, reference)
                    if report["vu1_worker"]["enabled"]:
                        raise ValueError("asynchronous VU1 not permitted in G1A")
                    run["attribution"] = extract_profile(report, directory / "stderr.log", enabled)
            receipt["median_wall_seconds"] = {
                name: statistics.median(run["wall_seconds"] for run in runs)
                for name, runs in receipt["runs"].items()}
            medians = receipt["median_wall_seconds"]
            receipt["profiling_overhead_ratio"] = medians["profiled"] / medians["control"] - 1
            receipt["correctness_accepted"] = True
        except (OSError, ValueError, KeyError, TypeError) as error:
            receipt["error"] = str(error)
        finally:
            receipt["inputs_unchanged"] = (
                all(sha(path) == hashes[key] for key, path in paths.items()) and
                aot_identity(args.vu1) == inventory)
            if not receipt["inputs_unchanged"]:
                receipt["correctness_accepted"] = False
            (args.output / "profiling-receipt.json").write_text(
                json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
            print("G1 archive:", shutil.make_archive(str(args.output), "zip", args.output), flush=True)
        if receipt["error"] or not receipt["inputs_unchanged"]:
            raise ValueError(receipt["error"] or "profiling inputs changed")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"G1 refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
