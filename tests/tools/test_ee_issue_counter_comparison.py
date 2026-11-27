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
from compare_ee_issue_bench import (BEGIN, END, HEADER, PAIR_BEGIN, PAIR_END,
                                    PAIR_HEADER, parse_counter_log, parse_issue_pair_log,
                                    compare, compare_issue_pairs, slope)


def main():
    lines = [BEGIN, HEADER]
    candidates = {}
    for name in CASES:
        for repeat in REPEATS:
            candidates[(name, repeat)] = {
                "candidate_issue_cycles": 2 * repeat,
                "max_candidate_completion": 2 * repeat + 2,
                "candidate_single_issue_cycles": 2 * repeat,
                "candidate_dual_issue_cycles": 0,
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
    pair_lines = [PAIR_BEGIN, PAIR_HEADER]
    for name in CASES:
        for repeat in REPEATS:
            for sample in range(SAMPLES):
                pair_lines.append(f"{name},{repeat},{sample},{2*repeat+sample},0")
    pair_lines.append(PAIR_END)
    pairs = parse_issue_pair_log("\n".join(lines + pair_lines))
    assert len(pairs) == len(CASES) * len(REPEATS)
    assert pairs[("nop_pair", REPEATS[0])][0] == (2 * REPEATS[0], 0)
    output = io.StringIO()
    with redirect_stdout(output):
        compare_issue_pairs(pairs, candidates)
    assert output.getvalue().count(
        "observed-min=2 observed-median=2 candidate=2 median-minus-candidate=0"
    ) == len(CASES) * 2
    assert output.getvalue().count(
        "observed-min=0 observed-median=0 candidate=0 median-minus-candidate=0"
    ) == len(CASES) * 2
    for invalid in (pair_lines[:-1], pair_lines[:2] + pair_lines[3:],
                    pair_lines[:3] + [pair_lines[2]] + pair_lines[3:],
                    pair_lines[:2] + [pair_lines[2].rsplit(",", 1)[0] + ",2147483648"] + pair_lines[3:],
                    pair_lines + [PAIR_END]):
        try:
            parse_issue_pair_log("\n".join(invalid))
        except ValueError:
            pass
        else:
            raise AssertionError("invalid issue-pair capture accepted")
    print("PASS: counter importer completeness and exact slopes (synthetic)")


if __name__ == "__main__":
    main()
