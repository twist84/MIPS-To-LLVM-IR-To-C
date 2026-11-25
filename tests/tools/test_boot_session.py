#!/usr/bin/env python3
"""Unit tests for bounded, cache-preserving native EE boot sessions."""
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import sys
from types import SimpleNamespace
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "run_boot_session", ROOT / "tools" / "run_boot_session.py",
)
SESSION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SESSION)


def run_case(passes, initial_reuse=False, diagnostics=False,
             initial_function_count=None, expansion_per_pass=2,
             snapshots=False, quick_probe=False, graphics=None, legacy=False,
             dispatch_limits=()):
    with tempfile.TemporaryDirectory(prefix="ee-boot-session-") as temp:
        work = Path(temp) / "work"
        report = work / "first-boot.json"
        elf = Path(temp) / "synthetic.elf"
        elf.write_bytes(b"synthetic test input, never executed")
        calls = []
        if quick_probe:
            work.mkdir(parents=True)
            (work / "boot.log").write_text("previous full trace", encoding="utf-8")
        metadata = work / "ir" / "ee_functions.json"
        if initial_function_count is not None:
            metadata.parent.mkdir(parents=True, exist_ok=True)
            metadata.write_text(
                json.dumps({"function_count": initial_function_count}),
                encoding="utf-8",
            )

        def fake_run(command, **kwargs):
            assert kwargs["check"] is False
            assert kwargs["text"] is True
            assert kwargs["stderr"] is SESSION.subprocess.STDOUT
            assert not report.exists(), "a previous pass's report survived"
            calls.append(list(map(str, command)))
            trace = "--trace" in command
            index = len(calls) - 1
            item = passes[index]
            assert trace == item["trace"], (index, command, item)
            state = {
                "schema": "ee-game-run-report-v1",
                "dispatches": 140 + index * 40,
                "exit_reason": item.get("exit_reason", 3),
                "dispatch_status": item.get("dispatch_status", 0),
                "final_pc": 0x004E2640 + 16 * index,
            }
            report.write_text(json.dumps(state), encoding="utf-8")
            if "function_count" in item:
                metadata.parent.mkdir(parents=True, exist_ok=True)
                metadata.write_text(
                    json.dumps({"function_count": item["function_count"]}),
                    encoding="utf-8",
                )
            replay = item.get("replay")
            if item.get("live"):
                header = ("== Executing translated EE code "
                          "(live cache-miss resume) ==\\n")
            elif replay is not None:
                header = f"== Executing translated EE code (replay {replay}) ==\\n"
            else:
                header = ""
            output = header + item.get("message", "EE stopped")
            if "initial_link" in item:
                output = (
                    f"link_native_objects: PASS ({item['initial_link']} functions, "
                    "fixture)\\n" + output
                )
            return SimpleNamespace(
                returncode=item.get("code", 3),
                stdout=output + "\\n",
            )

        argv = [
            str(elf), "--work-dir", str(work), "--report", str(report),
            "--expansion-passes=2",
            f"--expansion-per-pass={expansion_per_pass}",
            "--driver", str(ROOT / "tools" / "run_game.py"),
            "--hle-startup", "--kseg-ram", "--max-dispatches=1000",
        ]
        argv.extend(dispatch_limits)
        if initial_reuse:
            argv.append("--no-recompile")
        if legacy:
            argv.append("--replay-after-expand")
        if quick_probe:
            argv.extend(("--quick-probe", "--no-iop-catalog"))
        if snapshots:
            work.mkdir(parents=True, exist_ok=True)
            display = work / "gs-display.ppm"
            upload = work / "gs-upload.ppm"
            display.write_bytes(b"stale display")
            upload.write_bytes(b"stale upload")
            argv.extend((
                "--gs-snapshot", str(display),
                "--gs-upload-snapshot", str(upload),
            ))
        if graphics == "window":
            argv.append("--gs-webgpu-window")
        elif graphics == "snapshot":
            argv.append(f"--gs-webgpu-snapshot={work / 'webgpu.ppm'}")
        if diagnostics:
            argv.extend((
                "--trace-entry=0x0018cf68",
                "--watch-word=0x01ffec14",
                "--trace-store=0x01ffec14",
                "--trace-v0=0x00194478:0x00194794",
            ))
        with patch.object(SESSION.subprocess, "run", side_effect=fake_run):
            code = SESSION.main(argv)
        assert len(calls) == len(passes), calls
        expansion = next((arg.split("=")[1] for arg in dispatch_limits
                          if arg.startswith("--max-expansion-dispatches=")), "1000")
        replay = next((arg.split("=")[1] for arg in dispatch_limits
                       if arg.startswith("--max-replay-dispatches=")), "1000")
        for call in calls:
            expected = replay if "--trace" in call else expansion
            assert [arg for arg in call if arg.startswith("--max-dispatches=")] == [
                f"--max-dispatches={expected}"
            ], calls
            assert not any(arg.startswith(("--max-expansion-dispatches=", "--max-replay-dispatches="))
                           for arg in call), calls
        assert all(("--replay-after-expand" in call) == legacy for call in calls), calls
        assert all("--trace" not in call for call in calls[:-1]), calls
        if quick_probe:
            assert all("--trace" not in call for call in calls), calls
            assert report.is_file()
            assert (work / "boot.log").read_text() == "previous full trace"
            log = (work / "quick-probe.log").read_text()
            return code, calls, log, log
        if len(passes) > 1:
            assert "--trace" in calls[-1], calls
        if initial_reuse:
            assert "--no-recompile" in calls[0], calls
        if snapshots:
            # Temporary expansion passes must not emit image artifacts;
            # the final replay is the only one with the two options.
            assert all(
                not any(arg.startswith("--gs-") for arg in call)
                for call in calls[:-1]
            ), calls
            assert f"--gs-snapshot={display.resolve()}" in calls[-1]
            assert f"--gs-upload-snapshot={upload.resolve()}" in calls[-1]
            # No emulated images were produced: old files were removed.
            assert not display.exists() and not upload.exists()
        if diagnostics:
            assert all(
                "--trace-entry=0x0018cf68" in call and
                "--watch-word=0x01ffec14" in call and
                "--trace-store=0x01ffec14" in call and
                "--trace-v0=0x00194478:0x00194794" in call
                for call in calls
            ), calls
            assert "--trace-entry=0x0018cf68" in (
                work / "boot.log"
            ).read_text(), "missing diagnostic in boot command"
        if len(passes) > 2:
            assert "--no-recompile" in calls[1], calls
        if len(passes) > 1:
            final_limit = expansion_per_pass if graphics else 0
            assert f"--auto-expand={final_limit}" in calls[-1], calls
        if graphics:
            option = f"--gs-webgpu-{graphics}"
            assert any(arg.startswith(option) for arg in calls[-1])
            assert all(not any(arg.startswith(option) for arg in call) for call in calls[:-1])
            assert "--no-recompile" in calls[-1]
        assert (work / "expansion.log").is_file()
        assert (work / "boot.log").is_file()
        return code, calls, (work / "expansion.log").read_text(), (work / "boot.log").read_text()


