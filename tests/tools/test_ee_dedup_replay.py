#!/usr/bin/env python3
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from replay_ee_dedup_experiment import record_count, replay_summary, sha, validate_libraries, validate_prior, validate_source_libraries, validate_source_reference, trace_comparison, terminal_budget


def run():
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
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
