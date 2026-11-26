#!/usr/bin/env python3
"""Matched, isolated graphics sweep: prior EE, VU1 AOT and runtime arms.

The caller supplies a retained PAD0 recording. Never rebuild, activate or
rewrite either native cache. All executions use the same dispatch budget
and input stream. Unprofiled measurements and correctness are distinct.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import shutil
import statistics
import sys

from replay_ee_dedup_experiment import (
    read_json, record_count, replay_environment, replay_summary, run_one,
    sha, terminal_budget, trace_comparison, validate_source_libraries,
)


def aot_identity(directory):
    marker = directory / "vu1-build-target.txt"
    target = marker.read_text(encoding="utf-8").strip()
    suffix = ".dll" if os.name == "nt" else ".so"
    libraries = {path.name: sha(path) for path in sorted(directory.glob("*" + suffix))}
    if target not in ("native", "generic") or not libraries:
        raise ValueError("invalid VU1 target marker or empty library set")
    return {"target": target, "libraries": libraries}


def architectural_report(path):
    report = read_json(path)
    # Host timing only, not the emulated device counters or guest state.
    report.pop("runtime_profile", None)
    aot = report.get("vu1_aot", {})
    for program in aot.get("program_profiles", []):
        for key in ("profiled_attempts", "profiled_pairs", "compiled_exclusive_ns"):
            program.pop(key, None)
    # All arms run synchronously; worker host profiling is irrelevant.
    worker = report.get("vu1_worker", {})
    for key in list(worker):
        if key.endswith("_ns"):
            worker.pop(key)
    return report


def validate_run(args, directory, reference, recording, polls, run):
    if run["returncode"] != 4 or not terminal_budget(directory, args.max_dispatches):
        raise ValueError("arm did not reach terminal dispatch budget")
    pad = replay_summary(directory / "stderr.log")
    if not pad or not pad["valid"] or pad["consumed"] != polls or pad["total"] != polls:
        raise ValueError("arm PAD0 replay diverged")
    trace = trace_comparison(reference / "stdout.log", directory / "stdout.log")
    if not trace["common_samples"] or trace["different_common_samples"] or trace["unpaired_samples"]:
        raise ValueError("arm sampled transitions differ")
    report = architectural_report(directory / "report.json")
    expected = architectural_report(reference / "report.json")
    changed = sorted(key for key in report.keys() | expected.keys()
                     if report.get(key) != expected.get(key))
    if changed:
        raise ValueError("terminal architectural report differs: " + ", ".join(changed))
    if sha(directory / "gs-webgpu.ppm") != sha(reference / "gs-webgpu.ppm"):
        raise ValueError("terminal GS screenshot differs")
    aot = report["vu1_aot"]
    if aot["interpreter_fallbacks"] or not aot["native_pairs"]:
        raise ValueError("VU1 coverage lost or no native VU1 work")
    if report["timing_observer_failed"] or report["m2_iop_guest_fault"]:
        raise ValueError("timing observer or IOP fault")
    return {"pad": pad, "trace": trace, "guest_instruction_attempts":
            report["guest_instruction_attempts"], "ee_cycles": report["ee_cycles"],
            "gs_fields": report["gs_fields_started"], "vu1_native_pairs": aot["native_pairs"],
            "vu1_native_runs": aot["native_runs"],
            "report_sha256": sha(directory / "report.json"),
            "screen_sha256": sha(directory / "gs-webgpu.ppm")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("runner", "previous-runner", "elf", "disc", "profile",
                   "baseline-source", "candidate-source", "baseline-receipt",
                   "candidate-receipt", "baseline-vu1", "candidate-vu1",
                   "recording", "reference", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--baseline-runner-sha256", required=True)
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
        protected = (args.profile, args.baseline_source, args.candidate_source,
                     args.baseline_vu1, args.candidate_vu1, args.disc, args.reference)
        if (args.output.exists() or args.output.with_suffix(".zip").exists() or
                any(args.output.is_relative_to(path) or path.is_relative_to(args.output)
                    for path in protected)):
            raise ValueError("use a fresh output directory outside protected inputs")
        original, before, old_hashes = validate_source_libraries(
            args.profile, args.baseline_source, args.elf, args.baseline_receipt)
        original2, after, new_hashes = validate_source_libraries(
            args.profile, args.candidate_source, args.elf, args.candidate_receipt)
        if original != original2 or old_hashes["baseline"] != new_hashes["baseline"]:
            raise ValueError("source comparisons use different original baselines")
        if sha(args.previous_runner) != args.baseline_runner_sha256:
            raise ValueError("previous runner is not the pinned prior executable")
        if sha(args.runner) == sha(args.previous_runner):
            raise ValueError("candidate runner is not rebuilt")
        old_aot, new_aot = map(aot_identity, (args.baseline_vu1, args.candidate_vu1))
        if (old_aot["target"] != new_aot["target"] or
                old_aot["libraries"].keys() != new_aot["libraries"].keys() or
                old_aot["libraries"] == new_aot["libraries"]):
            raise ValueError("VU1 target/capture inventory changed or libraries not rebuilt")
        polls = record_count(args.recording)
        if not terminal_budget(args.reference, args.max_dispatches):
            raise ValueError("prior reference did not reach requested budget")
        identities = {
            "previous_runner": sha(args.previous_runner), "runner": sha(args.runner),
            "elf": sha(args.elf), "before": sha(before), "after": sha(after),
            "baseline_receipt": sha(args.baseline_receipt),
            "candidate_receipt": sha(args.candidate_receipt),
            "recording": sha(args.recording),
            "reference_report": sha(args.reference / "report.json"),
            "reference_trace": sha(args.reference / "stdout.log"),
            "reference_screen": sha(args.reference / "gs-webgpu.ppm"),
        }
        env = replay_environment(Path(__file__).resolve().parents[1])
        env.update(SDBZ_VU1_ASYNC="0", SDBZ_GS_ASYNC="0", SDBZ_RUNTIME_PROFILE="0",
                   SDBZ_PAD0_REPLAY=str(args.recording))
        card = env.get("SDBZ_MEMCARD")
        if card:
            identities["memcard"] = sha(Path(card))
        arms = {
            "control": (args.previous_runner, before, args.baseline_vu1),
            "ee_only": (args.previous_runner, after, args.baseline_vu1),
            "vu1_only": (args.previous_runner, before, args.candidate_vu1),
            "runtime_only": (args.runner, before, args.baseline_vu1),
            "combined": (args.runner, after, args.candidate_vu1),
        }
        receipt = {"schema": "ee-graphics-sweep-v1", "inputs": identities,
                   "baseline_vu1": old_aot, "candidate_vu1": new_aot,
                   "environment": {k: v for k, v in sorted(env.items())
                                   if k.startswith(("SDBZ_", "EE_WEBGPU_")) or k == "PS2_SCALE"},
                   "recorded_polls": polls, "runs": {name: [] for name in arms},
                   "error": None}
        args.output.mkdir(parents=True)
        try:
            for repetition in range(args.repeats):
                # Alternate order after an initial control to limit drift.
                order = list(arms)
                if repetition % 2:
                    order.reverse()
                for name in order:
                    runner, library, aot = arms[name]
                    directory = args.output / f"{name}-{repetition + 1}"
                    arm_args = copy.copy(args)
                    arm_args.runner = runner
                    print(f"Graphics sweep {name} {repetition + 1}/{args.repeats}", flush=True)
                    run = run_one(arm_args, library, directory,
                                  dict(env, SDBZ_VU1_AOT=str(aot)))
                    receipt["runs"][name].append(run)
                    run.update(validate_run(args, directory, args.reference,
                                            args.recording, polls, run))
            medians = {name: statistics.median(run["wall_seconds"] for run in runs)
                       for name, runs in receipt["runs"].items()}
            receipt["median_wall_seconds"] = medians
            receipt["relative_throughput"] = {name: medians["control"] / value
                                              for name, value in medians.items()}
        except (OSError, ValueError, KeyError) as error:
            receipt["error"] = str(error)
        finally:
            checks = {"previous_runner": args.previous_runner, "runner": args.runner,
                      "elf": args.elf, "before": before, "after": after,
                      "baseline_receipt": args.baseline_receipt,
                      "candidate_receipt": args.candidate_receipt,
                      "recording": args.recording,
                      "reference_report": args.reference / "report.json",
                      "reference_trace": args.reference / "stdout.log",
                      "reference_screen": args.reference / "gs-webgpu.ppm"}
            if card:
                checks["memcard"] = Path(card)
            receipt["inputs_unchanged"] = (
                all(sha(path) == identities[key] for key, path in checks.items()) and
                aot_identity(args.baseline_vu1) == old_aot and
                aot_identity(args.candidate_vu1) == new_aot)
            (args.output / "comparison.json").write_text(
                json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
            print("Graphics sweep archive:",
                  shutil.make_archive(str(args.output), "zip", args.output), flush=True)
        if receipt["error"] or not receipt["inputs_unchanged"]:
            raise ValueError(receipt["error"] or "graphics sweep inputs changed")
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(f"Graphics sweep refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
