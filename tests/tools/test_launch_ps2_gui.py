#!/usr/bin/env python3
"""Headless regression tests for live cache-miss controls in the Windows GUI.

No Tk window is created, so this also runs on Linux CI.
"""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import ModuleType, SimpleNamespace


# CI uses a minimal Python installation without the optional Tk package.
# Only the pure argument/settings logic is exercised: no GUI is created.
# Provide import-time stubs when Tk is absent, leaving a real installed
# tkinter module completely untouched on Windows developer machines.
try:
    import tkinter
except ImportError:
    tkinter = ModuleType("tkinter")
    tkinter.__path__ = []
    for module_name in ("ttk", "filedialog", "messagebox"):
        module = ModuleType("tkinter." + module_name)
        setattr(tkinter, module_name, module)
        sys.modules["tkinter." + module_name] = module
    tkinter.ttk.Frame = type("Frame", (), {})
    sys.modules["tkinter"] = tkinter


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "launch_ps2_gui", ROOT / "tools" / "launch_ps2_gui.py"
)
GUI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GUI)

from ps2_direct_options import TOOL_OPTIONS, tool_arguments


class c_fake_variable:
    def __init__(self, value):
        self.m_value = value

    def get(self):
        return self.m_value


def gui_values(settings):
    return {
        label: settings["numeric"][key]
        for label, (key, _, _) in GUI.NUMERIC_SETTINGS.items()
    }


def test_defaults_and_compatibility():
    defaults = GUI.default_settings()
    assert defaults["cache_miss_resume"] is True
    assert defaults["precompile_all"] is True
    assert defaults["fast_overlays"] is True
    assert defaults["numeric"]["EMIT_JOBS"] == ""
    assert defaults["diagnostics"] == {label: "" for label in GUI.DIAGNOSTIC_SETTINGS}
    assert defaults["embed_game_window"] is True
    assert defaults["display_aspect"] == "4:3 (PS2)"
    assert defaults["deinterlace"] == "Automatic"
    assert list(GUI.DEINTERLACE_MODES.values()) == [str(i) for i in range(10)]
    assert GUI.NUMERIC_SETTINGS["Functions per expansion"][2] == 1024
    with tempfile.TemporaryDirectory() as temporary:
        settings_path = Path(temporary) / "launcher-settings.json"
        # Old GUI settings do not contain the new key. Migration must
        # default to live state-preserving resume rather than legacy replay.
        old = {key: value for key, value in defaults.items()
               if key not in ("cache_miss_resume", "precompile_all", "fast_overlays")}
        settings_path.write_text(json.dumps(old), encoding="utf-8")
        restored, warning = GUI.load_settings(settings_path)
        assert not warning and restored["cache_miss_resume"] is True
        assert restored["precompile_all"] is True
        assert restored["fast_overlays"] is True
        assert all(not value for value in restored["diagnostics"].values())
        restored["diagnostics"]["Trace guest stores"] = "0x0044081c"
        restored["cache_miss_resume"] = False
        restored["precompile_all"] = False
        restored["fast_overlays"] = False
        restored["numeric"]["EXPANSION_PER_PASS"] = "1024"
        GUI.save_settings(settings_path, restored)
        again, warning = GUI.load_settings(settings_path)
        assert not warning and again["cache_miss_resume"] is False
        assert again["precompile_all"] is False
        assert again["fast_overlays"] is False
        assert again["diagnostics"]["Trace guest stores"] == "0x0044081c"
        assert again["numeric"]["EXPANSION_PER_PASS"] == "1024"