def test_ll_counter():
    # The progress bar belongs only to a real console and never writes
    # thousands of persistent log lines. Repeated polls have no output.
    class c_fake_terminal(io.StringIO):
        def isatty(self):
            return True

    with tempfile.TemporaryDirectory(prefix="ee-ll-progress-") as temp:
        ir_dir = Path(temp) / "ir"
        ir_dir.mkdir()
        terminal = c_fake_terminal()
        progress = SESSION.c_ll_file_progress(temp, stream=terminal)
        assert progress.m_enabled
        progress.refresh()  # No guessed or hardcoded denominator.
        assert terminal.getvalue() == ""

        (ir_dir / "ee_expected_ll_count.txt").write_text(
            "7550\n", encoding="ascii"
        )
        progress.refresh()
        progress.refresh()
        (ir_dir / "first.ll").write_text("define void @first() {}\n")
        progress.refresh()
        progress.refresh()
        (ir_dir / "second.ll").write_text("define void @second() {}\n")
        progress.refresh()
        before_done = terminal.getvalue()
        assert before_done.count("\r") == 3, repr(before_done)
        assert "\n" not in before_done, repr(before_done)
        assert "LLVM IR [0/7550] 0.0%" in before_done
        assert "LLVM IR [1/7550] 0.0%" in before_done
        assert "LLVM IR [2/7550] 0.0%" in before_done
        progress.finish_line()
        assert terminal.getvalue().endswith("\n")
        progress.refresh()  # Preserve last display across sessions.
        assert terminal.getvalue().count("\r") == 3

        # In a pipe/CI log, no carriage returns or per-file updates.
        piped = io.StringIO()
        quiet = SESSION.c_ll_file_progress(temp, stream=piped)
        quiet.refresh()
        quiet.finish_line()
        assert piped.getvalue() == ""

        gui = SESSION.c_ll_file_progress(temp, stream=piped, gui=True)
        gui.refresh()
        gui.refresh()
        gui.finish_line()
        assert piped.getvalue() == "LLVM IR [2/7550] 0.0%\n"

        # If an old discovery marker is absent, use actual persisted
        # function-count metadata (cached --no-recompile sessions).
        (ir_dir / "ee_expected_ll_count.txt").unlink()
        (ir_dir / "ee_functions.json").write_text(
            json.dumps({"functions_discovered": 5}), encoding="utf-8"
        )
        alt = c_fake_terminal()
        cached = SESSION.c_ll_file_progress(temp, stream=alt)
        cached.refresh()
        assert "LLVM IR [2/5] 40.0%" in alt.getvalue()
        cached.finish_line()

        # Dynamic functions beyond the original static discovery total
        # must never create an impossible [current > expected] ratio.
        (ir_dir / "ee_functions.json").write_text(
            json.dumps({"functions_discovered": 1}), encoding="utf-8"
        )
        dynamic = c_fake_terminal()
        updated = SESSION.c_ll_file_progress(temp, stream=dynamic)
        updated.refresh()
        assert "LLVM IR [2/2] 100.0%" in dynamic.getvalue()
        updated.finish_line()

        # Cached metadata may report a total already exceeded by
        # newly discovered guest entries. Percentages never exceed 100%.
        marker = ir_dir / "ee_expected_ll_count.txt"
        marker.write_text("1\n", encoding="ascii")
        capped = c_fake_terminal()
        latest = SESSION.c_ll_file_progress(temp, stream=capped)
        latest.refresh()
        assert "LLVM IR [2/2] 100.0%" in capped.getvalue()

        # Use the actual denominator for a nontrivial percentage;
        # cache-restored sessions must not show an integer-only ratio.
        marker.write_text("8\n", encoding="ascii")
        fractional = c_fake_terminal()
        precise = SESSION.c_ll_file_progress(temp, stream=fractional)
        precise.refresh()
        assert "LLVM IR [2/8] 25.0%" in fractional.getvalue()


