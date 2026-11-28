#!/usr/bin/env python3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from replay_ee_worker_isolation import combat_window, gs_worker_stats, compare_arms, program_profile_delta
import json


def main():
    log = ("EE progress: dispatched=8192 pc=0x0 elapsed_ms=100.0 native_ms=0 fields=1\n"
           "EE progress: dispatched=16384 pc=0x0 elapsed_ms=300.0 native_ms=0 fields=5\n")
    window = combat_window(log, 8192, 20000)
    assert window["fields_per_second"] == 20.0
    for start, end in ((0, 20000), (8192, 30000), (16384, 20000)):
        try:
            combat_window(log, start, end)
        except ValueError:
            pass
        else:
            raise AssertionError("incomplete combat window accepted")
    assert gs_worker_stats("unrelated\n") is None
    assert gs_worker_stats("GS async worker: submitted=8 completed=8 healthy=1\n") == {
        "submitted": 8, "completed": 8, "healthy": 1}
    evidence = {"combat": window, "graphics_workload_sha256": "a", "image_sha256": "b",
                "section_sha256": {"one": "a"}}
    arms = {"both_before": {"evidence": evidence},
            "sync": {"evidence": dict(evidence, combat=dict(window, end_field=6),
                                      section_sha256={"one": "c"})}}
    result = compare_arms(arms)
    assert result["both_before"]["combat_window_matches"]
    assert result["sync"]["combat_gain_vs_first_both_percent"] is None
    assert result["sync"]["differing_workload_sections"] == ["one"]
    row = {"pc": 0x730, "library_index": 1, "code_fnv1a64": "1234",
           "native_runs": 2, "native_pairs": 8, "profiled_attempts": 2,
           "profiled_pairs": 8, "compiled_exclusive_ns": 40}
    row_after = dict(row, native_runs=4, native_pairs=16, profiled_attempts=4,
                     profiled_pairs=16, compiled_exclusive_ns=104)
    snapshot = lambda dispatch, fields, item: (
        f"[vu1] worker profile snapshot dispatch={dispatch} fields={fields} aot=" +
        json.dumps({"program_profiles": [item]}) + "\n")
    prefix = snapshot(8192, 1, row)
    profile = program_profile_delta(prefix + snapshot(16384, 5, row_after), window)
    assert profile["compiled_exclusive_ns"] == 64
    assert profile["programs"][0]["ns_per_pair"] == 8
    for invalid in (snapshot(16384, 6, row_after),
                    snapshot(16384, 5, dict(row_after, profiled_pairs=15)),
                    snapshot(16384, 5, dict(row_after, code_fnv1a64="different"))):
        try:
            program_profile_delta(prefix + invalid, window)
        except ValueError:
            pass
        else:
            raise AssertionError("incorrect VU1 snapshot accepted")
    print("PASS: worker isolation combat window and workload evidence")


if __name__ == "__main__":
    main()
