#!/usr/bin/env python3
"""Compare declared physical-console PCR0 body-size slopes to offline candidates."""
import argparse
from fractions import Fraction
import hashlib
from pathlib import Path
import re
import statistics
import subprocess
import tempfile

from generate_ee_issue_bench import CASES, REPEATS, SAMPLES, generate

BEGIN = "EE_ISSUE_COUNTER_V1"
END = "EE_ISSUE_COUNTER_END"
HEADER = "case,repeat,sample,pcr0_delta"
PAIR_BEGIN = "EE_ISSUE_PAIR_COUNTER_V1"
PAIR_END = "EE_ISSUE_PAIR_COUNTER_END"
PAIR_HEADER = "case,repeat,sample,single_issue_delta,dual_issue_delta"


def parse_counter_log(text: str) -> dict:
    lines = [line.strip() for line in text.splitlines()]
    if lines.count(BEGIN) != 1 or lines.count(END) != 1:
        raise ValueError("expected exactly one complete counter capture")
    begin, end = lines.index(BEGIN), lines.index(END)
    if end <= begin or lines[begin + 1] != HEADER:
        raise ValueError("invalid counter capture header/order")
    samples = {}
    for line in lines[begin + 2:end]:
        match = re.fullmatch(r"([a-z0-9_]+),([0-9]+),([0-9]+),([0-9]+)", line)
        if not match:
            raise ValueError(f"invalid counter sample: {line!r}")
        name, repeat, sample, delta = match.groups()
        repeat, sample, delta = int(repeat), int(sample), int(delta)
        if (name not in CASES or repeat not in REPEATS or
                sample >= SAMPLES or not 0 < delta <= 0xffffffff):
            raise ValueError(f"out-of-range or zero counter sample: {line!r}")
        key = (name, repeat)
        group = samples.setdefault(key, {})
        if sample in group:
            raise ValueError(f"duplicate counter sample: {line!r}")
        group[sample] = delta
    expected = {(name, repeat) for name in CASES for repeat in REPEATS}
    if set(samples) != expected or any(set(group) != set(range(SAMPLES))
                                        for group in samples.values()):
        raise ValueError("missing benchmark samples")
    return samples


def parse_issue_pair_log(text: str) -> dict:
    lines = [line.strip() for line in text.splitlines()]
    if lines.count(PAIR_BEGIN) != 1 or lines.count(PAIR_END) != 1:
        raise ValueError("expected exactly one complete issue-pair capture")
    begin, end = lines.index(PAIR_BEGIN), lines.index(PAIR_END)
    if end <= begin + 1 or lines[begin + 1] != PAIR_HEADER:
        raise ValueError("invalid issue-pair capture header/order")
    samples = {}
    for line in lines[begin + 2:end]:
        match = re.fullmatch(r"([a-z0-9_]+),([0-9]+),([0-9]+),([0-9]+),([0-9]+)", line)
        if not match:
            raise ValueError(f"invalid issue-pair sample: {line!r}")
        name, repeat, sample, single, dual = match.groups()
        repeat, sample, single, dual = map(int, (repeat, sample, single, dual))
        # Zero is a real measurement. Refuse high-bit deltas; the
        # bounded fixture is not an overflow/wrap experiment.
        if (name not in CASES or repeat not in REPEATS or sample >= SAMPLES or
                not 0 <= single < 0x80000000 or not 0 <= dual < 0x80000000):
            raise ValueError(f"out-of-range issue-pair sample: {line!r}")
        group = samples.setdefault((name, repeat), {})
        if sample in group:
            raise ValueError(f"duplicate issue-pair sample: {line!r}")
        group[sample] = (single, dual)
    expected = {(name, repeat) for name in CASES for repeat in REPEATS}
    if set(samples) != expected or any(set(group) != set(range(SAMPLES))
                                        for group in samples.values()):
        raise ValueError("missing issue-pair samples")
    return samples


def candidate_totals(binary: Path, *, ordered_overlap=False, cop1_a_stage=False) -> dict:
    result = {}
    with tempfile.TemporaryDirectory(prefix="ee-counter-candidate-") as directory:
        out = Path(directory)
        for row in generate(out):
            command = [str(binary.resolve()), str(out / row["trace"]),
                       "--manual-cache-hit"]
            if ordered_overlap:
                command.append("--ordered-waw-overlap")
            if cop1_a_stage:
                command.append("--cop1-a-stage-pairing")
            run = subprocess.run(command, capture_output=True, text=True, timeout=30)
            if run.returncode:
                raise ValueError(f"{row['symbol']}: candidate rejected: {run.stderr.strip()}")
            values = {}
            for field in ("candidate_issue_cycles", "reached", "max_candidate_completion",
                          "candidate_single_issue_cycles", "candidate_dual_issue_cycles"):
                match = re.search(rf"\b{field}=([0-9]+)\b", run.stdout)
                if not match:
                    raise ValueError(f"{row['symbol']}: missing {field}")
                values[field] = int(match.group(1))
            if values["reached"] != row["instructions"]:
                raise ValueError(f"{row['symbol']}: candidate body count mismatch")
            result[(row["case"], row["repeat"])] = values
    return result