def test_windows_launcher_preserves_guest_cache():
    # The Windows launcher must try a safe host-only relink before any
    # checkout-triggered cache miss falls back to recompiling all guest IR.
    launcher = (ROOT / "run_ps2_game.bat").read_text(encoding="utf-8")
    # Progress is always enabled, including --quick (which is what the
    # retail boot user runs); do not duplicate it in --full arguments.
    assert 'python tools\\run_boot_session.py "%GAME_ELF%" ^\n    --ll-progress ^' in launcher
    assert 'set "SESSION_ARGS=--ll-progress' not in launcher
    probe = launcher.split(
        "rem Git checkout may rebuild host tools", 1
    )[1].split("echo === Starting extracted-disc", 1)[0]
    check = probe.index("--no-recompile --link-only")
    recovery = probe.index("--relink-existing --link-only")
    assert check < recovery, "must validate existing library first"
    cache_probes = probe.split(
        "rem Cache recovery is reconstruction-only", 1
    )[0]
    assert cache_probes.count('--emit-opt=0') == 2, "both probes use boot optimization"
    assert cache_probes.count('--work-dir "%WORK_DIR%"') == 2, "same guest cache"
    # Optimized full replays defer source validation to the shared IR
    # builder. Only profile 0 enters the native-library probes below.
    optimized = cache_probes.index(
        'if not "%SDBZ_EE_OPT%"=="0" if exist "%WORK_DIR%\\translation-cache.json" (')
    baseline = cache_probes.index(
        'if not defined CACHE_FLAG if exist "%WORK_DIR%\\translation-cache.json" (')
    assert optimized < baseline
    assert 'set "CACHE_FLAG=--no-recompile"' in cache_probes[optimized:baseline]
    assert '--link-only' not in cache_probes[optimized:baseline]
    assert cache_probes[baseline:].count('set "CACHE_FLAG=--no-recompile"') == 2

    # All optimization profiles now use the canonical IR directly.
    variants = probe.split("rem Build only the selected profile", 1)[1]
    assert variants.count('python tools\\build_optimized_cache.py') == 3
    assert variants.count('--source-work-dir "%WORK_DIR%"') == 3
    assert '--source-opt=2' not in variants
    assert '--work-dir "%WORK_DIR%\\IR-O2" --emit-opt=4' in variants
    assert '--work-dir "%WORK_DIR%\\O2-native" --emit-opt=5' in variants
    assert '--work-dir "%WORK_DIR%-O2" --emit-opt=2' not in variants
    assert 'set "DIRECT_REPLAY_ARG=--skip-headless-expansion"' in variants
    assert 'set "WARM_VALIDATION_ARG=--defer-warm-validation"' in variants
    assert variants.count('%WARM_VALIDATION_ARG%') == 3
    assert "if not errorlevel 1 (" in probe, "failed relink must not reuse"
    assert "full IR rebuild required" in probe

    # Normal boots automatically regenerate when validation, relinking and
    # strict non-destructive recovery all fail. Existing --matched and
    # --recover-cache branches must return before that fallback.
    rebuild_text = (
        'if not defined CACHE_FLAG (\n'
        '    echo Guest translation cache is not reusable; regenerating IR automatically.'
    )
    rebuild = probe.index(rebuild_text)
    strict = probe.index("rem Strict unattended recovery")
    matched = probe.index("rem A paired experiment MUST use the existing guest IR")
    recover_mode = probe.index(
        'if /I "%REQUESTED_SESSION_MODE%"=="recover" (\n'
        '    if defined CACHE_FLAG'
    )
    assert check < recovery < recover_mode < strict < matched < rebuild
    assert "recover_translation_cache.py --elf" in probe[strict:rebuild]
    assert 'set "CACHE_FLAG=--no-recompile"' in probe[strict:rebuild]
    assert 'if not defined CACHE_FLAG (' in probe[rebuild:]
    assert "call :confirm_guest_cache_rebuild" not in launcher
    assert "choice /C YN" not in launcher
    assert ":confirm_guest_cache_rebuild" not in launcher
    assert launcher.index(rebuild_text) < launcher.index(
        "echo === Starting extracted-disc"
    )

    # The full retail replay must be able to continue genuine guest IOP
    # execution past the old fixed 100k ceiling; quick probes stay short.
    assert 'set "DEFAULT_MAX_DISPATCHES=100000"' in launcher
    assert 'set "DEFAULT_MAX_DISPATCHES=6000000"' in launcher
    assert 'if not defined MAX_DISPATCHES set "MAX_DISPATCHES=%DEFAULT_MAX_DISPATCHES%"' in launcher
    assert '--max-dispatches=%MAX_DISPATCHES%' in launcher
    assert '--timeout-seconds=%BOOT_TIMEOUT_SECONDS%' in launcher
    assert 'if /I "%~1"=="--replay-after-expand" (' in launcher
    assert '%SESSION_ARGS% %CACHE_MISS_ARGS%' in launcher
    assert '--max-dispatches=100000 ^' not in launcher
    assert (
        'if /I "%REQUESTED_SESSION_MODE%"=="quick" '
        'set "SESSION_ARGS=--quick-probe --no-iop-catalog"'
    ) in launcher
    quick_session = launcher.index(
        'if /I "%REQUESTED_SESSION_MODE%"=="quick" '
        'set "SESSION_ARGS=--quick-probe --no-iop-catalog"'
    )
    assert launcher.index('if not defined CACHE_FLAG set "SESSION_ARGS="') < quick_session
    # There is also an earlier --full block configuring WebGPU
    # defaults. Compare the two session-mode assignments specifically.
    assert quick_session < launcher.index(
        'if /I "%REQUESTED_SESSION_MODE%"=="full" set "SESSION_ARGS="'
    )
    assert 'set "EE_WEBGPU_SNAPSHOT=0"' in launcher
    assert 'set "EE_WEBGPU_WINDOW=0"' in launcher
    assert launcher.index('set "EE_WEBGPU_SNAPSHOT=0"') < launcher.index(
        'if /I "%EE_WEBGPU_SNAPSHOT%"=="1"')
    assert launcher.index('set "EE_WEBGPU_WINDOW=0"') < launcher.index(
        'if /I "%EE_WEBGPU_WINDOW%"=="1"')

    # The user's repeatable 6M / 600s SDBZ graphics replay should not
    # require entering a dozen SET commands before every full run.
    # Existing caller-set overrides are respected; --quick stays headless
    # with its original 100k/120s default budget.
    assert 'if /I "%REQUESTED_SESSION_MODE%"=="full" (\n' in launcher
    assert 'if not defined EE_WEBGPU_SNAPSHOT set "EE_WEBGPU_SNAPSHOT=1"' in launcher
    assert 'if not defined EE_WEBGPU_WINDOW set "EE_WEBGPU_WINDOW=1"' in launcher
    assert launcher.index('if /I "%REQUESTED_SESSION_MODE%"=="full" (') < (
        launcher.index('if not defined EE_WEBGPU_SNAPSHOT')
    )
    assert 'if /I "%REQUESTED_SESSION_MODE%"=="quick" (\n' in launcher
    assert 'if not defined SDBZ_REPORT_SNAPSHOT_MS set "SDBZ_REPORT_SNAPSHOT_MS=2000"' in launcher
    assert 'if not defined SDBZ_DISPATCH_TRACE_EVERY set "SDBZ_DISPATCH_TRACE_EVERY=1024"' in launcher
    assert 'if not defined LOOP_YIELD_RESUMES set "LOOP_YIELD_RESUMES=16"' in launcher
    assert 'if /I "%REPORT_NAME%"=="first-boot.json" (' in launcher
    assert 'set "BOOT_TIMEOUT_SECONDS=600"' in launcher
    assert 'set "BOOT_TIMEOUT_SECONDS=120"' in launcher
    assert '--loop-yield-resumes=%LOOP_YIELD_RESUMES%' in launcher
    assert '--max-dispatches=%MAX_DISPATCHES%' in launcher

    # Cross experiments are an explicit option; neutral can clear
    # inherited manual pad injection, without changing the caller's
    # variables because the launcher is wrapped in SETLOCAL.
    assert 'set "REQUESTED_PAD_PROFILE="' in launcher
    assert 'if /I "%~1"=="--cross" (' in launcher
    assert 'if /I "%~1"=="--neutral" (' in launcher
    assert 'set "SDBZ_PAD0_CROSS_FIRST_POLL=200"' in launcher
    assert 'set "SDBZ_PAD0_CROSS_POLL_COUNT=8"' in launcher
    assert 'set "SDBZ_PAD0_CROSS_FIRST_POLL="' in launcher
    assert 'set "SDBZ_PAD0_CROSS_POLL_COUNT="' in launcher
    assert 'set "CAPTURE_PROFILE=neutral"' in launcher
    assert 'set "CAPTURE_PROFILE=pad-custom"' in launcher
    assert 'set "CAPTURE_PROFILE=cross"' in launcher

    # A full run always captures a uniquely named upload-ready archive.
    # It must never silently overwrite the neutral/Cross evidence nor
    # include the expensive cached guest .ll or .o files.
    archive = launcher.split("\n:archive_full_run\n", 1)[1]
    assert "Get-ChildItem -LiteralPath '%WORK_DIR%' -File" in archive
    for diagnostic in (
        "boot.log", "expansion.log", "first-boot.json",
        "translation-cache.json", "iop-disc-catalog.json",
        "gs-display.ppm", "gs-webgpu.ppm", "gs-upload.ppm",
    ):
        assert f"'{diagnostic}'" in archive
    assert "Get-Date -Format 'yyyyMMdd-HHmmssfff'" in archive
    assert "sdbz-graphics-pr148-%CAPTURE_PROFILE%-" in archive
    assert "Compress-Archive -LiteralPath $files.FullName" in archive
    assert "-Force" not in archive
    assert "'*.ll'" not in archive and "'*.o'" not in archive
    assert 'set "EXIT_CODE=%ERRORLEVEL%"' in launcher
    assert 'if /I "%REPORT_NAME%"=="first-boot.json" call :archive_full_run' in launcher
    assert 'exit /b %EXIT_CODE%' in launcher
    assert launcher.index('set "EXIT_CODE=%ERRORLEVEL%"') < launcher.index(
        'call :archive_full_run'
    )
    assert launcher.index('if exist "%WORK_DIR%\\%%F" del /q "%WORK_DIR%\\%%F"') < (
        launcher.index('python tools\\run_boot_session.py "%GAME_ELF%"')
    )
    # Strict recovery and normal automatic fallback precede guest execution.
    assert launcher.index(rebuild_text) < (
        launcher.index('python tools\\run_boot_session.py "%GAME_ELF%"')
    )


