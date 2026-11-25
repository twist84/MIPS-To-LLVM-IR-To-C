#!/usr/bin/env python3
"""Combine the six independent original ELF reports without changing judgement."""
import argparse
from collections import Counter
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATUSES = ("PASS", "DIFF", "INCOMPLETE", "NO_OUTPUT", "ERROR")


def combine(reports, manifest):
    expected = [test["name"] for test in manifest["tests"]]
    results = {}
    provenance = None
    for report in reports:
        identity = (report["runtime_revision"], report["reference_revision"],
                    report["native_tool_sha256"])
        if identity[1] != manifest["revision"] or report["suite_total"] != len(expected):
            raise ValueError("unexpected reference revision or inventory")
        if provenance is not None and identity != provenance:
            raise ValueError("inconsistent runtime revision or native tool hashes")
        provenance = identity
        if report["selected"] != len(report["results"]):
            raise ValueError("inconsistent shard selected count")
        for result in report["results"]:
            name = result["name"]
            if name not in expected or name in results:
                raise ValueError(f"unknown or duplicate result: {name}")
            if result["status"] not in STATUSES:
                raise ValueError(f"unknown status: {name}")
            results[name] = result
    missing = [name for name in expected if name not in results]
    if missing:
        raise ValueError("missing original tests: " + ", ".join(missing))
    ordered = [results[name] for name in expected]
    totals = Counter(result["status"] for result in ordered)
    return {"runtime_revision": provenance[0], "reference_revision": provenance[1],
            "native_tool_sha256": provenance[2], "suite_total": len(expected),
            "selected": len(ordered),
            "counts": {status: totals[status] for status in STATUSES},
            "results": ordered}


def markdown(summary):
    counts = summary["counts"]
    total = summary["suite_total"]
    rows = ["# Original ps2autotests — full suite", "",
            f"Revision: `{summary['runtime_revision']}`", "",
            f"**{counts['PASS']}/{total} exact transcripts "
            f"({100 * counts['PASS'] / total:.2f}%)**", "",
            " | ".join(f"{status}: {counts[status]}" for status in STATUSES), "",
            "| Test | Status | Stop | PC | Dispatches |",
            "|---|---|---|---|---:|"]
    for result in summary["results"]:
        pc = result.get("final_pc")
        pc = f"0x{pc:08x}" if isinstance(pc, int) else (pc or "-")
        rows.append(f"| {result['name']} | {result['status']} | "
                    f"{result.get('exit_name') or '-'} | {pc} | "
                    f"{result.get('dispatches', '-')} |")
    return "\n".join(rows) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads((ROOT / "tools/ps2autotests_manifest.json").read_text())
    reports = [json.loads(path.read_text()) for path in
               sorted(args.reports.rglob("summary.json"))]
    summary = combine(reports, manifest)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    rendered = markdown(summary)
    (args.output / "summary.md").write_text(rendered)
    print(rendered)


if __name__ == "__main__":
    main()
