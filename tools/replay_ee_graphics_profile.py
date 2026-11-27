#!/usr/bin/env python3
"""G1A: matched synchronous profiling on/off on one validated graphics image."""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import statistics
import sys

from replay_ee_dedup_experiment import (
    read_json, record_count, replay_environment, run_one, sha,
    terminal_budget, validate_source_libraries, replay_summary, trace_comparison,
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


def iop_library_identity(directory):
    suffix = ".dll" if os.name == "nt" else ".so"
    if not directory.is_dir():
        raise ValueError("IOP AOT directory missing")
    libraries = {path.name: sha(path) for path in sorted(directory.iterdir())
                 if path.is_file() and path.suffix == suffix}
    if not libraries:
        raise ValueError("IOP AOT directory has no libraries")
    return libraries


def iop_capture_identity(directory):
    records = {}
    binaries = {path.stem for path in directory.glob("*.bin")}
    metadata = {path.stem for path in directory.glob("*.json")}
    if not metadata or metadata != binaries:
        raise ValueError("missing or unpaired IOP captures")
    for stem in sorted(metadata):
        descriptor = directory / (stem + ".json")
        binary = directory / (stem + ".bin")
        data = binary.read_bytes()
        meta = read_json(descriptor)
        digest = 14695981039346656037
        for byte in data:
            digest = ((digest ^ byte) * 1099511628211) & 0xffffffffffffffff
        if not data or len(data) != meta["text_size"] or digest != int(meta["hash"], 16):
            raise ValueError("IOP capture size/hash mismatch: " + stem)
        records[stem] = {"metadata": meta, "metadata_sha256": sha(descriptor),
                         "binary_sha256": sha(binary)}
    return records


def iop_aot_observation(stderr, expected_libraries):
    with stderr.open(encoding="utf-8", errors="replace") as stream:
        lines = [line.strip() for line in stream if line.startswith("IOP AOT:")]
    if not expected_libraries:
        if lines:
            raise ValueError("unexpected IOP AOT in interpreter arm")
        return None
    pattern = (r"IOP AOT: modules=(\d+) active=(\d+) entries=(\d+) "
               r"instructions=(\d+) translated_ms=([\d.]+) refresh_ms=([\d.]+)")
    matches = [match for line in lines if (match := re.fullmatch(pattern, line))]
    if len(matches) != 1:
        raise ValueError("missing terminal IOP AOT statistics")
    modules, active, entries, instructions = map(int, matches[0].groups()[:4])
    if modules != expected_libraries or not 0 < active <= modules or not entries or not instructions:
        raise ValueError("IOP AOT incomplete load or no native retirement")
    return {"modules": modules, "active": active, "entries": entries,
            "instructions": instructions,
            "translated_ms": float(matches[0][5]), "refresh_ms": float(matches[0][6])}


def iop_exit_profile(stderr, observation):
    """Validate diagnostic rows against the existing terminal AOT totals."""
    header = None
    rows = []
    keys = set()
    pattern = (r"IOP EXIT ROW: base=([0-9a-f]{8}) hash=([0-9a-f]{16}) "
               r"entry=([0-9a-f]{8}) resume=([0-9a-f]{8}) reason=(\d+) "
               r"delay=(\d+) entries=(\d+) retired=(\d+) budget=(\d+) "
               r"horizon=(\d+) zero=(\d+)")
    with stderr.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            line = line.strip()
            if line.startswith("IOP EXIT PROFILE:"):
                match = re.fullmatch(r"IOP EXIT PROFILE: keys=(\d+) dropped=(\d+)", line)
                if header is not None or not match:
                    raise ValueError("invalid or duplicate IOP exit profile header")
                header = tuple(map(int, match.groups()))
            elif line.startswith("IOP EXIT ROW:"):
                match = re.fullmatch(pattern, line)
                if header is None or not match:
                    raise ValueError("invalid IOP exit profile row")
                values = [int(v, 16) for v in match.groups()[:4]]
                values += list(map(int, match.groups()[4:]))
                key = tuple(values[:6])
                entries, retired, budget, horizon, zero = values[6:]
                if (key in keys or key[4] not in (0, 1) or key[5] not in (0, 1)
                        or entries < 1 or max(budget, horizon, zero) > entries
                        or budget + zero > entries or retired < entries - zero):
                    raise ValueError("inconsistent IOP exit profile counters")
                keys.add(key)
                rows.append(dict(zip(("base", "hash", "entry", "resume", "reason",
                                      "delay", "entries", "retired", "budget",
                                      "horizon", "zero"), values)))
    if observation is None:
        if header is not None or rows:
            raise ValueError("unexpected IOP exit profile in interpreter arm")
        return None
    if header != (len(rows), 0) or not rows:
        raise ValueError("missing, incomplete or truncated IOP exit profile")
    if (sum(r["entries"] for r in rows) != observation["entries"]
            or sum(r["retired"] for r in rows) != observation["instructions"]):
        raise ValueError("IOP exit profile totals differ from native totals")
    return {"schema": 1, "keys": len(rows), "dropped": 0,
            "entries": observation["entries"], "retired": observation["instructions"],
            "rows": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("runner", "elf", "disc", "profile", "source", "source-receipt",
                   "vu1", "recording", "reference", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--capture-iop", type=Path,
                        help="single interpreter replay capturing linked IRX text outside archive")
    parser.add_argument("--candidate-iop-aot", type=Path,
                        help="compare interpreter against one IOP AOT library directory")
    parser.add_argument("--compare-iop-chain", action="store_true",
                        help="Compare chain off/on using identical IOP DLLs and runner")
    parser.add_argument("--profile-iop-exits", action="store_true",
                        help="Diagnostic native exit attribution on IOP candidate only")
    parser.add_argument("--candidate-runner", type=Path,
                        help="matched alternative runner; same EE and VU1 images")
    parser.add_argument("--control-runner-sha256",
                        help="pinned control runner hash; required for runner and IOP comparisons")
    parser.add_argument("--candidate-vu1", type=Path,
                        help="matched alternative VU1 inventory; compare with same runner and EE")
    parser.add_argument("--candidate-source", type=Path,
                        help="matched alternative EE source-lowered image; same runner and VU1")
    parser.add_argument("--candidate-source-receipt", type=Path,
                        help="source-lowering comparison receipt for the candidate EE image")
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
        if args.compare_iop_chain and (args.candidate_iop_aot is None or args.profile_iop_exits):
            raise ValueError("--compare-iop-chain requires --candidate-iop-aot without exit profiling")
        if args.profile_iop_exits and args.candidate_iop_aot is None:
            raise ValueError("--profile-iop-exits requires --candidate-iop-aot")
        if args.profile_vu1 and args.candidate_vu1 is None:
            raise ValueError("--profile-vu1 requires --candidate-vu1")
        if bool(args.candidate_source) != bool(args.candidate_source_receipt):
            raise ValueError("candidate EE source and receipt must be supplied together")
        if sum(bool(x) for x in (args.candidate_source, args.candidate_vu1,
                                  args.candidate_runner, args.candidate_iop_aot,
                                  args.capture_iop)) > 1:
            raise ValueError("compare one changed component at a time")
        if (args.candidate_runner or args.capture_iop or args.candidate_iop_aot) and not args.control_runner_sha256:
            raise ValueError("comparison or IOP capture requires pinned control runner hash")
        if args.control_runner_sha256 and sha(args.runner) != args.control_runner_sha256:
            raise ValueError("control runner hash differs from pinned runner")
        if args.candidate_runner and sha(args.candidate_runner) == sha(args.runner):
            raise ValueError("candidate runner must differ from control")
        protected = (args.profile, args.source, args.vu1, args.disc, args.reference)
        if args.candidate_iop_aot is not None:
            protected += (args.candidate_iop_aot,)
        if args.capture_iop is not None:
            if args.repeats != 1 or args.capture_iop.exists() or any(
                    args.capture_iop.is_relative_to(path) or path.is_relative_to(args.capture_iop)
                    for path in (*protected, args.output, args.runner, args.recording,
                                 args.elf, args.source_receipt, args.output.with_suffix(".zip"))):
                raise ValueError("capture requires one replay and a fresh separate directory")
            protected += (args.capture_iop,)
        if args.candidate_source:
            protected += (args.candidate_source, args.candidate_source_receipt)
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
        candidate_library = None
        if args.candidate_source:
            candidate_original, candidate_library, candidate_hashes = validate_source_libraries(
                args.profile, args.candidate_source, args.elf, args.candidate_source_receipt)
            if (candidate_original != original or candidate_library == library or
                    candidate_hashes["dedup"] == sha(library)):
                raise ValueError("candidate EE image must differ from control")
        iop_inventory = (iop_library_identity(args.candidate_iop_aot)
                         if args.candidate_iop_aot is not None else None)
        comparison = (candidate_inventory is not None or candidate_library is not None
                      or args.candidate_runner is not None or iop_inventory is not None)
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
        env.pop("SDBZ_IOP_AOT_EXIT_PROFILE", None)
        env.pop("SDBZ_IOP_AOT_CHAIN", None)
        if args.capture_iop or iop_inventory is not None:
            for key in ("SDBZ_IOP_AOT", "SDBZ_IOP_AOT_VERIFY", "SDBZ_IOP_AOT_CAPTURE",
                        "SDBZ_IOP_AOT_BUILD_OUT"):
                env.pop(key, None)
        paths = {"runner": args.runner, "elf": args.elf, "library": library,
                 "original_library": original, "source_receipt": args.source_receipt,
                 "recording": args.recording, "reference_report": args.reference / "report.json",
                 "reference_trace": args.reference / "stdout.log",
                 "reference_screen": args.reference / "gs-webgpu.ppm"}
        if candidate_library is not None:
            paths["candidate_library"] = candidate_library
            paths["candidate_source_receipt"] = args.candidate_source_receipt
        if args.candidate_runner is not None:
            paths["candidate_runner"] = args.candidate_runner
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
        if candidate_inventory is not None:
            receipt["schema"] = "ee-graphics-vu1-comparison-v1"
            receipt["candidate_vu1_inventory"] = candidate_inventory
            receipt["profile_both_arms"] = args.profile_vu1
        elif candidate_library is not None:
            receipt["schema"] = "ee-graphics-ee-comparison-v1"
            receipt["profile_both_arms"] = False
        elif args.candidate_runner is not None:
            receipt["schema"] = "ee-graphics-runner-comparison-v1"
            receipt["profile_both_arms"] = False
        elif iop_inventory is not None:
            receipt["schema"] = "ee-graphics-iop-aot-comparison-v1"
            receipt["candidate_iop_inventory"] = iop_inventory
            receipt["profile_iop_exits"] = args.profile_iop_exits
            receipt["compare_iop_chain"] = args.compare_iop_chain
            if args.compare_iop_chain:
                receipt["schema"] = "ee-graphics-iop-chain-comparison-v1"
            receipt["profile_both_arms"] = False
        elif args.capture_iop is not None:
            receipt["schema"] = "ee-graphics-iop-capture-v1"
            receipt["runs"] = {"capture": []}
            receipt["capture_directory"] = str(args.capture_iop)
        args.output.mkdir(parents=True)
        try:
            if args.capture_iop:
                args.capture_iop.mkdir(parents=True)
            for repetition in range(args.repeats):
                order = ("control", second_arm) if repetition % 2 == 0 else (second_arm, "control")
                if args.capture_iop:
                    order = ("capture",)
                for name in order:
                    enabled = args.profile_vu1 if comparison else name == "profiled"
                    aot = args.candidate_vu1 if name == "candidate" and candidate_inventory else args.vu1
                    image = candidate_library if name == "candidate" and candidate_library else library
                    directory = args.output / f"{name}-{repetition + 1}"
                    print(f"Graphics {name} {repetition + 1}/{args.repeats}", flush=True)
                    arm_args = argparse.Namespace(**vars(args))
                    if name == "candidate" and args.candidate_runner:
                        arm_args.runner = args.candidate_runner
                    arm_env = dict(env, SDBZ_RUNTIME_PROFILE="1" if enabled else "0",
                                   SDBZ_VU1_AOT=str(aot))
                    if args.capture_iop:
                        arm_env["SDBZ_IOP_AOT_CAPTURE"] = str(args.capture_iop)
                    if iop_inventory is not None and (name == "candidate" or args.compare_iop_chain):
                        arm_env["SDBZ_IOP_AOT"] = str(args.candidate_iop_aot)
                        if args.profile_iop_exits:
                            arm_env["SDBZ_IOP_AOT_EXIT_PROFILE"] = "1"
                        if args.compare_iop_chain and name == "candidate":
                            arm_env["SDBZ_IOP_AOT_CHAIN"] = "1"
                    run = run_one(arm_args, image, directory, arm_env)
                    receipt["runs"][name].append(run)
                    run["directory"] = directory.name
                    run["profile_enabled"] = enabled
                    if args.compare_iop_chain:
                        run["chain_enabled"] = name == "candidate"
                    if args.capture_iop or iop_inventory is not None:
                        # Partial observations remain useful even when a strict
                        # PAD replay stop prevents terminal AOT statistics.
                        if run["returncode"] != 4:
                            stderr = directory / "stderr.log"
                            if stderr.is_file():
                                run["pad"] = replay_summary(stderr)
                                with stderr.open(encoding="utf-8", errors="replace") as stream:
                                    run["iop_aot_partial_log"] = [
                                        line.strip() for line in stream
                                        if line.startswith("IOP AOT:")]
                            stdout = directory / "stdout.log"
                            if stdout.is_file():
                                run["partial_trace"] = trace_comparison(
                                    args.reference / "stdout.log", stdout)
                            raise ValueError(
                                f"{name} exited {run['returncode']} before terminal budget")
                        run["iop_aot"] = iop_aot_observation(directory / "stderr.log",
                            len(iop_inventory) if iop_inventory and
                            (name == "candidate" or args.compare_iop_chain) else 0)
                        if args.compare_iop_chain and run["iop_aot"]["active"] != len(iop_inventory):
                            raise ValueError("chain comparison requires all IOP libraries active")
                        if args.profile_iop_exits:
                            exits = iop_exit_profile(directory / "stderr.log", run["iop_aot"])
                            if exits is not None:
                                (directory / "iop-exit-profile.json").write_text(
                                    json.dumps(exits, indent=2) + "\n", encoding="utf-8")
                                run["iop_exit_profile"] = {
                                    key: exits[key] for key in ("keys", "dropped", "entries", "retired")}
                    run.update(validate_run(args, directory, args.reference, args.recording, polls, run))
                    report = read_json(directory / "report.json")
                    require_match(report, reference)
                    if report["vu1_worker"]["enabled"]:
                        raise ValueError("asynchronous VU1 not permitted in G1A")
                    run["attribution"] = extract_profile(report, directory / "stderr.log", enabled)
            if args.capture_iop:
                receipt["captures"] = iop_capture_identity(args.capture_iop)
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
            elif not args.capture_iop:
                receipt["profiling_overhead_ratio"] = medians["profiled"] / medians["control"] - 1
            if args.compare_iop_chain:
                pairs = []
                for control, candidate in zip(receipt["runs"]["control"], receipt["runs"]["candidate"]):
                    a, b = control["iop_aot"], candidate["iop_aot"]
                    if a["instructions"] != b["instructions"]:
                        raise ValueError("chain comparison native retirement changed")
                    saved = a["entries"] - b["entries"]
                    if saved <= 0:
                        raise ValueError("chain comparison did not demonstrate fewer native entries")
                    pairs.append({"control_entries": a["entries"], "candidate_entries": b["entries"],
                                  "entries_saved": saved, "retired": a["instructions"]})
                receipt["chain_pairs"] = pairs
            receipt["correctness_accepted"] = True
        except (OSError, ValueError, KeyError, TypeError) as error:
            receipt["error"] = str(error)
        finally:
            receipt["inputs_unchanged"] = (
                all(sha(path) == hashes[key] for key, path in paths.items()) and
                aot_identity(args.vu1) == inventory and
                (candidate_inventory is None or aot_identity(args.candidate_vu1) == candidate_inventory) and
                (iop_inventory is None or iop_library_identity(args.candidate_iop_aot) == iop_inventory))
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