def test_clean_ctrl_c_cancellation():
    # The real subprocess.run implementation kills its direct child when
    # KeyboardInterrupt propagates out of communicate(). The orchestration
    # layer must convert that interrupt to exit 130, not show a traceback.
    with tempfile.TemporaryDirectory(prefix="ee-boot-interrupt-") as temp:
        work = Path(temp)
        log = work / "interrupt.log"
        def interrupt_with_partial_output(command, **kwargs):
            # Check that in-progress child output is preserved even when
            # subprocess.run raises while waiting for the child.
            assert kwargs["stdout"].write(b"already emitted guest diagnostics\\n")
            kwargs["stdout"].flush()
            raise KeyboardInterrupt

        with patch.object(SESSION.subprocess, "run",
                          side_effect=interrupt_with_partial_output):
            code, output, elapsed = SESSION.run_session(
                ["fake-ee-game", "--auto-expand=2"], log
            )
        assert code == 130
        assert elapsed >= 0.0
        assert "already emitted guest diagnostics" in output
        assert "cancelled by user (Ctrl+C)" in output
        assert "already emitted guest diagnostics" in log.read_text(
            encoding="utf-8")
        assert "cancelled by user (Ctrl+C)" in log.read_text(encoding="utf-8")
        assert "fake-ee-game" in log.read_text(encoding="utf-8")

    for quick_probe in (False, True):
        with tempfile.TemporaryDirectory(prefix="ee-boot-interrupt-") as temp:
            work = Path(temp) / "work"
            work.mkdir()
            report = work / ("quick-probe.json" if quick_probe else "first-boot.json")
            report.write_text('{"stale":true}', encoding="utf-8")
            elf = Path(temp) / "synthetic.elf"
            elf.write_bytes(b"synthetic")
            argv = [
                str(elf), "--work-dir", str(work),
                "--report", str(report), "--expansion-passes=2",
                "--driver", str(ROOT / "tools" / "run_game.py"),
                "--no-iop-catalog",
            ]
            if quick_probe:
                argv.append("--quick-probe")
            cancelled_stderr = io.StringIO()
            with patch.object(SESSION.subprocess, "run",
                              side_effect=KeyboardInterrupt) as subprocess_call:
                with patch("sys.stderr", cancelled_stderr):
                    code = SESSION.main(argv)
            assert code == 130
            assert subprocess_call.call_count == 1, "must not start another guest pass"
            assert not report.exists(), "stale previous guest report must be removed"
            output = (work / ("quick-probe.log" if quick_probe else "boot.log")
                      ).read_text(encoding="utf-8")
            assert "cancelled by user (Ctrl+C)" in output
            assert "cancelled by user (Ctrl+C)" in cancelled_stderr.getvalue()
            if not quick_probe:
                expansion = (work / "expansion.log").read_text(encoding="utf-8")
                assert "exit=130" in expansion

    # Interruption during the final traced replay must not be interpreted
    # as a normal DISPATCH/GPU exit or trigger further guest work.
    with tempfile.TemporaryDirectory(prefix="ee-boot-final-interrupt-") as temp:
        work = Path(temp) / "work"
        work.mkdir()
        report = work / "first-boot.json"
        elf = Path(temp) / "synthetic.elf"
        elf.write_bytes(b"synthetic")
        seen = []

        def interrupted_final(command, **kwargs):
            seen.append(list(map(str, command)))
            if len(seen) == 1:
                report.write_text(json.dumps({
                    "schema": "ee-game-run-report-v1",
                    "dispatches": 500,
                    "exit_reason": 5,
                    "dispatch_status": 0,
                    "final_pc": 0x00109ba0
                }), encoding="utf-8")
                return SimpleNamespace(returncode=3, stdout="expansion complete\\n")
            raise KeyboardInterrupt

        argv = [
            str(elf), "--work-dir", str(work),
            "--report", str(report), "--expansion-passes=1",
            "--driver", str(ROOT / "tools" / "run_game.py"),
            "--no-iop-catalog",
        ]
        cancelled_stderr = io.StringIO()
        with patch.object(SESSION.subprocess, "run", side_effect=interrupted_final):
            with patch("sys.stderr", cancelled_stderr):
                code = SESSION.main(argv)
        assert code == 130 and len(seen) == 2
        assert "--trace" in seen[1]
        assert "expansion complete" in (work / "expansion.log").read_text(
            encoding="utf-8")
        assert "cancelled by user (Ctrl+C)" in (work / "boot.log").read_text(
            encoding="utf-8")
        assert "no guest completion is inferred" in cancelled_stderr.getvalue()
        assert not report.exists(), "interrupted final run must not retain prior state"

    # Ctrl+C during catalog/setup (outside a child) must also exit cleanly.
    cancelled_stderr = io.StringIO()
    with patch.object(SESSION, "main", side_effect=KeyboardInterrupt):
        with patch("sys.stderr", cancelled_stderr):
            assert SESSION.cli_main([]) == 130
    assert "Native boot cancelled by user" in cancelled_stderr.getvalue()


