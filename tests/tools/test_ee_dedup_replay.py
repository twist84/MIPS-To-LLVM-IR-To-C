#!/usr/bin/env python3
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from replay_ee_dedup_experiment import record_count, replay_summary, sha, validate_libraries, validate_prior, validate_source_libraries, validate_source_reference, trace_comparison, terminal_budget


from replay_ee_graphics_sweep import validate_aot_pair, aot_identity
from build_vu1_aot import matching_captures


def test_vu1_inventory(root):
    import os
    suffix = ".dll" if os.name == "nt" else ".so"
    before, after, captures = (root / name for name in ("vu-before", "vu-after", "vu-captures"))
    for directory in (before, after, captures):
        directory.mkdir()
    for directory in (before, after):
        (directory / "vu1-build-target.txt").write_text("generic\n")
    (before / ("0080-a" + suffix)).write_bytes(b"old")
    (after / ("0080-a" + suffix)).write_bytes(b"new")
    (captures / "0080-a.bin").write_bytes(b"capture")
    (captures / "0090-extra.bin").write_bytes(b"extra")
    assert matching_captures(captures, before, suffix, "generic") == [captures / "0080-a.bin"]
    validate_aot_pair(aot_identity(before), aot_identity(after))
    for new, expected in (
        ({"target": "native", "libraries": {}}, "target mismatch"),
        ({"target": "generic", "libraries": {"extra": "hash"}}, "inventory mismatch"),
        (aot_identity(before), "byte-identical"),
    ):
        try:
            validate_aot_pair(aot_identity(before), new)
        except ValueError as error:
            assert expected in str(error)
        else:
            raise AssertionError("invalid VU1 inventory accepted")
    (captures / "0080-a.bin").unlink()
    try:
        matching_captures(captures, before, suffix, "generic")
    except ValueError as error:
        assert "missing baseline capture" in str(error)
    else:
        raise AssertionError("missing VU1 capture accepted")