def test_launcher_flags_and_bounds():
    values = gui_values(GUI.default_settings())
    for mode in ("Full replay", "Automatic", "Quick probe"):
        args, env = GUI.launcher_options(
            mode, "Neutral", False, True, True, values
        )
        assert "--replay-after-expand" not in args
        assert "--live-full-relink" not in args
        assert env["PRECOMPILE_ALL"] == "1"
        assert env["EXPANSION_PER_PASS"] == "128"
        args, env = GUI.launcher_options(
            mode, "Neutral", False, True, True, values,
            resume_on_cache_miss=False
        )
        assert args.count("--replay-after-expand") == 1
    for mode in ("Matched neutral/Cross", "Recover translation cache"):
        args, env = GUI.launcher_options(
            mode, "Neutral", False, False, False, values,
            resume_on_cache_miss=False,
            precompile_all=False, fast_overlays=False,
        )
        assert "--replay-after-expand" not in args
        assert "--live-full-relink" not in args
        assert "PRECOMPILE_ALL" not in env

    for mode in ("Full replay", "Automatic", "Quick probe"):
        args, env = GUI.launcher_options(
            mode, "Neutral", False, False, False, values,
            precompile_all=False, fast_overlays=False,
        )
        assert "--live-full-relink" in args
        assert "--replay-after-expand" not in args
        assert env["PRECOMPILE_ALL"] == "0"
        args, _ = GUI.launcher_options(
            mode, "Neutral", False, False, False, values,
            resume_on_cache_miss=False, fast_overlays=False,
        )
        assert "--live-full-relink" not in args
        assert "--replay-after-expand" in args

    values["Functions per expansion"] = "1024"
    args, env = GUI.launcher_options(
        "Full replay", "Neutral", False, True, True, values,
        resume_on_cache_miss=False
    )
    assert env["EXPANSION_PER_PASS"] == "1024"
    values["Functions per expansion"] = "1025"
    try:
        GUI.launcher_options("Full replay", "Neutral", False, True, True,
                             values)
    except ValueError as error:
        assert "Functions per expansion must be 1–1024" in str(error)
    else:
        raise AssertionError("accepted an expansion count above 1024")


def test_diagnostic_validation():
    values = gui_values(GUI.default_settings())
    target = {
        "Watch guest word": "0x0044081c",
        "Trace guest stores": "0x0044081C",
        "Trace V0 writes": "0x00137970:0x00137d18",
    }
    args, env = GUI.launcher_options(
        "Full replay", "Neutral", False, True, True, values,
        diagnostics=target,
    )
    assert "--full" in args
    assert env["SDBZ_WATCH_WORD"] == "0x0044081c"
    assert env["SDBZ_TRACE_STORE"] == "0x0044081c"
    assert env["SDBZ_TRACE_V0"] == "0x00137970:0x00137d18"
    assert not any("TRACE" in key or "WATCH" in key
                   for key in GUI.launcher_options(
                       "Recover translation cache", "Neutral", False,
                       False, False, values, diagnostics=target)[1])
    for invalid in ("5", "0x0044081d", "-1", "0x100000000", "unknown"):
        try:
            GUI.diagnostic_overrides({"Trace guest stores": invalid})
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted invalid store watch: {invalid}")
    for invalid in ("0x1:0x2", "0x4000:0x3000", "0x1000:0x4000", "abc"):
        try:
            GUI.diagnostic_overrides({"Trace V0 writes": invalid})
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted invalid V0 range: {invalid}")


