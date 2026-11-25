#!/usr/bin/env python3
"""Execute original ps2autotests ELFs and compare captured guest output."""
import argparse
import datetime
import difflib
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = Path(__file__).with_name("ps2autotests_manifest.json")
BEGIN = b"-- TEST BEGIN"
END = b"-- TEST END"
PREFIX = b"EE KPUTS: "
ESCAPE = re.compile(rb"\\x([0-9a-fA-F]{2})")


def blob_hash(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()


def verified_file(root, entry, repository, revision):
    path = root / entry["path"]
    if path.is_file():
        data = path.read_bytes()
        if blob_hash(data) == entry["git_blob"]:
            return path
    url = f"https://raw.githubusercontent.com/{repository}/{revision}/{entry['path']}"
    request = urllib.request.Request(url, headers={"User-Agent": "mips-to-llvm-ps2autotests"})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read()
    if blob_hash(data) != entry["git_blob"]:
        raise ValueError(f"upstream blob hash mismatch: {entry['path']}")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)
    return path


def normalized_lines(data):
    # Match the console reference as bytes, except CRLF and trailing
    # empty lines. Spaces, case and every nonempty line remain significant.
    lines = data.replace(b"\r\n", b"\n").split(b"\n")
    while lines and lines[-1] == b"":
        lines.pop()
    return lines


def extract_guest(log):
    # KPUTS escapes every backslash, quote and nonprintable byte as
    # \xNN. Decode individual records before concatenation: one printf
    # can be split across messages, or several lines can share a message.
    chunks = []
    for line in log.splitlines():
        if line.startswith(PREFIX):
            payload = line[len(PREFIX):]
            if re.search(rb"\\(?!x[0-9a-fA-F]{2})", payload):
                raise ValueError("malformed KPUTS byte escape")
            chunks.append(ESCAPE.sub(lambda m: bytes([int(m[1], 16)]), payload))
    return b"".join(chunks)


def test_region(data):
    lines = normalized_lines(data)
    starts = [i for i, line in enumerate(lines) if line == BEGIN]
    ends = [i for i, line in enumerate(lines) if line == END]
    if not starts:
        return None, False
    if len(starts) != 1 or len(ends) > 1 or (ends and ends[0] < starts[0]):
        raise ValueError("ambiguous test begin/end markers")
    return lines[starts[0]:ends[0] + 1] if ends else lines[starts[0]:], bool(ends)


def judge(expected, guest):
    want = normalized_lines(expected)
    reference, complete = test_region(expected)
    if not complete or want != reference:
        raise ValueError("reference lacks one complete marker-delimited transcript")
    got, complete = test_region(guest)
    if not complete:
        return ("INCOMPLETE" if got is not None else "NO_OUTPUT"), got or [], ""
    if want == got:
        return "PASS", got, ""
    diff = "\n".join(difflib.unified_diff(
        [line.decode("latin-1") for line in want],
        [line.decode("latin-1") for line in got],
        fromfile="console.expected", tofile="native.out", lineterm="")) + "\n"
    return "DIFF", got, diff


def select_tests(tests, patterns):
    return [case for case in tests if not patterns or any(
        fnmatch.fnmatchcase(case["name"], pattern) or
        fnmatch.fnmatchcase(case["name"].replace("/", "-"), pattern)
        for pattern in patterns)]