def test_graphics_profile(root):
    from replay_ee_graphics_profile import extract_profile, require_match
    from copy import deepcopy

    row = {"calls": 2, "inclusive_ns": 8, "exclusive_ns": 6, "work": 3}
    program = {"pc": 80, "library_index": 0, "code_fnv1a64": "abc",
               "attempts": 2, "attempted_pairs": 3, "native_runs": 2,
               "native_pairs": 3, "profiled_attempts": 2, "profiled_pairs": 3,
               "compiled_exclusive_ns": 6}
    from replay_ee_graphics_profile import WORKER_TIMING_FIELDS
    report = {"dispatches": 40, "ee_cycles": 100, "gs_fields_started": 2,
              "gpr_lo": [0, 1], "vu1_aot": {"program_profiles": [program]},
              "vu1_worker": dict.fromkeys(WORKER_TIMING_FIELDS, 0),
              "runtime_profile": {
                  "schema": "ee-runtime-profile-v1", "enabled": True,
                  "scope": "runner_thread_completed_scopes", "clock": "steady_clock",
                  "dispatches": 40, "guest_fields": 2, "presentations": 1,
                  "wall_ns": 10, "covered_ns": 6, "uninstrumented_ns": 4,
                  "subsystems": {"vu1_compiled": row}}}
    interval = {"schema": "ee-runtime-profile-interval-v1",
                "start_dispatch": 0, "end_dispatch": 40,
                "start_cycle": 0, "end_cycle": 100,
                "start_fields": 0, "end_fields": 2,
                "start_presentations": 0, "end_presentations": 1,
                "wall_ns": 10, "covered_ns": 6, "uninstrumented_ns": 4,
                "subsystems": {"vu1_compiled": row}}
    log = root / "profile.log"
    log.write_text("EE profile interval: " + json.dumps(interval) + "\n")
    extracted = extract_profile(report, log, True)
    assert extracted["vu1_programs"] == [program]
    assert "vu0_sync" in extracted["unattributed"]
    # Old receipts remain readable. New scopes must arrive as a complete
    # inventory and must also be present in every interval.
    with_vu0 = deepcopy(report)
    with_vu0_interval = deepcopy(interval)
    vu0_scopes = ("vu0_sync_halted", "vu0_sync_cmsar1",
                  "vu0_sync_micro", "vu0_sync_invalid")
    zero = dict(calls=0, inclusive_ns=0, exclusive_ns=0, work=0)
    for name in vu0_scopes:
        with_vu0["runtime_profile"]["subsystems"][name] = dict(zero)
        with_vu0_interval["subsystems"][name] = dict(zero)
    log.write_text("EE profile interval: " + json.dumps(with_vu0_interval) + "\n")
    extracted = extract_profile(with_vu0, log, True)
    assert "vu0_sync" not in extracted["unattributed"]
    assert "vu0_sync_noncallback" in extracted["unattributed"]
    del with_vu0["runtime_profile"]["subsystems"]["vu0_sync_invalid"]
    del with_vu0_interval["subsystems"]["vu0_sync_invalid"]
    log.write_text("EE profile interval: " + json.dumps(with_vu0_interval) + "\n")
    try:
        extract_profile(with_vu0, log, True)
    except ValueError as error:
        assert "incomplete VU0 callback scope inventory" in str(error)
    else:
        raise AssertionError("partial VU0 inventory accepted")
    log.write_text("EE profile interval: " + json.dumps(interval) + "\n")
    control = deepcopy(report)
    control["runtime_profile"] = {"schema": "ee-runtime-profile-v1", "enabled": False}
    control["vu1_aot"]["program_profiles"][0]["compiled_exclusive_ns"] = 0
    require_match(report, control)
    for mutate in (
        lambda value: value["gpr_lo"].__setitem__(1, 2),
        lambda value: value["vu1_aot"]["program_profiles"][0].__setitem__("native_pairs", 2),
        lambda value: value["vu1_worker"].__setitem__("unexpected_ns", 1),
    ):
        bad = deepcopy(control)
        mutate(bad)
        try:
            require_match(report, bad)
        except ValueError:
            pass
        else:
            raise AssertionError("architectural difference ignored")
    bad = deepcopy(interval)
    bad["end_dispatch"] = 39
    log.write_text("EE profile interval: " + json.dumps(bad) + "\n")
    try:
        extract_profile(report, log, True)
    except ValueError:
        pass
    else:
        raise AssertionError("incomplete interval accepted")
    bad = deepcopy(report)
    bad["runtime_profile"]["covered_ns"] = 7
    try:
        extract_profile(bad, log, True)
    except ValueError:
        pass
    else:
        raise AssertionError("invalid accounting accepted")
    log.write_text("")
    assert extract_profile(control, log, False) is None


