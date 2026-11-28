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
    plans = [path for path in (experiment / "dedup-plan.json", experiment / "size-plan.json", experiment / "helper-plan.json") if path.is_file()]
    if len(plans) != 1:
        raise ValueError("expected one experiment plan")
    plan = read_json(plans[0])
    inputs = translation.get("inputs", {})
    if inputs.get("ELF") != sha(elf):
        raise ValueError("boot ELF differs from baseline translation provenance")
    if inputs.get("native_alu_certificate_mode") != hashlib.sha256(b"0").hexdigest():
        raise ValueError("baseline must have native ALU certificates disabled")
    if (plan.get("mode") not in ("experimental_owner_coalescing", "experimental_selective_native_size", "experimental_shared_observation", "experimental_shared_counter", "experimental_shared_quadword", "experimental_shared_cop1_mul", "experimental_shared_cop1_add", "experimental_selective_scalar_memory", "experimental_expanded_scalar_memory", "experimental_shared_scalar_memory", "experimental_residual_shared_scalar", "experimental_selective_alignment_merge", "experimental_expanded_alignment_merge", "experimental_selective_entry_rotation", "experimental_expanded_entry_rotation", "experimental_single_use_fmac_inline") or
            not plan.get("runtime_validation_required") or
            plan.get("metadata_sha256") != translation.get("metadata_sha256")):
        raise ValueError("experiment and normal translation metadata do not match")
    hashes = {"baseline": sha(baseline), "dedup": sha(candidate)}
    if (hashes["baseline"] != translation.get("library_sha256") or
            hashes["dedup"] != plan.get("linked_library_sha256") or
            candidate.stat().st_size != plan.get("linked_library_bytes")):
        raise ValueError("library identity differs from recorded manifest")
    if (plan.get("mode") == "experimental_single_use_fmac_inline" and
            plan.get("baseline_library_sha256") != hashes["baseline"]):
        raise ValueError("FMAC pilot baseline library differs")
    if baseline.resolve() == candidate.resolve() or hashes["baseline"] == hashes["dedup"]:
        raise ValueError("two distinct verified native libraries required")
    return baseline, candidate, hashes


def validate_source_libraries(profile, experiment, elf, receipt_path):
    """Authenticate a separately regenerated O5 image, not an overlay."""
    receipt = read_json(receipt_path)
    translation_path = profile / "translation-cache.json"
    translation = read_json(translation_path)
    baseline = profile / "game-translated.dll"
    candidate = experiment / baseline.name
    meta = experiment / "ir" / "ee_functions.json"
    baseline_objects = profile / "objects" / "objects.json"
    candidate_objects = experiment / "objects" / "objects.json"
    hashes = {"baseline": sha(baseline), "dedup": sha(candidate)}
    rows = receipt["modules"]
    before = {row["module"]: row["before_sha256"] for row in rows}
    after = {row["module"]: row["after_sha256"] for row in rows}
    if (receipt.get("schema") != "ee-source-lowering-comparison-v1" or
            receipt.get("stage") != "linked_o5_candidate" or
            receipt.get("profile") != 5 or receipt.get("error") or
            receipt.get("runtime_validation_required") is not True or
            receipt.get("retail_replay_performed") is not False or
            receipt["elf_sha256"] != sha(elf) or
            translation["inputs"]["ELF"] != receipt["elf_sha256"] or
            translation["inputs"]["native_alu_certificate_mode"] !=
            hashlib.sha256(b"0").hexdigest() or translation["emit_opt"] != 5 or
            sha(translation_path) != receipt["baseline_translation_sha256"] or
            translation["metadata_sha256"] != receipt["baseline_metadata_sha256"] or
            sha(meta) != receipt["candidate_metadata_sha256"] or
            sha(baseline_objects) != receipt["baseline_object_manifest_sha256"] or
            sha(candidate_objects) != receipt["candidate_object_manifest_sha256"] or
            any(receipt["ownership_changes"].values()) or receipt["guest_pc_changes"] or
            len(before) != len(rows) or len(rows) != receipt["functions"] or
            before != translation["ir_hashes"] or after != receipt["candidate_ir_hashes"] or
            hashes["baseline"] != translation["library_sha256"] or
            hashes["baseline"] != receipt["baseline_library"]["sha256"] or
            hashes["dedup"] != receipt["candidate_library"]["sha256"] or
            baseline.stat().st_size != receipt["baseline_library"]["bytes"] or
            candidate.stat().st_size != receipt["candidate_library"]["bytes"] or
            baseline.resolve() == candidate.resolve() or hashes["baseline"] == hashes["dedup"]):
        raise ValueError("source-lowering replay provenance differs")
    for manifest_path in (baseline_objects, candidate_objects):
        manifest = read_json(manifest_path)
        if (not manifest.get("complete") or manifest.get("optimization_level") != 5 or
                any(manifest.get(key) != value for key, value in receipt["native_target"].items())):
            raise ValueError("source-lowering replay native profile differs")
    return baseline, candidate, hashes


