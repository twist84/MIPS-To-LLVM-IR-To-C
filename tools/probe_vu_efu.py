#!/usr/bin/env python3
"""Compare shared EFU arithmetic directly against pinned original console words.

This is a diagnostic, not an original ELF transcript pass or a hardware
timing test. No output normalization or known-difference exceptions.
"""
import argparse
import hashlib
import pathlib
import re
import subprocess
import urllib.request

REVISION = "97469ffbed8631277b94e28d01dabd702aa97ef3"
FILES = {
    "efu.cpp": "76e0d9c56a8e08c95f8d837fbe8c98386329e820",
    "efu.expected": "51bc7235260d18aa6963991f8ff9f154ac4a3a2b",
}
# Opcode index and existing runtime latency, not independent timing evidence.
OPS = {
    "EATAN": (0x7D, 54), "EATANxy": (0x74, 54), "EATANxz": (0x75, 54),
    "EEXP": (0x7E, 44), "ELENG": (0x72, 18), "ERCPR": (0x7A, 12),
    "ERLENG": (0x73, 24), "ERSADD": (0x71, 18), "ERSQRT": (0x79, 18),
    "ESADD": (0x70, 11), "ESIN": (0x7C, 29), "ESQRT": (0x78, 12),
    "ESUM": (0x76, 12),
}


def reference_file(name, root):
    relative = "tests/vu/lower/" + name
    if root is None:
        url = f"https://raw.githubusercontent.com/unknownbrackets/ps2autotests/{REVISION}/{relative}"
        with urllib.request.urlopen(url, timeout=30) as response:
            data = response.read()
    else:
        data = (root / relative).read_bytes()
    blob = hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()
    if blob != FILES[name]:
        raise ValueError(f"Unexpected reference blob for {name}: {blob}")
    return data.decode("utf-8")


def parse_cases(source, expected):
    vectors = {}
    for name, values in re.findall(r"\bC_([A-Z0-9_]+)\[4\]\s*=\s*\{([^}]+)\}", source):
        words = tuple(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{8}", values))
        if len(words) != 4 or name in vectors:
            raise ValueError(f"Invalid vector {name}")
        vectors[name] = words
    cases = []
    seen = set()
    for op, name, word in re.findall(
            r"^\s+(E\w+)\s+CVF_([A-Z0-9_]+):\s+([0-9a-fA-F]{8})/", expected, re.M):
        key = (op, name)
        if key in seen or op not in OPS or name not in vectors:
            raise ValueError(f"Invalid reference case {key}")
        seen.add(key)
        cases.append((op, name, vectors[name], int(word, 16)))
    if len(vectors) != 16 or seen != {(op, name) for op in OPS for name in vectors}:
        raise ValueError("Reference does not contain all 208 unique EFU cases")
    return cases


def compare(probe, cases):
    requests = "".join(
        f"{OPS[op][0]:x} 2 " + " ".join(f"{word:08x}" for word in words) + "\n"
        for op, _, words, _ in cases
    )
    run = subprocess.run([str(probe.resolve())], input=requests, text=True,
                         stdout=subprocess.PIPE, check=True, timeout=60)
    rows = run.stdout.splitlines()
    if len(rows) != len(cases):
        raise ValueError(f"Probe returned {len(rows)} rows for {len(cases)} requests")
    counts = {op: [0, 0] for op in OPS}
    latency_mismatches = 0
    for (op, name, words, expected), row in zip(cases, rows):
        if not re.fullmatch(r"[0-9a-fA-F]{8} [0-9]+", row):
            raise ValueError(f"Invalid probe output: {row!r}")
        word, latency = row.split()
        actual = int(word, 16)
        counts[op][1] += 1
        if actual == expected:
            counts[op][0] += 1
        else:
            raw = " ".join(f"{x:08x}" for x in words)
            print(f"DIFF {op} {name}: VF={raw} expected={expected:08x} actual={actual:08x}")
        if int(latency) != OPS[op][1]:
            latency_mismatches += 1
            print(f"LATENCY {op} {name}: expected model={OPS[op][1]} actual={latency}")
    for op, (matched, total) in counts.items():
        print(f"{op}: {matched}/{total} exact words")
    matched = sum(pair[0] for pair in counts.values())
    print(f"Direct EFU arithmetic: {matched}/{len(cases)} exact words; "
          f"{latency_mismatches} existing-model latency mismatches")
    return matched == len(cases) and latency_mismatches == 0, latency_mismatches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    parser.add_argument("--reference-root", type=pathlib.Path,
                        help="Pinned local upstream checkout; otherwise fetch two verified text files")
    parser.add_argument("--report-only", action="store_true",
                        help="Do not fail on arithmetic differences (malformed inputs still fail)")
    args = parser.parse_args()
    try:
        cases = parse_cases(reference_file("efu.cpp", args.reference_root),
                            reference_file("efu.expected", args.reference_root))
        matched, latency_mismatches = compare(args.probe, cases)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(2, f"EFU diagnostic failed: {exc}\n")
    return 0 if matched or (args.report_only and not latency_mismatches) else 1


if __name__ == "__main__":
    raise SystemExit(main())
