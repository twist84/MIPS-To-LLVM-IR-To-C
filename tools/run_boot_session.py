#!/usr/bin/env python3
"""Run bounded auto-expansion sessions with an optional final boot trace.

Keep this wrapper separate from run_game.py: changing the translation driver
invalidates its ELF/object provenance cache, while changing orchestration
must not throw away previously compiled guest functions.
"""
import argparse
import hashlib
import json
import locale
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from iop_disc_catalog import build_catalog, reconcile_runtime_loadfile
from shared_ee_ir import ir_directory
from ee_discovery_evidence import EVIDENCE_NAME, record as record_discovery_evidence


LINKED_COUNT_PATTERN = re.compile(
    r"link_native_objects: PASS \((\d+) functions?,"
)


def read_function_count(path):
    """Read how many guest functions were actually committed."""
    report = read_report(path)
    count = report.get("function_count")
    return count if type(count) is int and count >= 0 else None


REPLAY_PATTERN = re.compile(r"== Executing translated EE code \(replay (\d+)\) ==")
# Shared by restart replays "(replay N)" and the live cache-miss session.
EXECUTION_HEADER_PREFIX = "== Executing translated EE code ("


def read_report(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return {}


# Only capture settings needed for reproducible retail comparisons.
# Never dump the process environment, credentials, or local file paths.
PROVENANCE_ENV = (
    "SDBZ_EE_OPT", "SDBZ_SPEED_PERCENT", "SDBZ_RUNTIME_PROFILE",
    "SDBZ_PROFILE_HORIZON", "SDBZ_PROFILE_PCS",
    "SDBZ_PROFILE_PCS_FIRST", "SDBZ_PROFILE_PCS_LAST", "SDBZ_NATIVE_ALU_CERTIFICATES",
    "SDBZ_IOP_COALESCE_IDLE_TURNS", "SDBZ_IOP_FAST_QUIET_TURNS",
    "SDBZ_IOP_CHECKPOINT_TRACE", "SDBZ_IDLE_SKIP", "SDBZ_IDLE_SKIP_FORWARD_BRANCHES",
    "SDBZ_VU1_NATIVE_OVERLAP_PROFILE", "SDBZ_VU1_ASYNC", "SDBZ_GS_ASYNC",
    "SDBZ_EE_DIRECT_DISPATCH", "SDBZ_GS_GPU_VRAM", "SDBZ_GIF_IMAGE_DIGEST",
    "SDBZ_EE_FASTMEM_WORD", "SDBZ_EE_FASTMEM_SCALAR", "SDBZ_EE_FASTMEM_QUAD", "PS2_SCALE", "PS2_RASTER_WORKGROUP",
    "SDBZ_HOST_AUDIO", "SDBZ_PAD0_CROSS_FIRST_POLL", "SDBZ_PAD0_CROSS_POLL_COUNT",
    "SDBZ_PAD0_SCRIPT", "SDBZ_PAD0_RECORD", "SDBZ_TRACE_BUFFERED", "SDBZ_DISPATCH_TRACE_EVERY",
    "SDBZ_REPORT_PROGRESS_EVERY", "SDBZ_REPORT_SNAPSHOT_MS",
    "SDBZ_STATE_TRACE_FIRST", "SDBZ_STATE_TRACE_LAST",
    "SDBZ_STATE_TRACE_EVERY",
    "SDBZ_STATE_TRACE_DETAIL",
    "SDBZ_RTC_FIXED_UTC",
    "SDBZ_REPORT_HOTSPOTS", "SDBZ_REPORT_BOUNDARY_TIMING",
    "SDBZ_WINDOW_PUMP_STRIDE", "EE_WEBGPU_WINDOW", "EE_WEBGPU_SNAPSHOT",
)


def capture_replay_provenance(repo, env, expansion_limit, replay_limit):
    def git(*args):
        try:
            result = subprocess.run(
                ["git", "-C", str(repo), *args],
                capture_output=True, text=True, check=False, timeout=10,
            )
            return result.stdout.strip() if result.returncode == 0 else None
        except (OSError, subprocess.SubprocessError):
            return None

    head = git("rev-parse", "HEAD")
    dirty = git("status", "--porcelain", "--untracked-files=no")
    replay_input = env.get("SDBZ_PAD0_REPLAY")
    replay_sha256 = None
    if replay_input:
        try:
            with open(replay_input, "rb") as stream:
                digest = hashlib.sha256()
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
                replay_sha256 = digest.hexdigest()
        except OSError:
            pass
    return {
        "schema": "ee-retail-replay-provenance-v1",
        "pad0_replay_configured": bool(replay_input),
        "pad0_replay_source_sha256": replay_sha256,
        "git_head": head,
        "tracked_tree_dirty": None if dirty is None else bool(dirty),
        "max_expansion_dispatches": expansion_limit,
        "max_replay_dispatches": replay_limit,
        "environment": {key: env.get(key) for key in PROVENANCE_ENV},
        "limitations": [
            "source_head_not_a_binary_hash",
            "untracked_files_not_checked",
            "environment_values_not_effective_runtime_defaults",
            "controller_input_reproduction_requires_valid_pad0_stream",
        ],
    }


class c_ll_file_progress:
    """Show IR file counts in a terminal or the GUI launch log."""

    def __init__(self, work, stream=None, gui=False):
        self.m_ir = ir_directory(work)
        self.m_stream = sys.stdout if stream is None else stream
        self.m_gui = gui
        self.m_enabled = gui or bool(getattr(self.m_stream, "isatty", lambda: False)())
        self.m_last = None
        self.m_visible = False
        self.m_line_width = 0

    def refresh(self):
        if not self.m_enabled:
            return
        marker = self.m_ir / "ee_expected_ll_count.txt"
        total = None
        try:
            if marker.is_file():
                value = marker.read_text(encoding="ascii").strip()
                if value.isdecimal():
                    total = int(value)
            if total is None or total <= 0:
                report = read_report(self.m_ir / "ee_functions.json")
                value = report.get("functions_discovered")
                if type(value) is int and value > 0:
                    total = value
            if total is None or total <= 0:
                return
            current = sum(1 for _ in self.m_ir.glob("*.ll"))
        except OSError:
            return

        # New runtime-discovered entries can extend the original static
        # discovery total. Never display impossible progress (N > total).
        total = max(total, current)
        status = (current, total)
        if status == self.m_last:
            return
        percentage = current * 100.0 / total
        line = f"LLVM IR [{current}/{total}] {percentage:.1f}%"
        if self.m_gui:
            self.m_stream.write(line + "\n")
        else:
            self.m_stream.write("\r" + line +
                                " " * max(0, self.m_line_width - len(line)))
            self.m_line_width = len(line)
            self.m_visible = True
        self.m_stream.flush()
        self.m_last = status

    def finish_line(self):
        if self.m_visible:
            self.m_stream.write("\n")
            self.m_stream.flush()
            self.m_visible = False
            self.m_line_width = 0


def watch_ll_files(progress, stopped):
    # At most four terminal updates per second. Filesystem scanning does
    # not hash or open individual IR files; only completed .ll filenames
    # are counted. All compiler output stays in the diagnostic log.
    while not stopped.wait(0.25):
        progress.refresh()


def watch_live_report(report_path, stopped, started):
    last = None
    while not stopped.wait(5):
        report = read_report(report_path)
        dispatches = report.get("dispatches") if isinstance(report, dict) else None
        elapsed = int(time.monotonic() - started)
        if type(dispatches) is int and dispatches != last:
            print(f"EE replay: {dispatches:,} dispatches ({elapsed}s in this pass).", flush=True)
            last = dispatches
        else:
            print(f"Replay command still running ({elapsed}s in this pass).", flush=True)


def run_session(command, log_path, append=False, progress=None, live_report=None):
    started = time.monotonic()
    stopped = threading.Event()
    worker = None
    report_worker = None
    if live_report is not None:
        report_worker = threading.Thread(
            target=watch_live_report, args=(live_report, stopped, started), daemon=True
        )
        report_worker.start()
    if progress is not None and progress.m_enabled:
        worker = threading.Thread(
            target=watch_ll_files, args=(progress, stopped), daemon=True
        )
        worker.start()
    try:
        # A temporary binary spool survives Ctrl+C while subprocess.run()
        # waits on the child. PIPE/communicate loses the child's buffered
        # output if Windows raises KeyboardInterrupt during text decoding.
        # Read and transcode once after the child exits (or is killed).
        with tempfile.TemporaryFile(mode="w+b") as capture:
            result = None
            interrupted = False
            try:
                result = subprocess.run(
                    [str(item) for item in command],
                    stdout=capture,
                    stderr=subprocess.STDOUT,
                    text=True,
                    errors="replace",
                    check=False,
                )
            except KeyboardInterrupt:
                # subprocess.run() terminates/reaps its direct child on
                # cancellation; no further guest expansion should start.
                interrupted = True
            capture.flush()
            capture.seek(0)
            output = capture.read().decode(
                locale.getpreferredencoding(False), errors="replace"
            )
            if interrupted:
                code = 130
                output += "\n# Native boot cancelled by user (Ctrl+C).\n"
            else:
                code = result.returncode
                # Preserve the previous mock/fallback semantics as well as
                # the production file-backed stdout capture.
                if result.stdout is not None:
                    output = result.stdout
    finally:
        stopped.set()
        if report_worker is not None:
            report_worker.join()
        if worker is not None:
            worker.join()
            progress.refresh()
            progress.finish_line()
    with log_path.open("a" if append else "w", encoding="utf-8") as log:
        log.write("+ " + subprocess.list2cmdline([str(item) for item in command]) + "\n")
        log.write(output)
    return code, output, time.monotonic() - started


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument(
        "--gs-snapshot", type=Path,
        help="Capture the final trace's real guest display buffer, if configured",
    )
    parser.add_argument(
        "--gs-upload-snapshot", type=Path,
        help="Capture final trace's completed GS upload, not a frame",
    )
    parser.add_argument(
        "--gs-webgpu-snapshot", type=Path,
        help="Capture final trace's native WebGPU raster (requires wgpu-native)",
    )
    parser.add_argument(
        "--gs-webgpu-window", action="store_true",
        help="Open raylib/WebGPU window during final traced replay (Windows)",
    )
    parser.add_argument("--expansion-passes", type=int, default=2)
    parser.add_argument("--expansion-per-pass", type=int, default=128)
    parser.add_argument("--max-dispatches", type=int,
                        help="Fallback dispatch limit for both phases")
    parser.add_argument("--max-expansion-dispatches", type=int,
                        help="Dispatch limit per headless expansion pass")
    parser.add_argument("--max-replay-dispatches", type=int,
                        help="Dispatch limit for the final traced replay")
    parser.add_argument(
        "--replay-after-expand", action="store_true",
        help="Legacy mode: restart the guest after each cache miss instead of resuming live",
    )
    parser.add_argument("--replay-provenance", action="store_true",
                        help="Record source revision and selected replay environment")
    parser.add_argument("--no-recompile", action="store_true")
    parser.add_argument("--skip-headless-expansion", action="store_true",
                        help="Cached graphics replay: skip the preliminary boot; "
                             "retain live cache-miss expansion in final replay")
    parser.add_argument(
        "--quick-probe", action="store_true",
        help="Use the last expansion pass as the final report, with no traced replay",
    )
    parser.add_argument(
        "--ll-progress", action="store_true",
        help="Show live in-place [completed/expected] .ll file count on a terminal",
    )
    parser.add_argument(
        "--disc-root", type=Path,
        help="Extracted CD-ROM root (default: directory containing the ELF)",
    )
    parser.add_argument(
        "--no-iop-catalog", action="store_true",
        help="Disable the non-executing diagnostic IOP disc catalog",
    )
    parser.add_argument(
        "--iop-auto-irx", action="append", default=[],
        metavar="GUEST_PATH_OR_NAME",
        help="Resolve a discovered IRX name from the game disc and append "
             "it to the explicit IOP bootstrap load list (repeat in "
             "provider-first order). Never infers execution order.",
    )
    parser.add_argument(
        "--driver", type=Path,
        default=Path(__file__).with_name("run_game.py"),
        help=argparse.SUPPRESS,
    )
    args, forwarded = parser.parse_known_args(argv)
    for option, value in (
        ("--max-dispatches", args.max_dispatches),
        ("--max-expansion-dispatches", args.max_expansion_dispatches),
        ("--max-replay-dispatches", args.max_replay_dispatches),
    ):
        if value is not None and not 1 <= value <= 0xFFFFFFFF:
            parser.error(f"{option} must fit a positive uint32")
    expansion_limit = args.max_expansion_dispatches or args.max_dispatches
    replay_limit = args.max_replay_dispatches or args.max_dispatches
    expansion_args = [f"--max-dispatches={expansion_limit}"] if expansion_limit is not None else []
    replay_args = [f"--max-dispatches={replay_limit}"] if replay_limit is not None else []
    if not 1 <= args.expansion_passes <= 64:
        parser.error("--expansion-passes must be between 1 and 64")
    if not 1 <= args.expansion_per_pass <= 1024:
        parser.error("--expansion-per-pass must be between 1 and 1024")
    if args.skip_headless_expansion and (
            not args.no_recompile or args.quick_probe or args.replay_after_expand or
            not (args.gs_webgpu_window or args.gs_webgpu_snapshot is not None)):
        parser.error("--skip-headless-expansion requires a cached graphics replay with live expansion")
    if args.quick_probe and (args.gs_snapshot or args.gs_upload_snapshot or
                             args.gs_webgpu_snapshot or args.gs_webgpu_window):
        parser.error("GS snapshots and the WebGPU window require a full boot session")
    if any(item == "--trace" or item == "--link-only" or
           item == "--auto-expand" or item.startswith("--auto-expand=") or
           item == "--no-recompile" for item in forwarded):
        parser.error("trace, auto-expand, and no-recompile are managed by this wrapper")
    if not args.driver.is_file():
        parser.error(f"driver does not exist: {args.driver}")

    work = args.work_dir.resolve()
    def preserve_discovery(output):
        try:
            record_discovery_evidence(work, args.elf, output)
        except (OSError, ValueError, KeyError) as exc:
            print(f"EE discovery evidence unavailable: {exc}", file=sys.stderr, flush=True)
    report = args.report.resolve()
    catalog_path = work / ("quick-probe-iop-disc-catalog.json" if args.quick_probe
                           else "iop-disc-catalog.json")
    boot_log = work / ("quick-probe.log" if args.quick_probe else "boot.log")
    expansion_log = boot_log if args.quick_probe else work / "expansion.log"
    metadata_path = ir_directory(work) / "ee_functions.json"
    provenance_path = work / ("quick-probe-provenance.json" if args.quick_probe
                              else "replay-provenance.json")
    if report in (args.elf.resolve(), args.driver.resolve(), catalog_path,
                  boot_log, expansion_log, metadata_path, provenance_path, work / "translation-cache.json",
                  work / EVIDENCE_NAME):
        parser.error("report path must be distinct from inputs, logs, and cache metadata")
    work.mkdir(parents=True, exist_ok=True)
    report.parent.mkdir(parents=True, exist_ok=True)
    if args.no_iop_catalog and args.iop_auto_irx:
        parser.error("--iop-auto-irx requires disc catalog discovery")
    if args.iop_auto_irx and any(
            arg.startswith("--iop-irx=") or
            arg == "--iop-irx-list" or
            arg.startswith("--iop-irx-list=")
            for arg in forwarded):
        parser.error("--iop-auto-irx cannot mix with manual IOP IRX lists")
    catalog = None
    if not args.no_iop_catalog:
        try:
            catalog = build_catalog(args.elf, args.disc_root)
            catalog_path.write_text(
                json.dumps(catalog, indent=2) + "\n", encoding="utf-8"
            )
            print(
                f"IOP disc discovery: {catalog['resolved_count']}/"
                f"{len(catalog['candidates'])} candidate files resolved "
                f"({catalog_path}); no IOP modules started",
                flush=True,
            )
            print(
                f"IOP static import inventory: "
                f"{catalog['static_import_count']} symbols in "
                f"{catalog['static_import_module_count']} file-backed IRXs; "
                "see iop-disc-catalog.json (not runtime load evidence)",
                flush=True,
            )
        except (OSError, ValueError) as exc:
            catalog_path.unlink(missing_ok=True)
            # Diagnostic discovery must not make EE boot or --no-recompile
            # fail when a disc is incomplete or a synthetic test ELF is
            # supplied. Never turn an invalid catalog into a load list.
            print(f"IOP disc discovery unavailable: {exc}", flush=True)

    for request in args.iop_auto_irx:
        if catalog is None:
            parser.error("IOP disc catalog could not be built")
        name = request.casefold()
        matches = [
            item for item in catalog["candidates"]
            if item["guest_path"].casefold() == name or
            item["guest_path"].replace("\\", "/").split("/")[-1]
                .split(";")[0].casefold() == name
        ]
        # A static ELF string is not a runtime load request or a
        # dependency graph. The caller chooses every bootstrap module.
        if len(matches) != 1 or matches[0]["status"] != "resolved":
            parser.error(
                f"IOP bootstrap IRX {request!r} is missing or ambiguous"
            )
        forwarded.append(f"--iop-irx={matches[0]['host_path']}")
        print(
            f"IOP explicit bootstrap selection: "
            f"{matches[0]['guest_path']} -> {matches[0]['host_path']}",
            flush=True,
        )

    gui_progress = os.environ.get("SDBZ_LAUNCHER_LIVE_PROGRESS") == "1"
    progress = c_ll_file_progress(work, gui=gui_progress) if args.ll_progress or gui_progress else None
    # Keep legacy run_session calls unchanged unless the console progress
    # feature was explicitly requested. Diagnostic tools and tests use the
    # existing three-argument interface without a progress keyword.
    progress_kwargs = {"progress": progress} if progress is not None else {}
    if gui_progress:
        progress_kwargs["live_report"] = report
    snapshots = [
        ("--gs-snapshot", args.gs_snapshot),
        ("--gs-upload-snapshot", args.gs_upload_snapshot),
        ("--gs-webgpu-snapshot", args.gs_webgpu_snapshot),
    ]
    requested_paths = [
        path.resolve() for _, path in snapshots if path is not None
    ]
    if len(requested_paths) != len(set(requested_paths)) or any(
            path in (report, expansion_log, boot_log, catalog_path, provenance_path, work / EVIDENCE_NAME)
            for path in requested_paths):
        parser.error("GS snapshot paths must be distinct from diagnostic outputs")
    if args.replay_provenance:
        # Replace evidence before executing any replay, never archive an
        # old revision/settings record from a previous run.
        provenance_path.unlink(missing_ok=True)
        provenance = capture_replay_provenance(
            Path(__file__).resolve().parents[1], os.environ,
            expansion_limit, replay_limit,
        )
        provenance_path.write_text(
            json.dumps(provenance, indent=2) + "\n", encoding="utf-8",
        )
    expansion_log.write_text("", encoding="utf-8")
    base = [
        sys.executable, str(args.driver.resolve()), str(args.elf.resolve()),
        f"--work-dir={work}", f"--report={report}", *forwarded,
        *(["--replay-after-expand"] if args.replay_after_expand else []),
    ]

    if args.gs_webgpu_window and "live_report" in progress_kwargs:
        print("Expansion runs headless. The game window opens for the final traced replay.", flush=True)

    reuse = args.no_recompile
    if args.skip_headless_expansion:
        message = "Headless expansion skipped: cached graphics replay with live cache misses"
        print(message, flush=True)
        expansion_log.write_text("# " + message + "\n", encoding="utf-8")
    for index in range(0 if args.skip_headless_expansion else args.expansion_passes):
        command = [*base, *expansion_args, f"--auto-expand={args.expansion_per_pass}"]
        if reuse:
            command.append("--no-recompile")
        print(
            f"=== Expansion pass {index + 1}/{args.expansion_passes} "
            f"(up to {args.expansion_per_pass} new functions) ===",
            flush=True,
        )
        # Cache mode starts from a persisted count. Fresh translation
        # creates its initial linked baseline before replay zero.
        # A jump-table relink can compile many functions in one replay.
        prior_count = read_function_count(metadata_path) if reuse else None
        report.unlink(missing_ok=True)
        code, output, elapsed = run_session(
            command, expansion_log, append=True, **progress_kwargs
        )
        preserve_discovery(output)
        state = read_report(report)
        replays = REPLAY_PATTERN.findall(output)
        last_replay = int(replays[-1]) if replays else -1
        final_pc = state.get("final_pc")
        pc_text = f"0x{final_pc:08x}" if isinstance(final_pc, int) else "unknown"
        if not reuse:
            # The baseline is the last link before the FIRST execution,
            # whether a restart replay or a live cache-miss session. Live
            # mode relinks after its header; counting those links as the
            # baseline reports new_functions=0 and skips later passes.
            initial_links = LINKED_COUNT_PATTERN.findall(
                output.split(EXECUTION_HEADER_PREFIX, 1)[0]
            )
            prior_count = int(initial_links[-1]) if initial_links else None
        current_count = read_function_count(metadata_path)
        added = (
            current_count - prior_count
            if prior_count is not None and current_count is not None and
               current_count >= prior_count
            else None
        )
        added_text = str(added) if added is not None else "unknown"
        summary = (
            f"pass {index + 1}: replay={last_replay} "
            f"new_functions={added_text} "
            f"dispatches={state.get('dispatches', '?')} "
            f"stop={pc_text} exit={code} elapsed={elapsed:.2f}s"
        )
        print(summary, flush=True)
        # Preserve the cap decision in the uploaded expansion log too.
        # stdout alone is not retained by the local launcher.
        with expansion_log.open("a", encoding="utf-8") as log:
            log.write("# " + summary + "\n")

        if code == 130:
            if not args.quick_probe:
                shutil.copyfile(expansion_log, boot_log)
            print(
                "Native boot cancelled by user (Ctrl+C); no further "
                f"expansion or guest replay. Log: {boot_log}",
                file=sys.stderr, flush=True,
            )
            return 130

        if code not in (0, 3, 4):
            if not args.quick_probe:
                shutil.copyfile(expansion_log, boot_log)
            print(
                f"Expansion failed; diagnostics are in {boot_log}",
                file=sys.stderr, flush=True,
            )
            return code

        # The expansion cap counts newly emitted FUNCTIONS, not replays.
        # Prefer the metadata delta; use the legacy replay sentinel only
        # when the driver supplies no usable function-count metadata.
        # Unknown guest stops must not trigger an unbounded retry.
        hit_cap = (
            added >= args.expansion_per_pass
            if added is not None else last_replay == args.expansion_per_pass
        )
        exhausted = (
            code == 3 and hit_cap and
            state.get("exit_reason") == 3 and
            state.get("dispatch_status") == 0 and
            isinstance(final_pc, int) and final_pc != 0
        )
        if exhausted and index + 1 < args.expansion_passes:
            print(
                "Expansion bound reached; resuming from the validated "
                "translation cache.", flush=True,
            )
            reuse = True
            continue
        if exhausted:
            print(
                "Session expansion budget reached; compiled functions "
                "are preserved for the next local run.", flush=True,
            )
        if args.quick_probe:
            return code
        break

    # Emit the *full* guest trace once. Intermediate replays don't need
    # repetitive dispatch logs, but the final snapshot must stay useful
    # for diagnosing the next kernel service or translation failure.
    print(f"=== Final traced replay: {boot_log} ===", flush=True)
    # Request images only for the FINAL traced boot, never for temporary
    # auto-expansion replays. A missing image must not be a stale file from
    # a previous local invocation.
    graphics_replay = args.gs_webgpu_window or args.gs_webgpu_snapshot is not None
    final_expansion = args.expansion_per_pass if graphics_replay else 0
    if graphics_replay:
        print(
            f"Graphics replay can cache up to {final_expansion} new functions. "
            "A missing function pauses and relinks within the same live guest "
            "process; a replay is needed only if the expansion budget is exhausted. "
            "Memory card writes remain in memory during expansion.",
            flush=True,
        )
    final_command = [
        *base, *replay_args, "--no-recompile", f"--auto-expand={final_expansion}", "--trace",
    ]
    for option, path in snapshots:
        if path is not None:
            target = path.resolve()
            target.parent.mkdir(parents=True, exist_ok=True)
            target.unlink(missing_ok=True)
            final_command.append(f"{option}={target}")
    if args.gs_webgpu_window:
        final_command.append("--gs-webgpu-window")
    report.unlink(missing_ok=True)
    code, final_output, elapsed = run_session(
        final_command, boot_log, **progress_kwargs
    )
    preserve_discovery(final_output)
    if code == 130:
        print(
            "Native final replay cancelled by user (Ctrl+C); "
            f"no guest completion is inferred. Log: {boot_log}",
            file=sys.stderr, flush=True,
        )
        return 130
    if catalog is not None:
        try:
            reconcile_runtime_loadfile(catalog, final_output)
            catalog_path.write_text(
                json.dumps(catalog, indent=2) + "\n", encoding="utf-8"
            )
            print(
                f"IOP committed LOADFILE requests: "
                f"{catalog['runtime_load_attempt_count']} attempts, "
                f"{catalog['runtime_load_count']} successful module starts "
                f"({catalog_path})",
                flush=True,
            )
        except (ValueError, OSError) as exc:
            # The original static catalog remains explicitly unobserved.
            # Diagnostic parsing must not invent loads or abort EE boot.
            print(f"IOP committed LOADFILE evidence unavailable: {exc}",
                  file=sys.stderr, flush=True)
    state = read_report(report)
    final_pc = state.get("final_pc")
    pc_text = f"0x{final_pc:08x}" if isinstance(final_pc, int) else "unknown"
    print(
        f"Final trace: dispatches={state.get('dispatches', '?')} "
        f"stop={pc_text} exit={code} elapsed={elapsed:.2f}s. "
        f"Logs: {boot_log}, {expansion_log}",
        flush=True,
    )
    return code


def cli_main(argv=None):
    try:
        return main(argv)
    except KeyboardInterrupt:
        # Also handle Ctrl+C outside a child replay (catalogue, reports,
        # progress setup) without printing a Python traceback.
        print(
            "\nNative boot cancelled by user (Ctrl+C).",
            file=sys.stderr, flush=True,
        )
        return 130


if __name__ == "__main__":
    raise SystemExit(cli_main())