def test_game_tab_command_and_environment():
    with tempfile.TemporaryDirectory() as temporary:
        disc = Path(temporary)
        (disc / "SYSTEM.CNF").write_text(
            r"BOOT2 = cdrom0:\\SLUS_214.42;1" + "\n", encoding="ascii"
        )
        vars = {
            "disc": c_fake_variable(str(disc)),
            "mode": c_fake_variable("Full replay"),
            "pad": c_fake_variable("Neutral"),
            "accept_ir": c_fake_variable(False),
            "window": c_fake_variable(True),
            "snapshot": c_fake_variable(True),
            "speed": c_fake_variable("150%"),
            "cache_miss_resume": c_fake_variable(False),
            "precompile_all": c_fake_variable(True),
            "fast_overlays": c_fake_variable(True),
            "diagnostics": {
                label: c_fake_variable("")
                for label in GUI.DIAGNOSTIC_SETTINGS
            },
            "numeric": {label: c_fake_variable(value)
                        for label, value in gui_values(GUI.default_settings()).items()},
        }
        vars["fields"] = {key: c_fake_variable(default) for key, (_, default, _) in GUI.RUNTIME_SETTINGS.items()}
        vars["fields"]["display_aspect"] = c_fake_variable("4:3 (PS2)")
        vars["fields"]["deinterlace"] = c_fake_variable("Automatic")
        instance = SimpleNamespace(**vars)
        env = {"EXPANSION_PER_PASS": "99", "SDBZ_DISC_ROOT": "stale", "SDBZ_PAD0_SCRIPT": "1:1:cross", "SDBZ_TRACE_IPU": "1", "SDBZ_IOP_AOT_VERIFY": "1"}
        command = GUI.Launcher.game_command(instance, env)
        assert command[:4] == [
            "cmd.exe", "/d", "/c", "run_ps2_game.bat"
        ], command
        assert "--full" in command and "--replay-after-expand" in command
        assert env["EXPANSION_PER_PASS"] == "128"
        assert env["SDBZ_DISC_ROOT"] == str(disc.resolve())
        assert env["SDBZ_SPEED_PERCENT"] == "150"
        assert env["EE_WEBGPU_ASPECT"] == "4:3"
        assert env["EE_WEBGPU_DEINTERLACE"] == "0"
        for name, value in GUI.DEINTERLACE_MODES.items():
            instance.fields["deinterlace"] = c_fake_variable(name)
            GUI.Launcher.game_command(instance, env)
            assert env["EE_WEBGPU_DEINTERLACE"] == value
        instance.fields["display_aspect"] = c_fake_variable("Native pixels")
        GUI.Launcher.game_command(instance, env)
        assert env["EE_WEBGPU_ASPECT"] == "native"
        assert env["SDBZ_IDLE_SKIP"] == "1"
        assert not any(key in env for key in ("SDBZ_PAD0_SCRIPT", "SDBZ_TRACE_IPU", "SDBZ_IOP_AOT_VERIFY"))
        assert env["PRECOMPILE_ALL"] == "1"
        assert "--live-full-relink" not in command
        assert GUI.default_settings()["speed"] == "100%"

        instance.cache_miss_resume = c_fake_variable(True)
        instance.fast_overlays = c_fake_variable(False)
        instance.precompile_all = c_fake_variable(False)
        command = GUI.Launcher.game_command(instance, env)
        assert "--replay-after-expand" not in command
        assert "--live-full-relink" in command
        assert env["PRECOMPILE_ALL"] == "0"
        instance.fast_overlays = c_fake_variable(True)
        command = GUI.Launcher.game_command(instance, env)
        assert "--live-full-relink" not in command
        instance.diagnostics["Trace guest stores"] = c_fake_variable("0x0044081c")
        command = GUI.Launcher.game_command(instance, env)
        assert "--live-full-relink" not in command
        assert env["SDBZ_TRACE_STORE"] == "0x0044081c"
        instance.diagnostics["Trace guest stores"] = c_fake_variable("")
        GUI.Launcher.game_command(instance, env)
        assert "SDBZ_TRACE_STORE" not in env


