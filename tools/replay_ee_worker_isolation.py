#!/usr/bin/env python3
"""Diagnostic EE/VU1/GS worker isolation; never rewrite a native cache."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys

from replay_ee_dedup_experiment import (
    GRAPHICS_WORKLOAD_KEYS, VU1_WORKLOAD_KEYS, lean_overrides, read_json,
    record_count, replay_environment, replay_summary, report_options,
    run_one, sha, terminal_budget, validate_raster_library,
)
from replay_ee_graphics_sweep import aot_identity


ARMS = (
    ("both_before", True, True),
    ("vu1_only", True, False),
    ("gs_only", False, True),
    ("sync", False, False),
    ("both_after", True, True),
)
PROGRESS = re.compile(
    r"EE progress: dispatched=(\d+).*?elapsed_ms=([\d.]+).*? fields=(\d+)")


def combat_window(log, start, end):
    samples = {}
    for line in log.splitlines():
        match = PROGRESS.match(line)
        if match:
            dispatch, elapsed, fields = match.groups()
            dispatch = int(dispatch)
            if start <= dispatch <= end:
                samples[dispatch] = (float(elapsed), int(fields))
    if not samples or start not in samples:
        raise ValueError("missing combat start progress sample")
    last = max(samples)
    # Progress is sampled every 8192 dispatches. Do not extrapolate.
    if end - last >= 8192 or last == start:
        raise ValueError("missing combat end progress sample")
    t0, f0 = samples[start]
    t1, f1 = samples[last]
    if t1 <= t0 or f1 <= f0:
        raise ValueError("invalid combat progress")
    return {"start_dispatch": start, "end_dispatch": last,
            "start_field": f0, "end_field": f1, "fields": f1 - f0,
            "seconds": (t1 - t0) / 1000.0,
            "fields_per_second": (f1 - f0) * 1000.0 / (t1 - t0)}


def gs_worker_stats(log):
    lines = re.findall(r"(?m)^GS async worker: (.+)$", log)
    if not lines:
        return None
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", lines[-1])}


def digest_payload(payload):
    return hashlib.sha256(json.dumps(payload, sort_keys=True,
        separators=(",", ":")).encode("utf-8")).hexdigest()


def inspect_arm(args, directory, vu1, gs, polls, run, inventory):
    if run["returncode"] != 4 or not terminal_budget(directory, args.max_dispatches):
        raise ValueError("arm did not reach terminal dispatch budget")
    pad = replay_summary(directory / "stderr.log")
    if pad != {"consumed": polls, "total": polls, "valid": True}:
        raise ValueError("arm PAD replay diverged")
    report = read_json(directory / "report.json")
    log = (directory / "stderr.log").read_text(encoding="utf-8", errors="replace")
    worker = report["vu1_worker"]
    gs_stats = gs_worker_stats(log)
    if worker["enabled"] is not vu1 or (gs_stats is not None) != gs:
        raise ValueError("actual worker configuration differs")
    if worker["host_failures"] or report["timing_observer_failed"] or report["m2_iop_guest_fault"]:
        raise ValueError("runtime reported a host or guest failure")
    if vu1 and (not worker["submitted_jobs"] or worker["native_overlap_profile"]):
        raise ValueError("VU1 worker inactive or overlap profiling unexpectedly enabled")
    if gs and (gs_stats["healthy"] != 1 or not gs_stats["submitted"] or
               gs_stats["completed"] != gs_stats["submitted"]):
        raise ValueError("GS worker not healthy or not drained")
    if report_options(directory / "stdout.log") != {"hotspots": False, "boundary": False}:
        raise ValueError("lean report settings not confirmed")
    if report["m2_runtime"]["enabled"]:
        raise ValueError("hotspot collector unexpectedly enabled")
    aot = report["vu1_aot"]
    if (not aot["enabled"] or aot["programs"] != len(inventory["libraries"]) or
            not aot["native_pairs"] or aot["interpreter_fallbacks"]):
        raise ValueError("native VU1 coverage lost")
    presentation = report["m2_gs_presentation"]
    if (not presentation["gpu_enabled"] or not presentation["window_present_succeeded"] or
            presentation["window_present_succeeded"] != report["gs_fields_started"]):
        raise ValueError("GPU presentation incomplete")
    payload = {key: report[key] for key in GRAPHICS_WORKLOAD_KEYS}
    payload["vu1_aot"] = {key: aot[key] for key in VU1_WORKLOAD_KEYS}
    return {"pad": pad, "combat": combat_window(log, args.combat_start, args.max_dispatches),
            "fields": report["gs_fields_started"], "guest_attempts": report["guest_instruction_attempts"],
            "draws": presentation["gpu_committed_draws"],
            "presentations": presentation["window_present_succeeded"],
            "graphics_workload_sha256": digest_payload(payload),
            "section_sha256": {key: digest_payload(value) for key, value in payload.items()},
            "report_sha256": sha(directory / "report.json"),
            "image_sha256": sha(directory / "gs-webgpu.ppm"),
            "vu1_worker": worker, "gs_worker": gs_stats,
            "vu1_native_pairs": aot["native_pairs"],
            "vu1_native_runs": aot["native_runs"],
            "runtime_profile": report["runtime_profile"]}


def compare_arms(arms):
    reference = arms["both_before"]["evidence"]
    base = reference["combat"]
    comparison = {}
    for name, arm in arms.items():
        evidence = arm["evidence"]
        window = evidence["combat"]
        matched = all(window[key] == base[key] for key in
                      ("start_dispatch", "end_dispatch", "start_field", "end_field"))
        comparison[name] = {
            "combat_window_matches": matched,
            "combat_fields_per_second": window["fields_per_second"],
            "graphics_workload_matches": evidence["graphics_workload_sha256"] == reference["graphics_workload_sha256"],
            "final_image_matches": evidence["image_sha256"] == reference["image_sha256"],
            "differing_workload_sections": [key for key, value in evidence["section_sha256"].items()
                                          if value != reference["section_sha256"][key]],
            "combat_gain_vs_first_both_percent": (
                100.0 * (window["fields_per_second"] / base["fields_per_second"] - 1.0)
                if matched else None),
        }
    return comparison


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("runner", "elf", "disc", "profile", "recording", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--max-dispatches", type=int, default=40000000)
    parser.add_argument("--combat-start", type=int, default=24002560)
    parser.add_argument("--timeout-seconds", type=int, default=600)
    args = parser.parse_args()
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.resolve())
    try:
        if (args.combat_start < 8192 or args.combat_start % 8192 or
                not args.combat_start + 8192 <= args.max_dispatches <= 0xffffffff or
                args.timeout_seconds < 1):
            raise ValueError("invalid combat window or timeout")
        library, _, hashes = validate_raster_library(args.profile, args.elf)
        env = replay_environment(Path(__file__).resolve().parents[1])
        env.update(lean_overrides("all"))
        env.update(SDBZ_RUNTIME_PROFILE="0", SDBZ_PROFILE_PCS="0",
                   PS2_RASTER_TILE="16", PS2_RASTER_WORKGROUP="16",
                   PS2_RASTER_LAZY_SETUP="0", PS2_RASTER_EARLY_DEPTH="0",
                   PS2_RASTER_BILINEAR_COORDS="0", PS2_RASTER_PLOT_ADDRESS_CACHE="0",
                   PS2_RASTER_DENSE_WORKGROUP="0", PS2_GS_PACKED_DIRECT="0",
                   PS2_GPU_REGULAR_TIMESTAMPS="1", SDBZ_PAD0_REPLAY=str(args.recording))
        env.pop("PS2_GPU_PROFILE", None)
        vu1_dir = Path(env["SDBZ_VU1_AOT"]).resolve()
        inventory = aot_identity(vu1_dir)
        protected = (args.profile, args.disc, vu1_dir, args.recording, args.runner, args.elf)
        if (args.output.exists() or args.output.with_suffix(".zip").exists() or
                any(args.output.is_relative_to(path) or path.is_relative_to(args.output)
                    for path in protected)):
            raise ValueError("use a fresh output directory outside all inputs")
        if not args.runner.is_file() or not args.disc.is_dir():
            raise ValueError("runner or disc missing")
        polls = record_count(args.recording)
        card = Path(env["SDBZ_MEMCARD"]).resolve() if env.get("SDBZ_MEMCARD") else None
        inputs = {"runner": sha(args.runner), "elf": sha(args.elf),
                  "library": hashes["baseline"], "recording": sha(args.recording),
                  "vu1": inventory, "memcard": sha(card) if card else None}
        receipt = {"mode": "diagnostic_worker_isolation",
                   "architectural_equivalence_proven": False, "inputs": inputs,
                   "recorded_polls": polls, "arms": {}, "error": None,
                   "environment": {key: value for key, value in sorted(env.items())
                                   if key.startswith(("SDBZ_", "PS2_", "EE_WEBGPU_"))}}
        args.output.mkdir(parents=True)
        try:
            for name, vu1, gs in ARMS:
                settings = dict(env, SDBZ_VU1_ASYNC=str(int(vu1)), SDBZ_GS_ASYNC=str(int(gs)))
                print("Worker isolation:", name, flush=True)
                directory = args.output / name
                arm = {"vu1_async": vu1, "gs_async": gs}
                receipt["arms"][name] = arm
                arm["run"] = run_one(args, library, directory, settings)
                try:
                    arm["evidence"] = inspect_arm(args, directory, vu1, gs, polls,
                                                  arm["run"], inventory)
                except (OSError, KeyError, ValueError) as error:
                    arm["error"] = str(error)
            if all("evidence" in arm for arm in receipt["arms"].values()):
                receipt["comparison"] = compare_arms(receipt["arms"])
            else:
                receipt["error"] = "one or more arms failed workload validation"
        finally:
            try:
                receipt["inputs_unchanged"] = (
                    sha(args.runner) == inputs["runner"] and sha(args.elf) == inputs["elf"] and
                    sha(library) == inputs["library"] and sha(args.recording) == inputs["recording"] and
                    aot_identity(vu1_dir) == inputs["vu1"] and
                    (not card or sha(card) == inputs["memcard"]))
            except (OSError, ValueError):
                receipt["inputs_unchanged"] = False
            (args.output / "comparison.json").write_text(
                json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
            print("Comparison archive:", shutil.make_archive(str(args.output), "zip", args.output))
        return 1 if receipt["error"] or not receipt["inputs_unchanged"] else 0
    except (OSError, KeyError, ValueError) as error:
        print("Worker isolation refused:", error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
