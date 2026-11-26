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
from replay_ee_graphics_sweep import aot_identity, validate_aot_pair, validate_run


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
    unattributed = ["cop1", "mmi", "vu1_dependency_scheduling"]
    vu0_scopes = {"vu0_sync_halted", "vu0_sync_cmsar1",
                  "vu0_sync_micro", "vu0_sync_invalid"}
    present = vu0_scopes & p["subsystems"].keys()
    if present and present != vu0_scopes:
        raise ValueError("incomplete VU0 callback scope inventory")
    if not present:
        unattributed.insert(0, "vu0_sync")
    else:
        # The wrapper measures installed runtime callbacks, not direct
        # periodic device-service calls or arithmetic outside callbacks.
        unattributed.insert(0, "vu0_sync_noncallback")
    return {
        "runner_thread": p,
        "vu1_programs": programs,
        "vu1_worker": report["vu1_worker"],
        "intervals": intervals,
        "unattributed": unattributed,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("runner", "elf", "disc", "profile", "source", "source-receipt",
                   "vu1", "recording", "reference", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--candidate-vu1", type=Path,
                        help="matched alternative VU1 inventory; compare with same runner and EE")
    parser.add_argument("--profile-vu1", action="store_true",
                        help="profile both VU1 comparison arms (diagnostic only)")
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
        if args.profile_vu1 and args.candidate_vu1 is None:
            raise ValueError("--profile-vu1 requires --candidate-vu1")
        protected = (args.profile, args.source, args.vu1, args.disc, args.reference)
        if args.candidate_vu1 is not None:
            protected += (args.candidate_vu1,)
        if (args.output.exists() or args.output.with_suffix(".zip").exists() or
                any(args.output.is_relative_to(path) or path.is_relative_to(args.output)
                    for path in protected)):
            raise ValueError("use a fresh output outside protected inputs")
        original, library, _ = validate_source_libraries(
            args.profile, args.source, args.elf, args.source_receipt)
        inventory = aot_identity(args.vu1)
        candidate_inventory = None
        if args.candidate_vu1 is not None:
            candidate_inventory = aot_identity(args.candidate_vu1)
            validate_aot_pair(inventory, candidate_inventory)
        comparison = candidate_inventory is not None
        second_arm = "candidate" if comparison else "profiled"
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
                   "runs": {"control": [], second_arm: []},
                   "correctness_accepted": False, "performance_accepted": False,
                   "error": None}
        if comparison:
            receipt["schema"] = "ee-graphics-vu1-comparison-v1"
            receipt["candidate_vu1_inventory"] = candidate_inventory
            receipt["profile_both_arms"] = args.profile_vu1
        args.output.mkdir(parents=True)
        try:
            for repetition in range(args.repeats):
                order = ("control", second_arm) if repetition % 2 == 0 else (second_arm, "control")
                for name in order:
                    enabled = args.profile_vu1 if comparison else name == "profiled"
                    aot = args.candidate_vu1 if name == "candidate" else args.vu1
                    directory = args.output / f"{name}-{repetition + 1}"
                    print(f"Graphics {name} {repetition + 1}/{args.repeats}", flush=True)
                    run = run_one(args, library, directory,
                                  dict(env, SDBZ_RUNTIME_PROFILE="1" if enabled else "0",
                                       SDBZ_VU1_AOT=str(aot)))
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
            if comparison:
                receipt["relative_throughput"] = medians["control"] / medians["candidate"]
                receipt["paired_wall_seconds"] = [
                    {"control": control["wall_seconds"], "candidate": candidate["wall_seconds"]}
                    for control, candidate in zip(receipt["runs"]["control"],
                                                  receipt["runs"]["candidate"])]
            else:
                receipt["profiling_overhead_ratio"] = medians["profiled"] / medians["control"] - 1
            receipt["correctness_accepted"] = True
        except (OSError, ValueError, KeyError, TypeError) as error:
            receipt["error"] = str(error)
        finally:
            receipt["inputs_unchanged"] = (
                all(sha(path) == hashes[key] for key, path in paths.items()) and
                aot_identity(args.vu1) == inventory and
                (not comparison or aot_identity(args.candidate_vu1) == candidate_inventory))
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
