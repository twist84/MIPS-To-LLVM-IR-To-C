"""Opt-in recovery timings. No additional cache scans or provenance writes."""
from contextlib import contextmanager
from contextvars import ContextVar
from datetime import datetime, timezone
from functools import wraps
import json
import math
from pathlib import Path
import re
import sys
import time

_active = ContextVar("ee_recovery_metrics", default=None)
CHILD_PREFIX = "EE_RECOVERY_METRICS "
RECOMP_TIMINGS = re.compile(
    r"ee_recomp timings: discovery=(\d+(?:\.\d+)?)ms "
    r"coverage=(\d+(?:\.\d+)?)ms IR=(\d+(?:\.\d+)?)ms")


def active():
    return _active.get()


def add(stage, seconds):
    report = active()
    if report is not None:
        bucket = report["stages"].setdefault(stage, {"seconds": 0.0, "calls": 0})
        bucket["seconds"] += seconds
        bucket["calls"] += 1


def count(name, amount=1):
    report = active()
    if report is not None:
        counters = report["counts"]
        counters[name] = counters.get(name, 0) + amount


def details(name, value):
    report = active()
    if report is not None:
        report["details"][name] = value


@contextmanager
def stage(name):
    if active() is None:
        yield
        return
    started = time.monotonic()
    try:
        yield
    finally:
        add(name, time.monotonic() - started)


def measure(name):
    def decorate(fn):
        @wraps(fn)
        def timed(*args, **kwargs):
            if active() is None:
                return fn(*args, **kwargs)
            with stage(name):
                return fn(*args, **kwargs)
        return timed
    return decorate


def observe_line(line, command_kind):
    """Consume only known timing lines emitted by the requested child."""
    report = active()
    if report is None:
        return
    if command_kind == "ee_recomp":
        match = RECOMP_TIMINGS.fullmatch(line.strip())
        if match:
            for name, value in zip(("discovery", "coverage", "llvm_ir_emission"),
                                   match.groups()):
                add(name, float(value) / 1000.0)
            count("recompiler_timing_records")
        launches = re.fullmatch(r"ee_recomp IR worker launches: (\d+)", line.strip())
        if launches and 1 <= int(launches[1]) <= 32:
            count("parallel_ir_worker_launches", int(launches[1]))
        parallel = re.fullmatch(
            r"ee_recomp parallel IR: workers=(\d+) modules=(\d+)", line.strip())
        if parallel:
            workers, modules = map(int, parallel.groups())
            if 1 <= workers <= 32 and workers <= modules <= 128:
                count("parallel_ir_batches")
                count("parallel_ir_modules", modules)
                count("parallel_ir_worker_assignments", workers)
                counters = report["counts"]
                counters["parallel_ir_peak_workers"] = max(
                    counters.get("parallel_ir_peak_workers", 0), workers)
    elif command_kind in {"verify_ir", "link_native_objects"} and line.startswith(CHILD_PREFIX):
        try:
            data = json.loads(line[len(CHILD_PREFIX):])
            if not isinstance(data, dict) or data.get("tool") != command_kind:
                return
            stages = data["stages"]
            if not isinstance(stages, dict) or any(
                    not isinstance(value, (int, float)) or not math.isfinite(value)
                    or value < 0 for value in stages.values()):
                return
            for name, value in stages.items():
                add(name, value)
            report["child_reports"].append(data)
            if command_kind == "verify_ir":
                for name in ("total_modules", "cache_hits", "cache_misses",
                             "valid_modules", "invalid_modules"):
                    value = data.get(name)
                    if isinstance(value, int) and value >= 0:
                        count("native_" + name, value)
                worker = data.get("worker_seconds")
                if isinstance(worker, (float, int)) and math.isfinite(worker) and worker >= 0:
                    report["worker_seconds"] += worker
                for entry in data.get("slowest_modules", []):
                    if (isinstance(entry, dict) and isinstance(entry.get("path"), str)
                            and isinstance(entry.get("seconds"), (float, int))
                            and math.isfinite(entry["seconds"]) and entry["seconds"] >= 0):
                        report["slowest_modules"].append(entry)
                report["slowest_modules"] = sorted(
                    report["slowest_modules"], key=lambda item: (-item["seconds"], item["path"]))[:10]
        except (ValueError, KeyError, TypeError, OverflowError):
            return


def emit_child(data):
    """Emit a machine-readable record inside the normal tool console stream."""
    import os
    if os.environ.get("EE_RECOVERY_TIMINGS") == "1":
        print(CHILD_PREFIX + json.dumps(data, sort_keys=True), flush=True)


def default_report(base, mode):
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    return Path(base).resolve().parent / f"ee-{mode}-timings-{stamp}.json"


@contextmanager
def report(mode, output, **configuration):
    output = Path(output).resolve()
    if output.exists():
        raise ValueError(f"timing report already exists: {output}")
    data = {"schema": "ee-recovery-timings-v1", "mode": mode,
            "configuration": configuration, "status": "failed",
            "stages": {}, "counts": {}, "details": {}, "child_reports": [],
            "worker_seconds": 0.0, "slowest_modules": []}
    token = _active.set(data)
    started = time.monotonic()
    try:
        yield data
        data["status"] = "success"
    except BaseException as error:
        data["error"] = str(error)
        raise
    finally:
        data["wall_seconds"] = time.monotonic() - started
        _active.reset(token)
        print(f"EE {mode} timing summary: {data['wall_seconds']:.3f}s "
              f"({data['status']}); stage timings are inclusive", flush=True)
        for name, item in sorted(data["stages"].items()):
            print(f"  {name}: {item['seconds']:.3f}s ({item['calls']} calls)", flush=True)
        for name, value in sorted(data["counts"].items()):
            print(f"  {name}: {value}", flush=True)
        if any(child["tool"] == "verify_ir" for child in data["child_reports"]):
            elapsed = data["stages"].get("native_compile_wall", {}).get("seconds", 0.0)
            misses = data["counts"].get("native_cache_misses", 0)
            data["native_throughput_modules_per_second"] = misses / elapsed if elapsed else 0.0
            print(f"  native worker time: {data['worker_seconds']:.3f}s; "
                  f"throughput: {data['native_throughput_modules_per_second']:.2f} modules/s",
                  flush=True)
            for entry in data["slowest_modules"]:
                print(f"  slow module: {entry['seconds']:.3f}s {entry['path']}", flush=True)
        try:
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open("x", encoding="utf-8", newline="\n") as stream:
                json.dump(data, stream, indent=2)
                stream.write("\n")
            print(f"EE recovery timing report: {output}", flush=True)
        except OSError as error:
            print(f"WARNING: could not save recovery timings: {error}", file=sys.stderr)