def test_graphics_ee_comparison(root):
    from unittest.mock import patch
    import replay_ee_graphics_profile as harness

    files = ("runner", "elf", "baseline.dll", "control.dll", "candidate.dll",
             "control.json", "candidate.json", "record.log")
    for name in files:
        (root / name).write_text(name)
    for name in ("profile", "control-source", "candidate-source", "vu1",
                 "disc", "reference"):
        (root / name).mkdir()
    for name in ("report.json", "stdout.log", "gs-webgpu.ppm"):
        (root / "reference" / name).write_text("{}")
    report = {"vu1_worker": {"enabled": False}}
    inventory = {"target": "native", "libraries": {"x.dll": "hash"}}
    calls = []

    def validate(profile, source, elf, receipt):
        assert profile == root / "profile" and elf == root / "elf"
        name = "control" if source.name == "control-source" else "candidate"
        assert receipt == root / (name + ".json")
        image = root / (name + ".dll")
        return root / "baseline.dll", image, {"dedup": sha(image)}

    def replay(args, image, directory, env):
        calls.append((directory.name, image.name, env["SDBZ_VU1_AOT"],
                      env["SDBZ_RUNTIME_PROFILE"]))
        directory.mkdir()
        return {"wall_seconds": 2.0 if image.name == "control.dll" else 1.0}

    args = ["graphics", "--repeats", "2"]
    for key, value in (("runner", "runner"), ("elf", "elf"), ("disc", "disc"),
                       ("profile", "profile"), ("source", "control-source"),
                       ("source-receipt", "control.json"), ("vu1", "vu1"),
                       ("recording", "record.log"), ("reference", "reference"),
                       ("output", "ee-replay"), ("candidate-source", "candidate-source"),
                       ("candidate-source-receipt", "candidate.json")):
        args += ["--" + key, str(root / value)]
    with patch.object(sys, "argv", args), patch.multiple(
            harness, validate_source_libraries=validate,
            aot_identity=lambda path: inventory,
            terminal_budget=lambda path, budget: True,
            read_json=lambda path: report, record_count=lambda path: 1,
            replay_environment=lambda path: {}, run_one=replay,
            validate_run=lambda *args: {}, require_match=lambda *args: None,
            extract_profile=lambda *args: None):
        assert harness.main() == 0
    assert calls == [
        (directory, image, str(root / "vu1"), "0")
        for directory, image in (("control-1", "control.dll"),
                                 ("candidate-1", "candidate.dll"),
                                 ("candidate-2", "candidate.dll"),
                                 ("control-2", "control.dll"))]
    receipt = json.loads((root / "ee-replay" / "profiling-receipt.json").read_text())
    assert receipt["schema"] == "ee-graphics-ee-comparison-v1"
    assert receipt["inputs"]["candidate_library"] == sha(root / "candidate.dll")
    assert receipt["inputs"]["candidate_source_receipt"] == sha(root / "candidate.json")
    assert receipt["correctness_accepted"] and receipt["inputs_unchanged"]
    assert not receipt["performance_accepted"] and receipt["relative_throughput"] == 2.0

    (root / "new-runner").write_text("new executable")
    calls.clear()

    def replay_runner(args, image, directory, env):
        assert image == root / "control.dll"
        calls.append((directory.name, args.runner.name,
                      env["SDBZ_VU1_AOT"], env["SDBZ_RUNTIME_PROFILE"]))
        directory.mkdir()
        return {"wall_seconds": 2.0 if args.runner.name == "runner" else 1.0}

    runner_args = args[:args.index("--candidate-source")]
    runner_args[runner_args.index("--output") + 1] = str(root / "runner-replay")
    runner_args += ["--candidate-runner", str(root / "new-runner"),
                    "--control-runner-sha256", sha(root / "runner")]
    with patch.multiple(
            harness, validate_source_libraries=validate,
            aot_identity=lambda path: inventory,
            terminal_budget=lambda path, budget: True,
            read_json=lambda path: report, record_count=lambda path: 1,
            replay_environment=lambda path: {}, run_one=replay_runner,
            validate_run=lambda *args: {}, require_match=lambda *args: None,
            extract_profile=lambda *args: None):
        with patch.object(sys, "argv", runner_args[:-1] + ["wrong"]):
            assert harness.main() == 1 and not calls
        with patch.object(sys, "argv", runner_args + [
                "--candidate-source", str(root / "candidate-source"),
                "--candidate-source-receipt", str(root / "candidate.json")]):
            assert harness.main() == 1 and not calls
        with patch.object(sys, "argv", runner_args):
            assert harness.main() == 0
    assert calls == [
        (directory, runner, str(root / "vu1"), "0")
        for directory, runner in (("control-1", "runner"),
                                  ("candidate-1", "new-runner"),
                                  ("candidate-2", "new-runner"),
                                  ("control-2", "runner"))]
    receipt = json.loads((root / "runner-replay" / "profiling-receipt.json").read_text())
    assert receipt["schema"] == "ee-graphics-runner-comparison-v1"
    assert receipt["inputs"]["runner"] == sha(root / "runner")
    assert receipt["inputs"]["candidate_runner"] == sha(root / "new-runner")
    assert receipt["correctness_accepted"] and receipt["inputs_unchanged"]
    assert not receipt["performance_accepted"] and receipt["relative_throughput"] == 2.0

    import os
    library_dir = root / "iop-libraries"
    library_dir.mkdir()
    iop_library = library_dir / ("test.dll" if os.name == "nt" else "test.so")
    iop_library.write_bytes(b"native")
    capture_dir = root / "capture"
    calls.clear()

    def replay_iop(args, image, directory, env):
        assert args.runner == root / "runner" and image == root / "control.dll"
        assert env["SDBZ_RUNTIME_PROFILE"] == "0"
        assert "SDBZ_IOP_AOT_VERIFY" not in env
        calls.append(directory.name)
        directory.mkdir()
        stderr = directory / "stderr.log"
        stderr.write_text("")
        if directory.name == "capture-1":
            assert env["SDBZ_IOP_AOT_CAPTURE"] == str(capture_dir)
            assert "SDBZ_IOP_AOT" not in env
            (capture_dir / "test.bin").write_bytes(b"a")
            (capture_dir / "test.json").write_text(json.dumps(
                {"text_size": 1, "hash": "af63dc4c8601ec8c"}))
        elif directory.name.startswith("candidate"):
            assert env["SDBZ_IOP_AOT"] == str(library_dir)
            assert "SDBZ_IOP_AOT_CAPTURE" not in env
            stderr.write_text("IOP AOT: modules=1 active=1 entries=3 "
                              "instructions=42 translated_ms=0.1 refresh_ms=0.2\n")
        else:
            assert "SDBZ_IOP_AOT" not in env
            assert "SDBZ_IOP_AOT_CAPTURE" not in env
        return {"wall_seconds": 1.0}

    iop_args = args[:args.index("--candidate-source")]
    iop_args[iop_args.index("--repeats") + 1] = "1"
    iop_args += ["--control-runner-sha256", sha(root / "runner")]
    iop_args[iop_args.index("--output") + 1] = str(root / "capture-replay")
    with patch.multiple(
            harness, validate_source_libraries=validate,
            aot_identity=lambda path: inventory,
            terminal_budget=lambda path, budget: True,
            read_json=lambda path: (json.loads(path.read_text())
                                    if path.parent == capture_dir else report),
            record_count=lambda path: 1,
            replay_environment=lambda path: {"SDBZ_IOP_AOT": "stale",
                                             "SDBZ_IOP_AOT_VERIFY": "1"},
            run_one=replay_iop, validate_run=lambda *args: {},
            require_match=lambda *args: None, extract_profile=lambda *args: None):
        with patch.object(sys, "argv", iop_args + ["--capture-iop", str(capture_dir)]):
            assert harness.main() == 0
        assert calls == ["capture-1"]
        capture_receipt = json.loads((root / "capture-replay" /
                                      "profiling-receipt.json").read_text())
        assert capture_receipt["correctness_accepted"]
        assert capture_receipt["captures"]["test"]["binary_sha256"] == sha(capture_dir / "test.bin")
        iop_args[iop_args.index("--output") + 1] = str(root / "iop-replay")
        with patch.object(sys, "argv", iop_args + ["--candidate-iop-aot", str(library_dir)]):
            assert harness.main() == 0
    assert calls == ["capture-1", "control-1", "candidate-1"]
    receipt = json.loads((root / "iop-replay" / "profiling-receipt.json").read_text())
    assert receipt["schema"] == "ee-graphics-iop-aot-comparison-v1"
    assert receipt["candidate_iop_inventory"] == {iop_library.name: sha(iop_library)}
    assert receipt["correctness_accepted"] and receipt["inputs_unchanged"]
    assert receipt["runs"]["control"][0]["iop_aot"] is None
    assert receipt["runs"]["candidate"][0]["iop_aot"]["instructions"] == 42
    stderr = root / "iop-replay" / "candidate-1" / "stderr.log"
    for count in (0, 2):
        try:
            harness.iop_aot_observation(stderr, count)
        except ValueError:
            pass
        else:
            raise AssertionError("incorrect IOP AOT inventory accepted")