def test_runtime_options():
    defaults = GUI.default_settings()
    assert defaults["idle_skip"] is True
    assert defaults["pad_script"] == "200:8:cross"
    assert defaults["trace_iop_sema"] == "128"
    assert defaults["trace_rpc_args"] == "0x80000593"
    for key in GUI.RUNTIME_GATED_FIELDS:
        assert defaults[key] and defaults[f"{key}_enabled"] is False
    assert GUI.runtime_overrides("Full replay", "Neutral", True, True, {}) == {"SDBZ_IDLE_SKIP": "1"}
    assert GUI.runtime_overrides("Full replay", "Scripted", True, True, {})["SDBZ_PAD0_SCRIPT"] == "200:8:cross"
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary)
        settings_path = path / "settings.json"
        settings_path.write_text(json.dumps({"pad": "Neutral", "iop_aot": str(path), "iop_capture": "", "pad_script": ""}), encoding="utf-8")
        restored, warning = GUI.load_settings(settings_path)
        assert not warning and restored["idle_skip"] is True
        assert restored["iop_aot_enabled"] is True and restored["iop_aot"] == str(path)
        assert restored["iop_capture_enabled"] is False and restored["iop_capture"] == defaults["iop_capture"]
        assert restored["pad_script"] == defaults["pad_script"]
        for key in GUI.RUNTIME_GATED_FIELDS:
            restored[f"{key}_enabled"] = True
        restored["pad"] = "Scripted"
        restored["pad_script"] = "740:8:cross;3500:4:start+circle"
        restored["idle_skip"] = False
        library = path / ("test.dll" if sys.platform == "win32" else "test.so")
        library.touch()
        restored["iop_aot"] = str(path)
        restored["iop_capture"] = str(path / "capture")
        restored["iop_verify"] = True
        restored["trace_ipu"] = True
        restored["trace_cdvd"] = True
        restored["trace_rpc_args"] = "0x80000592"
        restored["trace_iop_sema"] = "0"
        restored["frame_dir"] = str(path / "frames")
        restored["frame_interval"] = "30"
        restored["dump_ram"] = str(path / "ram.bin")
        GUI.save_settings(settings_path, restored)
        again, warning = GUI.load_settings(settings_path)
        assert not warning and again["pad_script"] == restored["pad_script"]
        assert again["idle_skip"] is False and again["iop_verify"] is True
        values = {key: again[key] for key in GUI.RUNTIME_SETTINGS}
        args, env = GUI.launcher_options(
            "Full replay", "Scripted", False, True, True, gui_values(again), runtime=values)
        assert "--neutral" not in args and "--cross" not in args
        assert env["SDBZ_PAD0_SCRIPT"] == "740:8:cross,3500:4:start+circle"
        assert env["SDBZ_IDLE_SKIP"] == "0"
        assert env["SDBZ_IOP_AOT"] == str(path.resolve())
        assert env["SDBZ_IOP_AOT_CAPTURE"] == str(path.resolve() / "capture")
        assert env["SDBZ_IOP_AOT_VERIFY"] == "1"
        assert env["SDBZ_TRACE_IPU"] == env["SDBZ_TRACE_CDVD"] == "1"
        assert env["SDBZ_TRACE_RPC_ARGS"] == str(0x80000592)
        assert env["SDBZ_TRACE_IOP_SEMA"] == "0"
        assert env["SDBZ_FRAME_DUMP"] == f"{path.resolve() / 'frames'}:30"
        assert env["SDBZ_DUMP_RAM"] == str(path.resolve() / "ram.bin")
        assert not (path / "frames").exists() and not (path / "capture").exists()
        GUI.prepare_runtime_outputs(env)
        assert (path / "frames").is_dir() and (path / "capture").is_dir()
        again["iop_aot_enabled"] = False
        GUI.save_settings(settings_path, again)
        disabled, warning = GUI.load_settings(settings_path)
        assert not warning and not disabled["iop_aot_enabled"] and disabled["iop_aot"] == str(path)
        matched = GUI.runtime_overrides("Matched neutral/Cross", "Scripted", True, True, values)
        assert not any(key in matched for key in ("SDBZ_PAD0_SCRIPT", "SDBZ_IOP_AOT_CAPTURE", "SDBZ_FRAME_DUMP", "SDBZ_DUMP_RAM"))
        assert GUI.runtime_overrides("Recover translation cache", "Scripted", True, True, values) == {}
        neutral = GUI.runtime_overrides("Full replay", "Neutral", False, False, values)
        assert "SDBZ_PAD0_SCRIPT" not in neutral and "SDBZ_FRAME_DUMP" not in neutral

        # A clean installation has no module directory yet. Both enabled
        # controls must launch capture, not reject the missing library.
        modules = path / "iop-aot" / "modules"
        bootstrap = {
            "iop_aot_enabled": True, "iop_aot": str(modules),
            "iop_capture_enabled": True, "iop_capture": str(path / "capture"),
        }
        first = GUI.runtime_overrides("Full replay", "Neutral", True, True, bootstrap)
        assert first["SDBZ_IOP_AOT_BUILD_OUT"] == str(modules.parent.resolve())
        assert "SDBZ_IOP_AOT" not in first
        assert "SDBZ_IOP_AOT_CAPTURE" in first
        modules.mkdir(parents=True)
        empty = GUI.runtime_overrides("Full replay", "Neutral", True, True, bootstrap)
        assert empty == first
        (modules / library.name).touch()
        second = GUI.runtime_overrides("Full replay", "Neutral", True, True, bootstrap)
        assert second["SDBZ_IOP_AOT"] == str(modules.resolve())
        assert "SDBZ_IOP_AOT_BUILD_OUT" not in second
        try:
            GUI.runtime_overrides("Full replay", "Neutral", True, True,
                                  {**bootstrap, "iop_capture_enabled": False,
                                   "iop_aot": str(path / "missing.dll")})
        except ValueError:
            pass
        else:
            raise AssertionError("accepted a missing load-only library")
    for raw in ("", "0:1:cross", "1:601:cross", "1:1:bad", "1:1:cross+", "1234567890:1:cross"):
        try:
            GUI.validate_pad_script(raw)
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted invalid pad script: {raw}")
    try:
        GUI.runtime_overrides("Full replay", "Neutral", True, True, {"iop_verify": True})
    except ValueError:
        pass
    else:
        raise AssertionError("accepted AOT verification without a library")


