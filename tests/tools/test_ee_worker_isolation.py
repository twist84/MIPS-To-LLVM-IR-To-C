#!/usr/bin/env python3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from replay_ee_worker_isolation import combat_window, gs_worker_stats, compare_arms


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
    print("PASS: worker isolation combat window and workload evidence")


if __name__ == "__main__":
    main()