def test_guest_rpc_snapshots_in_cached_probe():
    # A long cached probe must retain read-only guest worker diagnostics
    # without writing one line for every EE dispatch.
    runner = (ROOT / "src" / "tools" /
              "ee_game_runner_main.cpp").read_text(encoding="utf-8")
    assert "if (report_path != nullptr &&\n            dispatched >= next_iop_guest_rpc_snapshot)" in runner
    assert "if (trace && report_path != nullptr &&\n            dispatched >= next_iop_guest_rpc_snapshot)" not in runner
    assert "IOP guest RPC pending registers:" in runner
    assert "IOP guest RPC pending stack:" in runner
    assert "index < 129u" in runner


def main():
    # Independent phase limits, legacy fallback, and quick probe routing.
    for limits in (
        ("--max-expansion-dispatches=2000", "--max-replay-dispatches=4000"),
        ("--max-replay-dispatches=4000",),
        ("--max-expansion-dispatches=2000",),
    ):
        run_case([{"trace": False, "exit_reason": 5},
                  {"trace": True, "exit_reason": 5}],
                 initial_reuse=True, graphics="window", dispatch_limits=limits)
        run_case([{"trace": False, "exit_reason": 5}],
                 initial_reuse=True, quick_probe=True, dispatch_limits=limits)

    code, calls, expansion, boot = run_case([
        {"trace": True, "live": True, "dispatch_status": 1, "code": 4},
    ], initial_reuse=True, graphics="window", expansion_per_pass=7,
       dispatch_limits=("--skip-headless-expansion", "--max-replay-dispatches=4000"))
    assert code == 4 and len(calls) == 1
    assert "--trace" in calls[0] and "--auto-expand=7" in calls[0]
    assert "--skip-headless-expansion" not in calls[0]
    assert "Headless expansion skipped" in expansion
    assert "Executing translated EE code" in boot

    test_clean_ctrl_c_cancellation()
    test_guest_rpc_snapshots_in_cached_probe()
    test_windows_launcher_preserves_guest_cache()
    test_ll_counter()
    for graphics in ("window", "snapshot"):
        code, _, _, _ = run_case([
            {"trace": False, "replay": 0, "dispatch_status": 1, "code": 4},
            {"trace": True, "replay": 1, "dispatch_status": 1, "code": 4},
        ], initial_reuse=True, expansion_per_pass=7, graphics=graphics)
        assert code == 4
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 0, "exit_reason": 0, "dispatch_status": 1,
         "code": 4, "message": "DISPATCH LIMIT"},
    ], initial_reuse=True, initial_function_count=421, quick_probe=True)
    assert code == 4 and len(calls) == 1
    assert "--no-recompile" in calls[0] and "--auto-expand=2" in calls[0]
    assert "new_functions=0" in expansion and "DISPATCH LIMIT" in boot

    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 2, "function_count": 423, "message": "CAP"},
        {"trace": False, "replay": 1, "function_count": 424, "exit_reason": 5,
         "message": "LOOP_BUDGET"},
    ], initial_reuse=True, initial_function_count=421, quick_probe=True)
    assert code == 3 and len(calls) == 2
    assert "new_functions=2" in expansion and "LOOP_BUDGET" in boot

    # The boot-session CLI explicitly forwards the GUI-selected legacy
    # restart policy through both expansion and final replay commands.
    code, calls, _, _ = run_case([
        {"trace": False, "exit_reason": 5},
        {"trace": True, "exit_reason": 5},
    ], initial_reuse=True, legacy=True)
    assert code == 3 and len(calls) == 2

    # First pass consumes its expansion cap; the second finds a real
    # non-DISPATCH blocker and the final run emits the useful trace.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 2, "message": "CAP"},
        {"trace": False, "replay": 1, "exit_reason": 1, "message": "SYSCALL"},
        {"trace": True, "replay": 0, "exit_reason": 1, "message": "TRACE FINAL"},
    ])
    assert code == 3
    assert "CAP" in expansion and "SYSCALL" in expansion, expansion
    assert "TRACE FINAL" not in expansion
    assert "TRACE FINAL" in boot
    assert "--no-recompile" not in calls[0]

    # Regression from the first PS2_GAME boot: 128 functions were added
    # in only 76 replays because jump-table siblings are batched.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 76, "initial_link": 1,
         "function_count": 129},
        {"trace": False, "replay": 4, "function_count": 138,
         "exit_reason": 1, "message": "REAL BLOCKER"},
        {"trace": True, "replay": 0, "exit_reason": 1,
         "message": "FINAL TRACE"},
    ], expansion_per_pass=128)
    assert code == 3 and len(calls) == 3
    assert "new_functions=128" in expansion
    assert "new_functions=9" in expansion
    assert "--no-recompile" in calls[1]
    assert "FINAL TRACE" in boot

    # Live cache-miss sessions never print "(replay N)" and relink after
    # their header. A fresh pass must measure from the initial link, not
    # the last live relink, or the cap is missed and passes are skipped.
    code, calls, expansion, boot = run_case([
        {"trace": False, "live": True, "initial_link": 1,
         "function_count": 129,
         "message": "link_native_objects: PASS (2 functions, fixture)\\n"
                    "link_native_objects: PASS (129 functions, fixture)\\n"
                    "EE hot-expand: expansion budget reached"},
        {"trace": False, "live": True, "function_count": 140,
         "exit_reason": 1, "message": "REAL BLOCKER"},
        {"trace": True, "replay": 0, "exit_reason": 1,
         "message": "FINAL TRACE"},
    ], expansion_per_pass=128)
    assert code == 3 and len(calls) == 3, calls
    assert "new_functions=128" in expansion, expansion
    assert "new_functions=11" in expansion, expansion
    assert "--no-recompile" in calls[1]
    assert "FINAL TRACE" in boot

    # Cached passes measure relative to their existing function count.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 12, "function_count": 257},
        {"trace": False, "replay": 1, "function_count": 258,
         "exit_reason": 1},
        {"trace": True, "replay": 0, "exit_reason": 1},
    ], initial_reuse=True, initial_function_count=129,
       expansion_per_pass=128)
    assert len(calls) == 3 and "new_functions=128" in expansion

    # Never retry unless the actual number of added functions hit cap.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 76, "initial_link": 1,
         "function_count": 100},
        {"trace": True, "replay": 0},
    ], expansion_per_pass=128)
    assert len(calls) == 2 and "new_functions=99" in expansion

    # The SDBZ launcher forwards its diagnostic addresses through this
    # wrapper on both the expansion pass and the final traced replay.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 0, "exit_reason": 5},
        {"trace": True, "replay": 0, "exit_reason": 5},
    ], initial_reuse=True, diagnostics=True)
    assert code == 3
    assert "--watch-word=0x01ffec14" in expansion
    assert "--trace-store=0x01ffec14" in expansion
    assert "--trace-v0=0x00194478:0x00194794" in expansion
    assert "--trace-entry=0x0018cf68" in boot
    assert "--trace-store=0x01ffec14" in boot
    assert "--trace-v0=0x00194478:0x00194794" in boot

    # Final-only GS diagnostics must not leak into 128-function
    # expansion passes, nor preserve previous session's misleading PPMs.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 1, "exit_reason": 5},
        {"trace": True, "replay": 0, "exit_reason": 5},
    ], initial_reuse=True, snapshots=True)
    assert code == 3 and "gs-snapshot" in boot
    assert "gs-snapshot" not in expansion

    # Even if the second pass also hits its cap, stop at the session
    # budget and preserve the translated DLL for next local invocation.
    code, calls, expansion, boot = run_case([
        {"trace": False, "replay": 2},
        {"trace": False, "replay": 2},
        {"trace": True, "replay": 0, "message": "TRACE FINAL"},
    ], initial_reuse=True)
    assert code == 3
    assert len(calls) == 3

    # A failed driver must leave only current diagnostics in either mode.
    for quick_probe in (False, True):
        with tempfile.TemporaryDirectory(prefix="ee-boot-session-failure-") as temp:
            work = Path(temp) / "work"
            work.mkdir()
            report = work / ("quick-probe.json" if quick_probe else "first-boot.json")
            report.write_text(json.dumps({
                "schema": "ee-game-run-report-v1",
                "snapshot_kind": "stopped",
                "dispatches": 100000,
                "final_pc": 0x001BD58C,
            }), encoding="utf-8")
            elf = Path(temp) / "synthetic.elf"
            elf.write_bytes(b"fixture")
            calls = []

            def failing_run(command, **kwargs):
                calls.append(command)
                return SimpleNamespace(returncode=1, stdout="compilation failed\n")

            argv = [
                str(elf), "--work-dir", str(work), "--report", str(report),
                "--expansion-passes=2", "--expansion-per-pass=2",
                "--driver", str(ROOT / "tools" / "run_game.py"), "--no-iop-catalog",
            ]
            if quick_probe:
                argv.extend(("--quick-probe", "--no-recompile"))
            with patch.object(SESSION.subprocess, "run", side_effect=failing_run):
                code = SESSION.main(argv)
            assert code == 1 and len(calls) == 1
            assert not report.exists(), "a failed probe retained an old stop report"
            log = work / ("quick-probe.log" if quick_probe else "boot.log")
            summary = log.read_text()
            assert "compilation failed" in summary
            assert "dispatches=? stop=unknown" in summary

    # Keep the reproducible P40 retail profiling preset identical to the
    # manually validated replay environment, and propagate launcher failure.
    preset = (ROOT / "run_sdbz_p40.bat").read_text(encoding="utf-8")
    assert 'cd /d "%~dp0"' in preset
    assert "git pull --ff-only\nif errorlevel 1 exit /b 1" in preset
    assert preset.index("git pull --ff-only") < preset.index('set "SDBZ_EE_OPT=5"')
    assert [line for line in preset.splitlines() if line.startswith('set "')] == [
        "set \"SDBZ_EE_OPT=5\"",
        "set \"SDBZ_SKIP_HEADLESS_EXPANSION=1\"",
        "set \"SDBZ_SPEED_PERCENT=0\"",
        "set \"MAX_DISPATCHES=40000000\"",
        "set \"MAX_EXPANSION_DISPATCHES=40000000\"",
        "set \"MAX_REPLAY_DISPATCHES=40000000\"",
        "set \"BOOT_TIMEOUT_SECONDS=600\"",
        "set \"SDBZ_RUNTIME_PROFILE=1\"",
        "set \"SDBZ_TRACE_BUFFERED=1\"",
        "set \"SDBZ_REPORT_SNAPSHOT_MS=2000\"",
        "set \"SDBZ_DISPATCH_TRACE_EVERY=1024\"",
        "set \"SDBZ_EE_DIRECT_DISPATCH=1\"",
        "set \"SDBZ_VU1_ASYNC=1\"",
        "set \"SDBZ_GS_ASYNC=1\"",
        "set \"SDBZ_VU1_AOT=%CD%\\build\\vu1-native-v20\"",
        "set \"SDBZ_VU1_AOT_CAPTURE=\"",
        "set \"SDBZ_VU1_AOT_CAPTURE_MISSES=%CD%\\build\\vu1-misses\"",
        "set \"EE_WEBGPU_SNAPSHOT=1\"",
        "set \"EE_WEBGPU_WINDOW=1\"",
    ]
    assert preset.endswith("call run_ps2_game.bat --full --neutral\nexit /b %ERRORLEVEL%\n")

    print("boot session orchestration: PASS")


if __name__ == "__main__":
    main()