def test_live_deinterlacing():
    from unittest.mock import Mock
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "session.mode"
        other = Path(temporary) / "other.mode"
        other.write_bytes(b"7")
        variable = c_fake_variable("Automatic")
        session = SimpleNamespace(deinterlace_file=path, fields={"deinterlace": variable},
                                  status=Mock())
        for name, value in GUI.DEINTERLACE_MODES.items():
            variable.m_value = name
            GUI.Launcher.change_deinterlace(session)
            assert path.read_bytes() == value.encode("ascii")
            assert other.read_bytes() == b"7"
        variable.m_value = "Off"
        GUI.Launcher.change_deinterlace(session)
        assert path.read_bytes() == b"1"
        GUI.Launcher.clear_deinterlace_file(session)
        assert session.deinterlace_file is None and not path.exists()
        GUI.Launcher.change_deinterlace(session)
        assert not path.exists() and other.exists()


def test_live_display_aspect():
    from unittest.mock import Mock
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "session.aspect"
        other = Path(temporary) / "session.mode"
        other.write_bytes(b"7")
        variable = c_fake_variable("4:3 (PS2)")
        session = SimpleNamespace(aspect_file=path, fields={"display_aspect": variable},
                                  status=Mock())
        for name, value in GUI.DISPLAY_ASPECTS.items():
            variable.m_value = name
            GUI.Launcher.change_display_aspect(session)
            assert path.read_bytes() == value.encode("ascii")
            assert other.read_bytes() == b"7"
        GUI.Launcher.clear_aspect_file(session)
        assert session.aspect_file is None and not path.exists()
        GUI.Launcher.change_display_aspect(session)
        assert not path.exists() and other.exists()


def test_launch_close_button():
    from unittest.mock import Mock, patch

    launch = Mock()
    close_session = Mock()
    instance = SimpleNamespace(process=None, launch=launch, close_session=close_session)
    GUI.Launcher.toggle_launch(instance)
    launch.assert_called_once_with()
    close_session.assert_not_called()
    instance.process = Mock()
    GUI.Launcher.toggle_launch(instance)
    close_session.assert_called_once_with()

    process = Mock()
    process.pid = 1234
    process.poll.return_value = None
    killer = Mock()
    with patch.object(GUI.subprocess, "Popen", return_value=killer) as popen, \
            patch.object(GUI.subprocess, "CREATE_NO_WINDOW", 0x08000000, create=True):
        assert GUI.stop_process_tree(process) is killer
        assert popen.call_args.args[0] == ["taskkill.exe", "/PID", "1234", "/T", "/F"]
    process.poll.return_value = 0
    assert GUI.stop_process_tree(process) is None
    process.poll.return_value = None

    session = SimpleNamespace(
        process=process, stop_process=None, close_requested=False,
        embedded_running=True, game_host=Mock(), play=Mock(), status=Mock(),
    )
    session.game_host.winfo_id.return_value = 99
    with patch.object(GUI, "request_embedded_close", return_value=True) as graceful, \
            patch.object(GUI, "stop_process_tree", return_value=killer) as force:
        GUI.Launcher.close_session(session)
        graceful.assert_called_once_with(99)
        force.assert_not_called()
        assert session.close_requested and session.stop_process is None
        session.play.configure.assert_called_with(text="Force close")
        GUI.Launcher.close_session(session)
        force.assert_called_once_with(process)
        assert session.stop_process is killer
        session.play.configure.assert_called_with(text="Closing…")
        GUI.Launcher.close_session(session)
        force.assert_called_once_with(process)

    session.stop_process = None
    session.close_requested = False
    session.embedded_running = False
    with patch.object(GUI, "stop_process_tree", return_value=killer) as force:
        GUI.Launcher.close_session(session)
        force.assert_called_once_with(process)
        assert session.stop_process is killer


