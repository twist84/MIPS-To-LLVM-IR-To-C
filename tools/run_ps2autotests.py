#!/usr/bin/env python3
"""Execute original ps2autotests ELFs and compare captured guest output."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager, ExitStack
import datetime
import difflib
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = Path(__file__).with_name("ps2autotests_manifest.json")
BEGIN = b"-- TEST BEGIN"
END = b"-- TEST END"
PREFIXES = (b"EE KPUTS: ", b"EE FILEIO: ")
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


@contextmanager
def suite_lock(directory):
    """Hold a nonblocking OS lock until the suite exits."""
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / ".ps2autotests.lock"
    # Keep the lock file: deleting a locked inode can defeat exclusion.
    with path.open("a+b") as handle:
        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise OSError(f"autotest directory is locked or unavailable: {directory}") from error
        try:
            yield
        finally:
            handle.seek(0)
            if os.name == "nt":
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def prepare_references(cases, args, manifest):
    # All reference writes finish before workers start. Keep errors per test.
    prepared = {}
    for case in cases:
        try:
            elf = verified_file(args.cache, case["elf"], manifest["repository"],
                                manifest["revision"])
            expected = verified_file(args.cache, case["expected"], manifest["repository"],
                                     manifest["revision"])
            prepared[case["name"]] = (elf, expected, None)
        except (OSError, ValueError) as error:
            prepared[case["name"]] = (None, None, str(error))
    return prepared


def format_stop(result):
    pc = result.get("final_pc")
    pc_text = f"0x{pc:08x}" if isinstance(pc, int) else "-"
    return (f"{result.get('exit_name') or '-'} pc={pc_text} "
            f"dispatches={result.get('dispatches', '-')} "
            f"lines={result.get('output_lines', '-')}")


def run_cases(cases, args, manifest, run_dir, prepared):
    # Live completion messages; deterministic manifest-order summaries.
    results = [None] * len(cases)

    def publish(index, result):
        results[index] = result
        print(f"{result['status']:12} {result['name']} ({result['seconds']}s)", flush=True)
        if result["status"] != "PASS":
            print(f"  stop: {format_stop(result)}", flush=True)

    if args.jobs == 1:
        for index, case in enumerate(cases):
            publish(index, run_case(case, args, manifest, run_dir, prepared[case["name"]]))
    else:
        with ThreadPoolExecutor(max_workers=min(args.jobs, len(cases))) as executor:
            pending = {
                executor.submit(run_case, case, args, manifest, run_dir,
                                prepared[case["name"]]): index
                for index, case in enumerate(cases)
            }
            for future in as_completed(pending):
                publish(pending[future], future.result())
    return results


def normalized_lines(data):
    # Match the console reference as bytes, except CRLF and trailing
    # empty lines. Spaces, case and every nonempty line remain significant.
    lines = data.replace(b"\r\n", b"\n").split(b"\n")
    while lines and lines[-1] == b"":
        lines.pop()
    return lines


def extract_guest(log):
    # KPUTS and FILEIO escape every backslash, quote and nonprintable byte as
    # \xNN. Decode individual records before concatenation: one printf
    # can be split across messages, or several lines can share a message.
    chunks = []
    for line in log.splitlines():
        prefix = next((p for p in PREFIXES if line.startswith(p)), None)
        if prefix is not None:
            payload = line[len(prefix):]
            if re.search(rb"\\(?!x[0-9a-fA-F]{2})", payload):
                raise ValueError("malformed console byte escape")
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


def prepare_disc(elf, directory):
    # The original SDK startup initializes SIF and RPC even for pure
    # arithmetic tests. Use the same modeled IOP services as retail
    # folder mode, with a minimal disc containing the unmodified ELF.
    directory.mkdir()
    shutil.copyfile(elf, directory / "TEST.ELF")
    (directory / "SYSTEM.CNF").write_bytes(
        b"BOOT2 = cdrom0:\\TEST.ELF;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n")
    return directory


def run_case(case, args, manifest, run_dir, reference):
    name = case["name"]
    out = run_dir / name
    out.mkdir(parents=True)
    result = {"name": name, "reference_revision": manifest["revision"],
              "elf_blob": case["elf"]["git_blob"],
              "expected_blob": case["expected"]["git_blob"]}
    start = time.monotonic()
    try:
        elf, expected, error = reference
        if error is not None:
            raise ValueError(error)
        if elf.read_bytes()[:4] != b"\x7fELF":
            raise ValueError("reference ELF signature is invalid")
        work = args.output / "work" / name
        report = out / "runner.json"
        disc = prepare_disc(elf, out / "disc")
        command = [sys.executable, str(ROOT / "tools/run_game.py"), str(elf),
                   "--build-dir", str(args.build_dir), "--config", args.config,
                   "--work-dir", str(work), "--functions=all",
                   "--auto-expand", str(args.auto_expand),
                   "--emit-opt", str(args.emit_opt), "--emit-jobs", str(args.emit_jobs),
                   "--max-dispatches", str(args.max_dispatches),
                   "--timeout-seconds", str(args.timeout_seconds),
                   "--loop-budget=1000000", "--loop-yield-resumes=256",
                   "--hle-startup", "--kseg-ram", "--memcard-discard-writes",
                   "--disc-folder", str(disc), "--sif-poll-limit=1024",
                   "--report", str(report)]
        if args.trace:
            command.append("--trace")
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
    parser.add_argument("--prebuilt-manifest", type=Path,
                        help="use prebuilt native tools only if revision and SHA-256 match")
    parser.add_argument("--max-dispatches", type=int, default=1000000)
    parser.add_argument("--auto-expand", type=int, default=128)
    parser.add_argument("--emit-opt", type=int, choices=range(6), default=0)
    parser.add_argument("--jobs", type=int, default=4,
                        help="number of independent guest tests (default: 4)")
    parser.add_argument("--emit-jobs", type=int, default=2,
                        help="LLVM compilation workers per test (default: 2)")
    parser.add_argument("--timeout-seconds", type=int, default=60,
                        help="guest replay timeout, excluding compilation")
    parser.add_argument("--trace", action="store_true", help="enable detailed runner diagnostics")
    parser.add_argument("--report-only", action="store_true",
                        help="collect baseline outcomes without gating on nonmatching tests")
    args = parser.parse_args()
    if (min(args.max_dispatches, args.timeout_seconds) <= 0 or args.auto_expand < 0
            or not 1 <= args.jobs <= 32 or not 1 <= args.emit_jobs <= 32):
        parser.error("invalid execution budget; --jobs and --emit-jobs must be in [1, 32]")
    for attr in ("cache", "output", "build_dir"):
        setattr(args, attr, getattr(args, attr).resolve())
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    cases = select_tests(manifest["tests"], args.patterns)
    if not cases:
        parser.error("no tests match the selection")
    if args.list:
        print("\n".join(case["name"] for case in cases))
        return 0
    # Protect reusable cache and build roots across harness invocations.
    try:
        with ExitStack() as locks:
            for directory in sorted({args.output, args.build_dir, args.cache}):
                locks.enter_context(suite_lock(directory))
            return execute_suite(args, manifest, cases)
    except (OSError, ValueError) as error:
        print(f"Autotest infrastructure error: {error}", file=sys.stderr)
        return 2


def verify_prebuilt_tools(path, revision, tool_hashes):
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if (manifest.get("runtime_revision") != revision or
            manifest.get("native_tool_sha256") != tool_hashes):
        raise ValueError("prebuilt autotest tools do not match revision or SHA-256")


def execute_suite(args, manifest, cases):
    suite_start = time.monotonic()
    verified_file(args.cache, manifest["license"], manifest["repository"], manifest["revision"])
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    run_dir = args.output / "runs" / stamp
    run_dir.mkdir(parents=True)
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                              capture_output=True, text=True, check=True).stdout.strip()
    # Source HEAD alone does not prove that the runner was rebuilt.
    # Reuse the configured CMake build, but rebuild both native tools
    # before recording results. This does not touch retail caches.
    build_command = ["cmake", "--build", str(args.build_dir), "--config", args.config,
                     "--target", "ee_recomp", "ee_game_runner"]
    if args.prebuilt_manifest is None:
        with (run_dir / "native-build.log").open("wb") as log:
            build = subprocess.run(build_command, cwd=ROOT, stdout=log,
                                   stderr=subprocess.STDOUT, check=False)
        if build.returncode:
            print(f"Native build failed: {run_dir / 'native-build.log'}", file=sys.stderr)
            return 2
    from run_game import native_executable
    tool_hashes = {}
    for name in ("ee_recomp", "ee_game_runner"):
        executable = native_executable(args.build_dir, name, args.config)
        tool_hashes[name] = hashlib.sha256(executable.read_bytes()).hexdigest()
    if args.prebuilt_manifest is not None:
        verify_prebuilt_tools(args.prebuilt_manifest, revision, tool_hashes)
        (run_dir / "native-build.log").write_text(
            "Verified prebuilt tools against the exact source revision and SHA-256.\n",
            encoding="utf-8")
    prepared = prepare_references(cases, args, manifest)
    execution_start = time.monotonic()
    workers = min(args.jobs, len(cases))
    print(f"Running {len(cases)} tests with {workers} test workers, "
          f"{args.emit_jobs} emit workers/test", flush=True)
    results = run_cases(cases, args, manifest, run_dir, prepared)
    execution_wall = round(time.monotonic() - execution_start, 3)
    suite_wall = round(time.monotonic() - suite_start, 3)
    cumulative = round(sum(result["seconds"] for result in results), 3)
    counts = {status: sum(r["status"] == status for r in results)
              for status in ("PASS", "DIFF", "INCOMPLETE", "NO_OUTPUT", "ERROR")}
    summary = {"runtime_revision": revision, "native_tool_sha256": tool_hashes,
               "reference_revision": manifest["revision"],
               "suite_total": len(manifest["tests"]), "selected": len(cases),
               "jobs": args.jobs, "effective_jobs": workers, "emit_jobs": args.emit_jobs,
               "suite_wall_seconds": suite_wall, "execution_wall_seconds": execution_wall,
               "cumulative_test_seconds": cumulative,
               "counts": counts, "results": results}
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    rows = ["# ps2autotests output comparison", "",
            f"Runtime: {revision}; reference: {manifest['revision']}.",
            f"Selected {len(cases)}/{len(manifest['tests'])}; exact output matches: {counts['PASS']}.",
            f"Test workers: {workers}; emit workers/test: {args.emit_jobs}.",
            f"Suite wall: {suite_wall}s; execution wall: {execution_wall}s; "
            f"cumulative test time: {cumulative}s.",
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
