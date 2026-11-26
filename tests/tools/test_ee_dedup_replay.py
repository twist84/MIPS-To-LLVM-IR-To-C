#!/usr/bin/env python3
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from replay_ee_dedup_experiment import record_count, replay_summary, sha, validate_libraries, trace_comparison, terminal_budget


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
    print("PASS: dedup replay validates identities and input recording")


if __name__ == "__main__":
    run()