def test_game_window_environment():
    env = {"SDBZ_GS_PARENT_HWND": "123", "UNRELATED": "retained"}
    GUI.configure_game_window_environment(env, 987654)
    assert env == {
        "SDBZ_GS_PARENT_HWND": "987654", "UNRELATED": "retained"
    }
    GUI.configure_game_window_environment(env)
    assert env == {"UNRELATED": "retained"}
    for invalid in (0, -5, True, "bogus"):
        try:
            GUI.configure_game_window_environment(env, invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted invalid parent HWND {invalid!r}")


def test_direct_commands():
    direct = GUI.batch_tool_defaults("/nonexistent/PS2_GAME")
    for tool, defaults in direct.items():
        for flag in ("--trace-instructions", "--trace-instructions-window", "--trace-entry",
                     "--watch-word", "--trace-v0", "--trace-store"):
            assert defaults[flag] == "", (tool, flag)
    assert direct["Game runner (Python)"]["--trace"] is True
    assert "--trace" not in direct["Boot session"]
    assert direct["Boot session"]["--replay-after-expand"] is False
    assert direct["Game runner (Python)"]["--replay-after-expand"] is False
    assert direct["Boot session"]["--live-full-relink"] is False
    assert direct["Game runner (Python)"]["--live-full-relink"] is False
    assert direct["Boot session"]["--functions"] == "all"
    assert direct["Game runner (Python)"]["--functions"] == "all"
    assert 1 <= int(direct["Boot session"]["--emit-jobs"]) <= 32
    assert direct["Game runner (Python)"]["--auto-expand"] == "128"
    for tool in ("Boot session", "Game runner (Python)"):
        choices = [opt for options in TOOL_OPTIONS[tool].values()
                   for opt in options]
        names = [opt.name for opt in choices]
        assert names.count("--replay-after-expand") == 1
        assert names.count("--live-full-relink") == 1
        values = {**direct[tool], "--replay-after-expand": True}
        # The required source ELF is supplied by the UI's user-selected
        # disc file. We only test token generation, not loading the game.
        values["elf"] = "/tmp/dummy.elf"
        args = tool_arguments(tool, values, "")
        assert args.count("--replay-after-expand") == 1
        values["--replay-after-expand"] = False
        assert "--replay-after-expand" not in tool_arguments(tool, values, "")
        values["--live-full-relink"] = True
        assert tool_arguments(tool, values, "").count("--live-full-relink") == 1
        values["--live-full-relink"] = False
        assert "--live-full-relink" not in tool_arguments(tool, values, "")


def test_batch_route():
    batch = (ROOT / "run_ps2_game.bat").read_text(encoding="utf-8")
    assert '--watch-word=%SDBZ_WATCH_WORD%' in batch
    assert '--trace-store=%SDBZ_TRACE_STORE%' in batch
    assert '--trace-v0=%SDBZ_TRACE_V0%' in batch
    assert '%GUEST_TRACE_ARGS%' in batch
    assert 'if defined SDBZ_PAD0_SCRIPT set "CAPTURE_PROFILE=pad-script"' in batch
    assert '--target iop_recomp' in batch
    assert 'if /I "%~1"=="--replay-after-expand" (' in batch
    assert 'set "CACHE_MISS_ARGS=--replay-after-expand"' in batch
    assert '%SESSION_ARGS% %CACHE_MISS_ARGS% %LIVE_RELINK_ARG%' in batch
    assert 'if /I "%~1"=="--live-full-relink" (' in batch
    assert 'set "LIVE_RELINK_ARG=--live-full-relink"' in batch
    assert 'if not defined CACHE_FLAG if not "%PRECOMPILE_ALL%"=="0"' in batch
    assert 'if not defined EMIT_JOBS set "EMIT_JOBS=%NUMBER_OF_PROCESSORS%"' in batch
    # Matched and recovery modes have no auto-expansion to resume.
    assert 'ERROR: --replay-after-expand has no meaning during cache recovery.' in batch
    assert 'ERROR: --replay-after-expand has no meaning during matched replays.' in batch
    assert 'ERROR: --live-full-relink has no meaning during cache recovery.' in batch
    assert 'ERROR: --live-full-relink has no meaning during matched replays.' in batch
    assert 'ERROR: --live-full-relink requires live cache-miss resume.' in batch
    # Boot-session argv is forwarded to the game translation driver,
    # which owns the implementation of --replay-after-expand.
    session = (ROOT / "tools" / "run_boot_session.py").read_text(
        encoding="utf-8"
    )
    assert 'f"--report={report}", *forwarded,' in session
    assert '*(["--replay-after-expand"] if args.replay_after_expand else []),' in session
    assert '*forwarded,' in session
    driver = (ROOT / "tools" / "run_game.py").read_text(encoding="utf-8")
    assert 'parser.add_argument("--live-full-relink", action="store_true"' in driver
    assert 'use_overlays = not args.live_full_relink' in driver


def main():
    test_defaults_and_compatibility()
    test_launcher_flags_and_bounds()
    test_game_tab_command_and_environment()
    test_runtime_options()
    test_live_deinterlacing()
    test_live_display_aspect()
    test_launch_close_button()
    test_game_window_environment()
    test_direct_commands()
    test_batch_route()
    print("launcher live cache-miss controls: PASS")


if __name__ == "__main__":
    main()