def validate_source_reference(prior, profile, candidate, baseline, hashes, expected_sha):
    """An accepted mixed-profile image is an independent reference arm."""
    parent = read_json(prior / "helper-plan.json")
    translation = read_json(profile / "translation-cache.json")
    library = prior / candidate.name
    digest = sha(library)
    if (parent.get("mode") != "experimental_expanded_entry_rotation" or
            parent.get("metadata_sha256") != translation["metadata_sha256"] or
            parent.get("native_target") != read_json(profile / "objects" / "objects.json")["native_target"] or
            parent.get("linked_library_sha256") != digest or
            parent.get("linked_library_bytes") != library.stat().st_size or
            digest != expected_sha or digest in hashes.values() or
            library.resolve() in (baseline.resolve(), candidate.resolve())):
        raise ValueError("accepted source-lowering reference image differs")
    return library, digest


def validate_prior(experiment, prior, candidate, baseline, hashes):
    """Verify the immediate parent of a cumulative helper experiment."""
    current = read_json(experiment / "helper-plan.json")
    parent = read_json(prior / "helper-plan.json")
    library = prior / candidate.name
    digest = sha(library)
    parent_modes = {"experimental_shared_counter": "experimental_shared_observation",
                    "experimental_shared_quadword": "experimental_shared_counter",
                    "experimental_shared_cop1_mul": "experimental_shared_quadword",
                    "experimental_shared_cop1_add": "experimental_shared_cop1_mul",
                    "experimental_selective_scalar_memory": "experimental_shared_cop1_add",
                    "experimental_expanded_scalar_memory": "experimental_selective_scalar_memory",
                    "experimental_shared_scalar_memory": "experimental_expanded_scalar_memory",
                    "experimental_residual_shared_scalar": "experimental_shared_scalar_memory",
                    "experimental_selective_alignment_merge": "experimental_residual_shared_scalar",
                    "experimental_expanded_alignment_merge": "experimental_selective_alignment_merge",
                    "experimental_selective_entry_rotation": "experimental_expanded_alignment_merge",
                    "experimental_expanded_entry_rotation": "experimental_selective_entry_rotation"}
    if (current.get("mode") not in parent_modes or
            parent.get("mode") != parent_modes.get(current.get("mode")) or
            not parent.get("runtime_validation_required") or
            parent.get("metadata_sha256") != current.get("metadata_sha256") or
            parent.get("native_target") != current.get("native_target") or
            current.get("source_manifest_sha256") != sha(prior / "objects" / "objects.json") or
            current.get("prior_library_sha256") != digest or
            parent.get("linked_library_sha256") != digest or
            parent.get("linked_library_bytes") != library.stat().st_size or
            library.resolve() in (candidate.resolve(), baseline.resolve()) or
            digest in hashes.values()):
        raise ValueError("prior image does not match helper experiment provenance")
    return library, digest


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


def trace_comparison(left, right):
    """Compare sampled transitions without interpreting missing samples as equality."""
    pattern = re.compile(r"dispatch=(\d+) pc=(0x[0-9a-fA-F]+) -> (0x[0-9a-fA-F]+) reason=(\w+)\((\d+)\) status=(\d+)")
    def read(path):
        result = {}
        with path.open(encoding="utf-8", errors="replace") as stream:
            for line in stream:
                match = pattern.match(line)
                if match:
                    result[int(match[1])] = list(match.groups()[1:])
        return result
    a, b = read(left), read(right)
    common = a.keys() & b.keys()
    mismatches = [index for index in common if a[index] != b[index]]
    presence = a.keys() ^ b.keys()
    first = min(mismatches, default=None)
    return {"common_samples": len(common), "different_common_samples": len(mismatches),
            "first_different_dispatch": first,
            "first_different_left": a[first] if first is not None else None,
            "first_different_right": b[first] if first is not None else None,
            "first_unpaired_dispatch": min(presence, default=None),
            "unpaired_samples": len(presence)}


