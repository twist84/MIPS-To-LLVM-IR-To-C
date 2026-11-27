#!/usr/bin/env python3
"""Counter capture completeness and exact slope comparison (synthetic only)."""
from contextlib import redirect_stdout
from fractions import Fraction
import io
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from generate_ee_issue_bench import CASES, REPEATS, SAMPLES
from compare_ee_issue_bench import BEGIN, END, HEADER, parse_counter_log, compare, slope


def main():
    lines = [BEGIN, HEADER]
    candidates = {}
    for name in CASES:
        for repeat in REPEATS:
            candidates[(name, repeat)] = {
                "candidate_issue_cycles": 2 * repeat,
                "max_candidate_completion": 2 * repeat + 2,
            }
            for sample in range(SAMPLES):
                lines.append(f"{name},{repeat},{sample},{3*repeat+7+sample}")
    lines.append(END)
    groups = parse_counter_log("\n".join(["loader output", *lines, "done"]))
    assert len(groups) == len(CASES) * len(REPEATS)
    output = io.StringIO()
    with redirect_stdout(output):
        compare(groups, candidates)
    assert output.getvalue().count("observed-min=3 observed-median=3 "
                                  "candidate-issue=2 candidate-completion=2 "
                                  "median-minus-issue=1") == len(CASES)*2
    assert slope(5, 7, 3) == Fraction(2, 3)
    for invalid in (lines[:-1], lines[:2] + lines[3:],
                    lines[:3] + [lines[2]] + lines[3:],
                    lines[:2] + [lines[2].rsplit(",", 1)[0] + ",0"] + lines[3:],
                    lines + [END],
                    lines[:2] + ["unexpected text"] + lines[2:]):
        try:
            parse_counter_log("\n".join(invalid))
        except ValueError:
            pass
        else:
            raise AssertionError("invalid capture accepted")
    print("PASS: counter importer completeness and exact slopes (synthetic)")


if __name__ == "__main__":
    main()