def run_case(case, args, manifest, run_dir):
    name = case["name"]
    out = run_dir / name
    out.mkdir(parents=True)
    result = {"name": name, "reference_revision": manifest["revision"],
              "elf_blob": case["elf"]["git_blob"],
              "expected_blob": case["expected"]["git_blob"]}
    start = time.monotonic()
    try:
        elf = verified_file(args.cache, case["elf"], manifest["repository"], manifest["revision"])
        expected = verified_file(args.cache, case["expected"], manifest["repository"], manifest["revision"])
        if elf.read_bytes()[:4] != b"\x7fELF":
            raise ValueError("reference ELF signature is invalid")
        work = args.output / "work" / name
        report = out / "runner.json"
        command = [sys.executable, str(ROOT / "tools/run_game.py"), str(elf),
                   "--build-dir", str(args.build_dir), "--config", args.config,
                   "--work-dir", str(work), "--functions=all",
                   "--auto-expand", str(args.auto_expand),
                   "--emit-opt", str(args.emit_opt), "--emit-jobs", str(args.emit_jobs),
                   "--max-dispatches", str(args.max_dispatches),
                   "--timeout-seconds", str(args.timeout_seconds),
                   "--loop-budget=1000000", "--loop-yield-resumes=256",
                   "--hle-startup", "--kseg-ram", "--memcard-discard-writes",
                   "--report", str(report)]
        result["command"] = command
        with (out / "run.log").open("wb") as log:
            completed = subprocess.run(command, cwd=ROOT, stdout=log,
                                       stderr=subprocess.STDOUT, check=False)
        result["runner_exit_code"] = completed.returncode
        if report.is_file():
            report_data = json.loads(report.read_text(encoding="utf-8"))
            for key in ("snapshot_kind", "exit_name", "dispatches", "final_pc",
                        "cop0_cause", "cop0_epc", "timing_observer_failed"):
                result[key] = report_data.get(key)
        guest = extract_guest((out / "run.log").read_bytes())
        (out / "guest.raw").write_bytes(guest)
        status, got, diff = judge(expected.read_bytes(), guest)
        if status == "NO_OUTPUT" and completed.returncode == 1 and not report.is_file():
            status = "ERROR"
        result["status"] = status
        result["output_lines"] = len(got)
        (out / "test.out").write_bytes(b"\n".join(got) + (b"\n" if got else b""))
        if diff:
            (out / "test.diff").write_bytes(diff.encode("latin-1"))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        result.update(status="ERROR", error=str(error))
    result["seconds"] = round(time.monotonic() - start, 3)
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("patterns", nargs="*", help="exact names or globs, default all 55")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--cache", type=Path, default=ROOT / "build/autotests-reference")
    parser.add_argument("--output", type=Path, default=ROOT / "build/autotests")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--max-dispatches", type=int, default=1000000)
    parser.add_argument("--auto-expand", type=int, default=128)
    parser.add_argument("--emit-opt", type=int, choices=range(6), default=0)
    parser.add_argument("--emit-jobs", type=int, default=2)
    parser.add_argument("--timeout-seconds", type=int, default=60,
                        help="guest replay timeout, excluding compilation")
    parser.add_argument("--report-only", action="store_true",
                        help="collect baseline outcomes without gating on nonmatching tests")
    args = parser.parse_args()
    if min(args.max_dispatches, args.emit_jobs, args.timeout_seconds) <= 0 or args.auto_expand < 0:
        parser.error("invalid execution budget")
    for attr in ("cache", "output", "build_dir"):
        setattr(args, attr, getattr(args, attr).resolve())
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    cases = select_tests(manifest["tests"], args.patterns)
    if not cases:
        parser.error("no tests match the selection")
    if args.list:
        print("\n".join(case["name"] for case in cases))
        return 0
    verified_file(args.cache, manifest["license"], manifest["repository"], manifest["revision"])
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    run_dir = args.output / "runs" / stamp
    run_dir.mkdir(parents=True)
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                              capture_output=True, text=True, check=True).stdout.strip()
    results = []
    for case in cases:
        result = run_case(case, args, manifest, run_dir)
        results.append(result)
        print(f"{result['status']:12} {result['name']} ({result['seconds']}s)", flush=True)
    counts = {status: sum(r["status"] == status for r in results)
              for status in ("PASS", "DIFF", "INCOMPLETE", "NO_OUTPUT", "ERROR")}
    summary = {"runtime_revision": revision, "reference_revision": manifest["revision"],
               "suite_total": len(manifest["tests"]), "selected": len(cases),
               "counts": counts, "results": results}
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    rows = ["# ps2autotests output comparison", "",
            f"Runtime: {revision}; reference: {manifest['revision']}.",
            f"Selected {len(cases)}/{len(manifest['tests'])}; exact output matches: {counts['PASS']}.",
            "", "| Test | Output status | Runner exit | Stop |", "|---|---|---:|---|"]
    for result in results:
        rows.append(f"| {result['name']} | {result['status']} | "
                    f"{result.get('runner_exit_code', '-')} | {result.get('exit_name', '-')} |")
    (run_dir / "summary.md").write_text("\n".join(rows) + "\n", encoding="utf-8")
    print("\n".join(rows), flush=True)
    print(f"Results: {run_dir}", flush=True)
    return 0 if args.report_only or counts["PASS"] == len(cases) else 1


if __name__ == "__main__":
    raise SystemExit(main())