def terminal_budget(directory, limit):
    path = directory / "report.json"
    if not path.is_file():
        return False
    report = read_json(path)
    return report.get("snapshot_kind") == "stopped" and report.get("dispatches") == limit


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
    parser.add_argument("--prior-experiment", type=Path,
                        help="also replay the verified immediate helper-experiment parent")
    parser.add_argument("--source-comparison", type=Path,
                        help="linked source-lowering comparison receipt instead of overlay plan")
    parser.add_argument("--prior-source-comparison", type=Path,
                        help="receipt for the previous correctness-validated source O5 candidate")
    parser.add_argument("--reference-sha256",
                        help="accepted mixed-profile reference DLL SHA-256 for source comparison")
    parser.add_argument("--control", action="store_true",
                        help="Replay the recording on the original DLL before testing dedup")
    parser.add_argument("--sync", action="store_true",
                        help="Disable async VU1 and GS for all three runs")
    parser.add_argument("--max-dispatches", type=int, default=40000000)
    parser.add_argument("--timeout-seconds", type=int, default=600)
    args = parser.parse_args()
    for key in ("runner", "elf", "disc", "profile", "experiment", "output"):
        setattr(args, key, getattr(args, key).resolve())
    try:
        prior_dir = args.prior_experiment.resolve() if args.prior_experiment else None
        if prior_dir is not None and (not args.control or not args.sync):
            raise ValueError("prior experiment comparison requires --control --sync")
        if (args.output.exists() or args.output.with_suffix(".zip").exists() or
                any(args.output.is_relative_to(path) for path in
                    (args.profile, args.experiment, args.disc)) or
                (prior_dir is not None and args.output.is_relative_to(prior_dir))):
            raise ValueError("use a fresh output directory outside the cache and disc")
        if not args.runner.is_file() or not args.elf.is_file() or not args.disc.is_dir():
            raise ValueError("runner, ELF or disc path missing")
        if not 1 <= args.max_dispatches <= 0xffffffff or args.timeout_seconds < 1:
            raise ValueError("invalid dispatch or timeout budget")
        if args.source_comparison is not None:
            if prior_dir is None or not args.reference_sha256:
                raise ValueError("source comparison requires a pinned accepted reference and --control --sync")
            baseline, candidate, hashes = validate_source_libraries(
                args.profile, args.experiment, args.elf, args.source_comparison)
            if args.prior_source_comparison is not None:
                original, prior_library, prior_hashes = validate_source_libraries(
                    args.profile, prior_dir, args.elf, args.prior_source_comparison)
                digest = prior_hashes["dedup"]
                if (original != baseline or prior_hashes["baseline"] != hashes["baseline"] or
                        digest != args.reference_sha256 or digest in hashes.values() or
                        prior_library.resolve() == candidate.resolve()):
                    raise ValueError("previous source O5 reference identity differs")
                hashes["prior"] = digest
            else:
                prior_library, hashes["prior"] = validate_source_reference(
                    prior_dir, args.profile, candidate, baseline, hashes, args.reference_sha256)
        else:
            if args.reference_sha256 or args.prior_source_comparison:
                raise ValueError("source reference options require --source-comparison")
            baseline, candidate, hashes = validate_libraries(args.profile, args.experiment, args.elf)
            prior_library = None
            if prior_dir is not None:
                prior_library, hashes["prior"] = validate_prior(
                    args.experiment, prior_dir, candidate, baseline, hashes)
        env = replay_environment(Path(__file__).resolve().parents[1])
        if args.sync:
            env.update(SDBZ_VU1_ASYNC="0", SDBZ_GS_ASYNC="0")
        inputs = {"runner_sha256": sha(args.runner), "elf_sha256": sha(args.elf),
                  "library_sha256": hashes}
        if args.source_comparison is not None:
            inputs["source_comparison_sha256"] = sha(args.source_comparison)
        if args.prior_source_comparison is not None:
            inputs["prior_source_comparison_sha256"] = sha(args.prior_source_comparison)
        card = env.get("SDBZ_MEMCARD")
        if card:
            inputs["memcard_sha256"] = sha(Path(card))
        receipt = {"inputs": inputs, "environment": {
            k: v for k, v in sorted(env.items()) if k.startswith(("SDBZ_", "EE_WEBGPU_")) or k == "PS2_SCALE"},
            "baseline": None, "control": None, "dedup": None, "pad_replay": None,
            "control_pad_replay": None, "control_trace": None, "dedup_trace": None,
            "error": None, "libraries_unchanged": False}
        if prior_library is not None:
            receipt.update(prior=None, prior_pad_replay=None, prior_trace=None)
        args.output.mkdir(parents=True)
        try:
            print("Baseline: play to the dispatch limit; PAD0 input is recorded.", flush=True)
            receipt["baseline"] = run_one(args, baseline, args.output / "baseline",
                                          dict(env, SDBZ_PAD0_RECORD="1"))
            if receipt["baseline"]["returncode"] != 4 or not terminal_budget(args.output / "baseline", args.max_dispatches):
                raise ValueError("baseline failed to produce a terminal report")
            recording = args.output / "baseline" / "stderr.log"
            receipt["recorded_polls"] = record_count(recording)
            if args.control:
                print("Control: replaying the recording on the original DLL.", flush=True)
                receipt["control"] = run_one(args, baseline, args.output / "control",
                                             dict(env, SDBZ_PAD0_REPLAY=str(recording)))
                receipt["control_pad_replay"] = replay_summary(args.output / "control" / "stderr.log")
                receipt["control_trace"] = trace_comparison(
                    args.output / "baseline" / "stdout.log", args.output / "control" / "stdout.log")
                receipt["control"]["terminal_budget_reached"] = terminal_budget(
                    args.output / "control", args.max_dispatches)
                # Continue even on a failed A/A control: collect comparable
                # evidence, but never declare a matched A/B result.
            if prior_library is not None:
                print("Prior experiment: replaying the recorded PAD0 polls.", flush=True)
                receipt["prior"] = run_one(args, prior_library, args.output / "prior",
                                           dict(env, SDBZ_PAD0_REPLAY=str(recording)))
                receipt["prior_pad_replay"] = replay_summary(args.output / "prior" / "stderr.log")
                receipt["prior_trace"] = trace_comparison(
                    args.output / "baseline" / "stdout.log", args.output / "prior" / "stdout.log")
                receipt["prior"]["terminal_budget_reached"] = terminal_budget(
                    args.output / "prior", args.max_dispatches)
            print("Candidate: replaying the recorded PAD0 polls.", flush=True)
            receipt["dedup"] = run_one(args, candidate, args.output / "dedup",
                                       dict(env, SDBZ_PAD0_REPLAY=str(recording)))
            receipt["pad_replay"] = replay_summary(args.output / "dedup" / "stderr.log")
            receipt["dedup_trace"] = trace_comparison(
                args.output / "baseline" / "stdout.log", args.output / "dedup" / "stdout.log")
            receipt["dedup"]["terminal_budget_reached"] = terminal_budget(
                args.output / "dedup", args.max_dispatches)
            if args.control:
                control = receipt["control_pad_replay"]
                if (not control or not control["valid"] or
                        control["consumed"] != receipt["recorded_polls"] or
                        control["total"] != receipt["recorded_polls"] or
                        receipt["control"]["returncode"] != 4 or
                        not receipt["control"]["terminal_budget_reached"] or
                        not receipt["control_trace"]["common_samples"] or
                        receipt["control_trace"]["different_common_samples"] or
                        receipt["control_trace"]["unpaired_samples"]):
                    raise ValueError("A/A control not matched; dedup result remains diagnostic")
            if prior_library is not None:
                parent = receipt["prior_pad_replay"]
                trace = receipt["prior_trace"]
                if (not parent or not parent["valid"] or
                        parent["consumed"] != receipt["recorded_polls"] or
                        parent["total"] != receipt["recorded_polls"] or
                        receipt["prior"]["returncode"] != 4 or
                        not receipt["prior"]["terminal_budget_reached"] or
                        not trace["common_samples"] or trace["different_common_samples"] or
                        trace["unpaired_samples"]):
                    raise ValueError("prior experiment replay not matched")
            replay = receipt["pad_replay"]
            if (not replay or not replay["valid"] or replay["consumed"] != receipt["recorded_polls"]
                    or replay["total"] != receipt["recorded_polls"]):
                raise ValueError("PAD0 replay diverged or ended early; not a matched comparison")
            if receipt["dedup"]["returncode"] != 4 or not receipt["dedup"]["terminal_budget_reached"]:
                raise ValueError("deduplicated replay failed to produce a terminal budget report")
            if (not receipt["dedup_trace"]["common_samples"] or
                    receipt["dedup_trace"]["different_common_samples"] or
                    receipt["dedup_trace"]["unpaired_samples"]):
                raise ValueError("sampled dispatch transitions differ; not a matched comparison")
        except (OSError, ValueError) as error:
            receipt["error"] = str(error)
        finally:
            receipt["libraries_unchanged"] = (
                sha(baseline) == hashes["baseline"] and sha(candidate) == hashes["dedup"] and
                (prior_library is None or sha(prior_library) == hashes["prior"]))
            if args.source_comparison is not None:
                receipt["source_comparison_unchanged"] = (
                    sha(args.source_comparison) == inputs["source_comparison_sha256"])
            if args.prior_source_comparison is not None:
                receipt["prior_source_comparison_unchanged"] = (
                    sha(args.prior_source_comparison) == inputs["prior_source_comparison_sha256"])
            if card:
                receipt["memcard_unchanged"] = sha(Path(card)) == inputs["memcard_sha256"]
            (args.output / "comparison.json").write_text(
                json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
            archive = shutil.make_archive(str(args.output), "zip", args.output)
            print("Comparison archive:", archive, flush=True)
        if (receipt["error"] or not receipt["libraries_unchanged"] or
                not receipt.get("memcard_unchanged", True) or
                not receipt.get("source_comparison_unchanged", True) or
                not receipt.get("prior_source_comparison_unchanged", True)):
            print("Replay requires investigation:", receipt["error"], file=sys.stderr)
            return 1
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(f"EE dedup replay refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