def run():
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        test_vu1_inventory(root)
        test_graphics_profile(root)
        ee_root = root / "ee-comparison"
        ee_root.mkdir()
        test_graphics_ee_comparison(ee_root)
        path = root / "record.log"
        sample = "PAD0 input: 1 1 65 5 5 65535 128 128 128 128\n"
        path.write_text("noise\n" + sample + "PAD0 input summary: record 1\n")
        assert record_count(path) == 1
        for bad in (sample, sample + "PAD0 input summary: record 2\n",
                    sample + sample + "PAD0 input summary: record 2\n"):
            path.write_text(bad)
            try:
                record_count(path)
            except ValueError:
                pass
            else:
                raise AssertionError("incomplete or invalid record accepted")
        path.write_text("PAD0 input summary: replay consumed=1 total=1 valid=1\n")
        assert replay_summary(path) == {"consumed": 1, "total": 1, "valid": True}
        left, right = root / "a.log", root / "b.log"
        line = "dispatch=1 pc=0x00100000 -> 0x00100004 reason=DISPATCH(3) status=1\n"
        left.write_text(line)
        right.write_text(line.replace("0x00100004", "0x00100008"))
        result = trace_comparison(left, right)
        assert result["common_samples"] == 1 and result["first_different_dispatch"] == 1
        right.write_text(line.replace("dispatch=1", "dispatch=2"))
        result = trace_comparison(left, right)
        assert result["common_samples"] == 0 and result["unpaired_samples"] == 2
        (root / "report.json").write_text(json.dumps({"snapshot_kind": "in_progress", "dispatches": 40}))
        assert not terminal_budget(root, 40)
        (root / "report.json").write_text(json.dumps({"snapshot_kind": "stopped", "dispatches": 40}))
        assert terminal_budget(root, 40) and not terminal_budget(root, 41)
        profile, experiment = root / "profile", root / "experiment"
        profile.mkdir()
        experiment.mkdir()
        import os
        library_name = ("game-translated.dll" if os.name == "nt" else
                        "libgame-translated.dylib" if sys.platform == "darwin" else
                        "libgame-translated.so")
        baseline, candidate = profile / library_name, experiment / library_name
        import hashlib
        elf = root / "guest.elf"
        elf.write_bytes(b"guest ELF")
        baseline.write_bytes(b"normal")
        candidate.write_bytes(b"dedup")
        (profile / "translation-cache.json").write_text(json.dumps({
            "metadata_sha256": "same", "library_sha256": sha(baseline),
            "inputs": {"ELF": sha(elf), "native_alu_certificate_mode": hashlib.sha256(b"0").hexdigest()}}))
        (experiment / "dedup-plan.json").write_text(json.dumps({
            "mode": "experimental_owner_coalescing", "runtime_validation_required": True,
            "metadata_sha256": "same", "linked_library_sha256": sha(candidate),
            "linked_library_bytes": candidate.stat().st_size}))
        assert validate_libraries(profile, experiment, elf)[:2] == (baseline, candidate)
        elf.write_bytes(b"wrong ELF")
        try:
            validate_libraries(profile, experiment, elf)
        except ValueError:
            pass
        else:
            raise AssertionError("wrong guest accepted")
        elf.write_bytes(b"guest ELF")
        candidate.write_bytes(b"stale")
        try:
            validate_libraries(profile, experiment, elf)
        except ValueError:
            pass
        else:
            raise AssertionError("changed library accepted")

        # Validate the exact previous experiment, not just a similarly named DLL.
        prior = root / "prior"
        (prior / "objects").mkdir(parents=True)
        (prior / "objects" / "objects.json").write_text("{}")
        parent_lib = prior / library_name
        parent_lib.write_bytes(b"observation")
        target = {"cpu": "fixture"}
        parent_plan = {"mode": "experimental_shared_observation",
                       "runtime_validation_required": True, "metadata_sha256": "same",
                       "native_target": target, "linked_library_sha256": sha(parent_lib),
                       "linked_library_bytes": parent_lib.stat().st_size}
        (prior / "helper-plan.json").write_text(json.dumps(parent_plan))
        current = {"mode": "experimental_shared_counter", "metadata_sha256": "same",
                   "native_target": target, "prior_library_sha256": sha(parent_lib),
                   "source_manifest_sha256": sha(prior / "objects" / "objects.json")}
        (experiment / "helper-plan.json").write_text(json.dumps(current))
        hashes = {"baseline": sha(baseline), "dedup": sha(candidate)}
        assert validate_prior(experiment, prior, candidate, baseline, hashes) == (parent_lib, sha(parent_lib))
        for key in ("prior_library_sha256", "source_manifest_sha256", "metadata_sha256"):
            (experiment / "helper-plan.json").write_text(json.dumps(dict(current, **{key: "wrong"})))
            try:
                validate_prior(experiment, prior, candidate, baseline, hashes)
            except ValueError:
                pass
            else:
                raise AssertionError("invalid prior provenance accepted: " + key)
        # Source comparison has a different IR generation, but the same
        # guest ownership and exact authenticated original/accepted images.
        source = root / "source"
        (source / "ir").mkdir(parents=True)
        (source / "objects").mkdir()
        (profile / "objects").mkdir()
        original = profile / "game-translated.dll"
        regenerated = source / original.name
        accepted = prior / original.name
        original.write_bytes(b"original O5")
        regenerated.write_bytes(b"new O5")
        accepted.write_bytes(b"accepted mixed")
        (source / "ir" / "ee_functions.json").write_text("{}")
        metadata = sha(source / "ir" / "ee_functions.json")
        native = {"native_target": target, "target_triple": "fixture",
                  "llvm_version": "fixture", "ir_pipeline": "fixture"}
        for folder in (profile, source):
            (folder / "objects" / "objects.json").write_text(json.dumps(
                dict(native, complete=True, optimization_level=5)))
        translation_path = profile / "translation-cache.json"
        translation_path.write_text(json.dumps({
            "inputs": {"ELF": sha(elf), "native_alu_certificate_mode":
                       hashlib.sha256(b"0").hexdigest()},
            "emit_opt": 5, "metadata_sha256": metadata,
            "library_sha256": sha(original), "ir_hashes": {"a.ll": "old"}}))
        receipt_path = root / "source.json"
        receipt = {
            "schema": "ee-source-lowering-comparison-v1",
            "stage": "linked_o5_candidate", "profile": 5,
            "runtime_validation_required": True, "retail_replay_performed": False,
            "elf_sha256": sha(elf), "baseline_translation_sha256": sha(translation_path),
            "baseline_metadata_sha256": metadata, "candidate_metadata_sha256": metadata,
            "baseline_object_manifest_sha256": sha(profile / "objects" / "objects.json"),
            "candidate_object_manifest_sha256": sha(source / "objects" / "objects.json"),
            "ownership_changes": {"missing": [], "extra": [], "changed": []},
            "guest_pc_changes": [], "functions": 1, "native_target": native,
            "modules": [{"module": "a.ll", "before_sha256": "old", "after_sha256": "new"}],
            "candidate_ir_hashes": {"a.ll": "new"},
            "baseline_library": {"sha256": sha(original), "bytes": original.stat().st_size},
            "candidate_library": {"sha256": sha(regenerated), "bytes": regenerated.stat().st_size},
        }
        receipt_path.write_text(json.dumps(receipt))
        b, c, h = validate_source_libraries(profile, source, elf, receipt_path)
        assert (b, c) == (original, regenerated)
        (prior / "helper-plan.json").write_text(json.dumps({
            "mode": "experimental_expanded_entry_rotation", "metadata_sha256": metadata,
            "native_target": target, "linked_library_sha256": sha(accepted),
            "linked_library_bytes": accepted.stat().st_size}))
        assert validate_source_reference(prior, profile, c, b, h, sha(accepted))[0] == accepted
        for key, value in (("guest_pc_changes", ["a.ll"]),
                           ("candidate_ir_hashes", {"a.ll": "wrong"}),
                           ("candidate_object_manifest_sha256", "wrong")):
            receipt_path.write_text(json.dumps(dict(receipt, **{key: value})))
            try:
                validate_source_libraries(profile, source, elf, receipt_path)
            except ValueError:
                pass
            else:
                raise AssertionError("invalid source receipt accepted: " + key)
        try:
            validate_source_reference(prior, profile, c, b, h, "wrong")
        except ValueError:
            pass
        else:
            raise AssertionError("unpinned reference accepted")
    print("PASS: dedup replay validates identities and input recording")


if __name__ == "__main__":
    run()