def slope(first, second, count):
    return Fraction(second - first, count)


def compare(samples, candidates):
    # PCR0 includes fixed counter/fence and warm-cache effects. Difference
    # the same body's lengths; never infer per-instruction issue timestamps.
    for name in CASES:
        groups = [samples[(name, n)].values() for n in REPEATS]
        lows = [min(group) for group in groups]
        mids = [int(statistics.median(group)) for group in groups]
        highs = [max(group) for group in groups]
        issue = [candidates[(name, n)]["candidate_issue_cycles"] for n in REPEATS]
        completion = [candidates[(name, n)]["max_candidate_completion"] for n in REPEATS]
        print(f"{name}: raw min/median/max " +
              " ".join(f"{n}={lo}/{mid}/{hi}" for n, lo, mid, hi
                       in zip(REPEATS, lows, mids, highs)))
        for i in range(1, len(REPEATS)):
            delta = REPEATS[i] - REPEATS[i - 1]
            observed_min = slope(lows[i - 1], lows[i], delta)
            observed_median = slope(mids[i - 1], mids[i], delta)
            candidate_issue = slope(issue[i - 1], issue[i], delta)
            candidate_completion = slope(completion[i - 1], completion[i], delta)
            print(f"  {REPEATS[i-1]}->{REPEATS[i]} cycles/pair: "
                  f"observed-min={observed_min} observed-median={observed_median} "
                  f"candidate-issue={candidate_issue} candidate-completion={candidate_completion} "
                  f"median-minus-issue={observed_median - candidate_issue}")


def compare_issue_pairs(samples, candidates):
    # PCR0 and PCR1 reads are adjacent, not simultaneous. Compare size
    # slopes separately; do not infer a timestamp or retirement count.
    for name in CASES:
        for index, event in enumerate(("single", "dual")):
            groups = [[pair[index] for pair in samples[(name, n)].values()]
                      for n in REPEATS]
            lows = [min(group) for group in groups]
            mids = [int(statistics.median(group)) for group in groups]
            highs = [max(group) for group in groups]
            candidate = [candidates[(name, n)][f"candidate_{event}_issue_cycles"]
                         for n in REPEATS]
            print(f"{name} {event}: raw min/median/max " +
                  " ".join(f"{n}={lo}/{mid}/{hi}" for n, lo, mid, hi
                           in zip(REPEATS, lows, mids, highs)))
            for i in range(1, len(REPEATS)):
                count = REPEATS[i] - REPEATS[i - 1]
                observed_min = slope(lows[i - 1], lows[i], count)
                observed_median = slope(mids[i - 1], mids[i], count)
                modeled = slope(candidate[i - 1], candidate[i], count)
                print(f"  {REPEATS[i-1]}->{REPEATS[i]} {event}-issue cycles/pair: "
                      f"observed-min={observed_min} observed-median={observed_median} "
                      f"candidate={modeled} median-minus-candidate={observed_median - modeled}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--console", required=True)
    parser.add_argument("--bios", required=True)
    parser.add_argument("--toolchain", required=True)
    parser.add_argument("--loader", required=True)
    parser.add_argument("--ordered-waw-overlap", action="store_true")
    parser.add_argument("--cop1-a-stage-pairing", action="store_true")
    args = parser.parse_args()
    if any(not value.strip() for value in
           (args.console, args.bios, args.toolchain, args.loader)):
        parser.error("all declared capture provenance fields must be nonempty")
    try:
        raw = args.log.read_bytes()
        capture = raw.decode("utf-8")
        samples = parse_counter_log(capture)
        lines = [line.strip() for line in capture.splitlines()]
        pairs = (parse_issue_pair_log(capture)
                 if PAIR_BEGIN in lines or PAIR_END in lines else None)
        candidates = candidate_totals(args.candidate,
                                      ordered_overlap=args.ordered_waw_overlap,
                                      cop1_a_stage=args.cop1_a_stage_pairing)
        print("Declared capture provenance (not independently authenticated): "
              f"console={args.console!r} bios={args.bios!r} "
              f"toolchain={args.toolchain!r} loader={args.loader!r}")
        print(f"capture_sha256={hashlib.sha256(raw).hexdigest()}")
        print(f"candidate_profile=manual-cache-hit "
              f"ordered_waw_overlap={args.ordered_waw_overlap} "
              f"cop1_a_stage_pairing={args.cop1_a_stage_pairing}")
        compare(samples, candidates)
        if pairs is not None:
            compare_issue_pairs(pairs, candidates)
        else:
            print("No issue-pair capture; single/dual hardware comparison unavailable.")
        print("Observational comparison only; no validated clock or retirement trace.")
    except (ValueError, OSError, UnicodeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"counter comparison refused: {error}\n")


if __name__ == "__main__":
    main()
