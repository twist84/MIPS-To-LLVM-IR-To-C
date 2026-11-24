"""Configure and run the Windows native PS2 launcher."""

import json
import os
from pathlib import Path
import subprocess
import sys
from tempfile import NamedTemporaryFile
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from ps2_direct_options import TOOL_OPTIONS, tool_arguments
from resolve_ps2_boot import resolve_boot_elf

ROOT = Path(__file__).resolve().parents[1]
LOG_PATH = ROOT / "build" / "ps2-launcher.log"
SETTINGS_PATH = ROOT / "build" / "ps2-launcher-settings.json"


SPEEDS = {"50%": 50, "100%": 100, "150%": 150, "200%": 200, "Unlimited": 0}
DISPLAY_ASPECTS = {"4:3 (PS2)": "4:3", "16:9": "16:9", "Native pixels": "native", "1:1": "1:1"}
DEINTERLACE_MODES = {
    "Automatic": "0", "Off": "1",
    "Weave (TFF)": "2", "Weave (BFF)": "3",
    "Bob (TFF)": "4", "Bob (BFF)": "5",
    "Blend (TFF)": "6", "Blend (BFF)": "7",
    "Adaptive (TFF)": "8", "Adaptive (BFF)": "9",
}

SESSION_MODES = {
    "Full replay": "--full",
    "Automatic": None,
    "Quick probe": "--quick",
    "Matched neutral/Cross": "--matched",
    "Recover translation cache": "--recover-cache",
}
PAD_PROFILES = {"Neutral": "--neutral", "Cross replay": "--cross", "Custom": None, "Scripted": None}
NUMERIC_SETTINGS = {
    "Cross first poll": ("SDBZ_PAD0_CROSS_FIRST_POLL", 1, 0xFFFFFFFF),
    "Cross poll count": ("SDBZ_PAD0_CROSS_POLL_COUNT", 1, 120),
    "Max dispatches": ("MAX_DISPATCHES", 1, None),
    "Timeout (seconds)": ("BOOT_TIMEOUT_SECONDS", 1, None),
    "Expansion passes": ("EXPANSION_PASSES", 1, 64),
    "Functions per expansion": ("EXPANSION_PER_PASS", 1, 1024),
    "Compile jobs": ("EMIT_JOBS", 1, None),
    "SIF poll limit": ("SIF_POLL_LIMIT", 1, 0xFFFFFFFF),
    "Loop watchdog resumes": ("LOOP_YIELD_RESUMES", 0, 0xFFFFFFFF),
    "Trace every N dispatches": ("SDBZ_DISPATCH_TRACE_EVERY", 1, 0xFFFFFFFF),
    "Report snapshot interval (ms)": ("SDBZ_REPORT_SNAPSHOT_MS", 0, 60000),
}
COMMAND_TOOLS = ("Boot session", "Game runner (Python)", "Native runner")

DIAGNOSTIC_SETTINGS = {
    "Watch guest word": ("SDBZ_WATCH_WORD", "word"),
    "Trace guest stores": ("SDBZ_TRACE_STORE", "word"),
    "Trace V0 writes": ("SDBZ_TRACE_V0", "range"),
}

 
RUNTIME_SETTINGS = {
    "idle_skip": ("SDBZ_IDLE_SKIP", True, "Accelerate idle polling loops"),
    "pad_script": ("SDBZ_PAD0_SCRIPT", "200:8:cross", "Pad script"),
    "iop_aot": ("SDBZ_IOP_AOT", str(ROOT / "build" / "iop-aot" / "modules"), "IOP AOT library or directory"),
    "iop_capture": ("SDBZ_IOP_AOT_CAPTURE", str(ROOT / "build" / "iop-capture"), "Capture linked IRX modules"),
    "iop_verify": ("SDBZ_IOP_AOT_VERIFY", False, "Verify IOP AOT (diagnostic only)"),
    "trace_ipu": ("SDBZ_TRACE_IPU", False, "Trace IPU"),
    "trace_cdvd": ("SDBZ_TRACE_CDVD", False, "Trace CDVD"),
    "trace_rpc_reject": ("SDBZ_TRACE_RPC_REJECT", False, "Trace rejected RPC commands"),
    "trace_iop_threads": ("SDBZ_TRACE_IOP_THREADS", False, "Dump IOP threads at exit"),
    "trace_iop_sema": ("SDBZ_TRACE_IOP_SEMA", "128", "IOP semaphore trace limit"),
    "trace_rpc_args": ("SDBZ_TRACE_RPC_ARGS", "0x80000593", "Trace RPC server ID"),
    "frame_dir": ("SDBZ_FRAME_DUMP", str(ROOT / "build" / "ps2-game-hle" / "frames"), "Frame dump directory"),
    "frame_interval": (None, "60", "Frame dump interval (fields)"),
    "dump_ram": ("SDBZ_DUMP_RAM", str(ROOT / "build" / "ps2-game-hle" / "ee-ram.bin"), "EE RAM dump file"),
}

# A populated value is not an opt-in. These switches are persisted separately
# from the parameters so a fresh install never starts tracing or writing files.
RUNTIME_GATED_FIELDS = (
    "iop_aot", "iop_capture", "trace_iop_sema", "trace_rpc_args",
    "frame_dir", "dump_ram",
)
RUNTIME_SETTINGS.update({
    f"{key}_enabled": (None, False, RUNTIME_SETTINGS[key][2])
    for key in RUNTIME_GATED_FIELDS
})

PAD_BUTTONS = frozenset(
    "select l3 r3 start up right down left l2 r2 l1 r1 triangle circle cross square".split()
)


def validate_pad_script(raw):
    steps = [step for step in raw.replace(";", ",").split(",") if step]
    if not 1 <= len(steps) <= 256:
        raise ValueError("Pad script requires 1–256 steps.")
    for step in steps:
        parts = step.split(":")
        if len(parts) != 3:
            raise ValueError("Pad script: expected FIRST:HOLD:BUTTON[+BUTTON].")
        first, hold, buttons = parts
        if (not first.isascii() or not first.isdecimal() or len(first) > 9 or
                not hold.isascii() or not hold.isdecimal() or len(hold) > 9 or
                int(first) < 1 or not 1 <= int(hold) <= 600 or
                any(button.lower() not in PAD_BUTTONS for button in buttons.split("+"))):
            raise ValueError(f"Invalid pad script step: {step}")
    return ",".join(steps)


def runtime_overrides(mode, pad, window, snapshot, values):
    """Translate persisted runtime controls into explicit, per-launch environment."""
    if mode == "Recover translation cache":
        return {}
    values = {**{key: default for key, (_, default, _) in RUNTIME_SETTINGS.items()}, **values}
    env = {"SDBZ_IDLE_SKIP": str(int(values["idle_skip"]))}
    if mode != "Matched neutral/Cross" and pad == "Scripted":
        env["SDBZ_PAD0_SCRIPT"] = validate_pad_script(values["pad_script"].strip())
    for key in ("trace_ipu", "trace_cdvd", "trace_rpc_reject", "trace_iop_threads"):
        if values[key]:
            env[RUNTIME_SETTINGS[key][0]] = "1"
    for key, minimum, maximum in (("trace_iop_sema", 0, 0xffffffff),
                                   ("trace_rpc_args", 0, 0xffffffff)):
        if not values[f"{key}_enabled"]:
            continue
        raw = values[key].strip()
        if not raw:
            raise ValueError(f"Enter a value for {RUNTIME_SETTINGS[key][2]}.")
        if raw:
            try:
                number = int(raw, 16 if raw.lower().startswith("0x") else 10)
                if not minimum <= number <= maximum:
                    raise ValueError
            except ValueError:
                raise ValueError(f"{RUNTIME_SETTINGS[key][2]} must be a 32-bit unsigned integer.") from None
            env[RUNTIME_SETTINGS[key][0]] = str(number)
    aot = values["iop_aot"].strip() if values["iop_aot_enabled"] else ""
    if values["iop_aot_enabled"]:
        if not aot:
            raise ValueError("Choose an IOP AOT library or directory.")
        path = Path(aot).expanduser().resolve()
        suffix = ".dll" if os.name == "nt" else ".so"
        capture_enabled = values["iop_capture_enabled"] and mode != "Matched neutral/Cross"
        # Both boxes enabled is the normal first-run workflow. No DLL can
        # exist until the game has captured and linked its IRX modules.
        has_libraries = path.is_file() or (
            path.is_dir() and any(p.is_file() for p in path.glob(f"*{suffix}"))
        )
        if has_libraries:
            env["SDBZ_IOP_AOT"] = str(path)
        elif capture_enabled and path.name == "modules" and (not path.exists() or path.is_dir()):
            if values["iop_verify"]:
                raise ValueError("Disable IOP AOT verification until the first capture has been compiled.")
            env["SDBZ_IOP_AOT_BUILD_OUT"] = str(path.parent)
        else:
            raise ValueError("No IOP AOT libraries found. Enable IRX capture and use an OUT/modules directory for the first build.")
    if values["iop_verify"]:
        if "SDBZ_IOP_AOT" not in env:
            raise ValueError("IOP AOT verification requires an existing AOT library.")
        env["SDBZ_IOP_AOT_VERIFY"] = "1"
    # Output-producing probes are excluded from matched runs to avoid one
    # replay overwriting the other. Capture is supported in ordinary sessions.
    if mode != "Matched neutral/Cross":
        for key in ("iop_capture", "dump_ram"):
            if not values[f"{key}_enabled"]:
                continue
            raw = values[key].strip()
            if not raw:
                raise ValueError(f"Choose a destination for {RUNTIME_SETTINGS[key][2]}.")
            if raw:
                path = Path(raw).expanduser().resolve()
                if key == "iop_capture":
                    if path.exists() and not path.is_dir():
                        raise ValueError("IRX capture destination must be a directory.")
                elif path.is_dir():
                    raise ValueError("RAM dump requires a file, not a directory.")
                env[RUNTIME_SETTINGS[key][0]] = str(path)
        raw = values["frame_dir"].strip()
        if values["frame_dir_enabled"] and mode in ("Full replay", "Automatic") and (window or snapshot):
            if not raw:
                raise ValueError("Choose a frame dump directory.")
            path = Path(raw).expanduser().resolve()
            if path.exists() and not path.is_dir():
                raise ValueError("Frame dump destination must be a directory.")
            interval = values["frame_interval"].strip()
            if not interval.isascii() or not interval.isdecimal() or not 1 <= int(interval) <= 0xffffffff:
                raise ValueError("Frame dump interval must be 1–4294967295 fields.")
            env["SDBZ_FRAME_DUMP"] = f"{path}:{int(interval)}"
    return env


def prepare_runtime_outputs(env):
    """Create only the directories requested by enabled output probes."""
    for key in ("SDBZ_IOP_AOT_CAPTURE", "SDBZ_FRAME_DUMP", "SDBZ_DUMP_RAM"):
        if key not in env:
            continue
        path = env[key]
        if key == "SDBZ_FRAME_DUMP":
            path = path.rsplit(":", 1)[0]
        directory = Path(path)
        if key == "SDBZ_DUMP_RAM":
            directory = directory.parent
        directory.mkdir(parents=True, exist_ok=True)


def diagnostic_overrides(values):
    """Validate optional read-only native guest diagnostics."""
    result = {}
    for label, (key, kind) in DIAGNOSTIC_SETTINGS.items():
        raw = values.get(label, "").strip()
        if not raw:
            continue
        try:
            if kind == "word":
                address = int(raw, 0)
                if not 0 <= address <= 0xffffffff or (address & 3):
                    raise ValueError
                result[key] = f"0x{address:08x}"
            else:
                first, last = raw.split(":", 1)
                start = int(first, 0)
                end = int(last, 0)
                if (start < 0 or end > 0xffffffff or
                        start > end or end - start > 0x2000 or
                        (start & 3) or (end & 3)):
                    raise ValueError
                result[key] = f"0x{start:08x}:0x{end:08x}"
        except (ValueError, TypeError):
            requirement = (
                "a 4-byte-aligned 32-bit address" if kind == "word" else
                "an aligned START:END range no larger than 0x2000 bytes"
            )
            raise ValueError(f"{label} must be {requirement}.") from None
    return result


OPTION_HELP = {
    "Extracted disc": "Folder containing the extracted game files and SYSTEM.CNF.",
    "Memory card": "Optional raw 8,650,752-byte PS2 card image for slot 1. Game writes may be saved to this file.",
    "Session mode": (
        "Full replay runs the traced boot. Automatic uses the launcher’s normal selection. "
        "Quick probe checks cached coverage. Matched replay runs neutral and Cross inputs "
        "with the same cached code. Cache recovery reconstructs known IR without running the game."
    ),
    "Controller profile": (
        "Neutral leaves the controller free of scripted presses. Cross replay injects "
        "eight polls starting at poll 200. Custom uses Limits and diagnostics; Scripted uses the Runtime tab."
    ),
    "WebGPU window": "Open the raylib game window with live controller input. Available for Full replay and Automatic.",
    "Deinterlacing": "Reconstruct interlaced fields at presentation: weave, bob, blend or motion-adaptive. TFF/BFF selects top/bottom field first. Changes apply during gameplay; raw captures are unchanged.",
    "Display aspect": "Host presentation aspect ratio. 4:3 corrects PS2 non-square pixels; Native pixels preserves raw GS dimensions. Changes apply during gameplay; captures are unaffected.",
    "Speed": "Target emulation speed for the game window. Headless expansion remains unpaced.",
    "WebGPU snapshot": "Save a WebGPU-rendered display snapshot. Available for Full replay and Automatic.",
    "Show game in launcher tab": (
        "Embed the native raylib/WebGPU window in the Game window tab rather than "
        "opening it separately. The Game tab's WebGPU window option must also be enabled. "
        "Windows only; this does not emulate a video stream or restart the game."
    ),
    "Resume after cache miss": (
        "Keep the current game process and guest state alive while an absent EE function "
        "is compiled and hot-loaded. Available during Automatic, Full replay and Quick probe. "
        "This is not a persistent savestate: closing the process loses its state. "
        "Uncheck to restart each replay after compiling a missing function. "
        "The Performance tab selects fast overlays or full relinks for live misses."
    ),
    "Precompile all functions on a cold cache": (
        "On a genuinely cold translation, compile all statically discovered EE functions "
        "before boot instead of repeatedly pausing for them. Existing valid caches are reused "
        "without retranslating. Uncheck to compile the entry point first."
    ),
    "Fast live overlays": (
        "After a live cache miss, compile and load only the newly discovered EE functions "
        "as an overlay DLL. This is PR #152's fast path, enabled by default. "
        "Uncheck to use the legacy whole-library relink on each live miss. "
        "Requires Resume after cache miss on the Game tab."
    ),
    "Accept changed IR hashes": "Allow cache recovery to accept IR hashes that differ from the saved manifest.",
    "Cross first poll": "The first port 1 controller poll that receives scripted Cross. Select the Custom profile to use it.",
    "Cross poll count": "How many consecutive polls hold Cross. Select the Custom profile to use it.",
    "Max dispatches": "Maximum translated EE dispatches per pass.",
    "Timeout (seconds)": "Wall-clock timeout for a replay or probe, in seconds.",
    "Expansion passes": "Maximum auto-expansion passes. Range 1–64.",
    "Functions per expansion": (
        "Maximum newly compiled EE functions per pass (1–1024). With live resume, "
        "the guest keeps running until this limit is reached or another stop occurs."
    ),
    "Compile jobs": "Number of parallel jobs used to compile translated guest code.",
    "SIF poll limit": "Maximum polling iterations allowed at a SIF wait boundary.",
    "Loop watchdog resumes": "How many times to resume the native loop watchdog between progress events. Zero stops on the first watchdog hit.",
    "Trace every N dispatches": "Sample ordinary verbose dispatch lines at this interval. One logs every dispatch.",
    "Report snapshot interval (ms)": "Interval between periodic JSON reports, in milliseconds. Zero uses dispatch-based checkpoints.",
    "Watch guest word": "Track changes between native dispatches. Example: 0x0044081c (sound-object handle).",
    "Trace guest stores": "Capture the exact guest STORE PCs and operands modifying this word (first 64 events). Example: 0x0044081c.",
    "Trace V0 writes": "Trace low-64-bit V0 assignments within an aligned guest PC range START:END (first 64 events). Example: 0x00137970:0x00137d18.",
    "Accelerate idle polling loops": "Enable the runtime idle-loop accelerator. Uncheck for baseline comparisons.",
    "Pad script": "Select Scripted on the Game tab. Example: 740:8:cross,2900:8:cross,3500:4:start. Combine buttons with +.",
    "IOP AOT library or directory": "Load an IRX library or OUT/modules directory. With capture enabled, missing libraries are built after the first replay.",
    "Capture linked IRX modules": "Capture relocated and linked IRX bytes for tools/build_iop_aot.py. Directory created when enabled.",
    "Verify IOP AOT (diagnostic only)": "Replay translated calls on the interpreter; expensive and may repeat MMIO side effects. Do not use for normal play.",
    "Trace IPU": "Log IPU commands and MMIO activity.",
    "Trace CDVD": "Log disc searches and reads.",
    "Trace rejected RPC commands": "Dump refused SIF RPC command words.",
    "Dump IOP threads at exit": "Print IOP thread state when the runner exits.",
    "IOP semaphore trace limit": "Tick to override the bounded semaphore event count (runtime default: 128).",
    "Trace RPC server ID": "Dump requests for this server ID (decimal or 0x hexadecimal).",
    "Frame dump directory": "Directory for periodic WebGPU PPM output; created when enabled. Only used in graphical replay modes.",
    "Frame dump interval (fields)": "Number of GS fields between frame dumps.",
    "EE RAM dump file": "Write the 32 MiB EE RAM image at exit. Existing files may be overwritten.",
    "Tool": "Choose the boot session orchestrator, Python game runner, or native executable for a direct command.",
    "Native executable": "Path to ee_game_runner.exe for the Native runner tool.",
    "Arguments": "Enter one argument per line. Keep paths containing spaces on one line without surrounding quotes.",
    "Show --help": "Print the selected tool’s command-line help in the output pane.",
    "Launch": "Launch the selected command, or close the running session without exiting the launcher.",
}


class Tooltip:
    def __init__(self, widget, text):
        self.widget = widget
        self.text = text
        self.pending = None
        self.popup = None
        widget.bind("<Enter>", self.schedule, add="+")
        for event in ("<Leave>", "<ButtonPress>", "<Unmap>", "<Destroy>"):
            widget.bind(event, self.hide, add="+")

    def schedule(self, _event):
        self.hide()
        self.pending = self.widget.after(500, self.show)

    def show(self):
        self.pending = None
        popup = tk.Toplevel(self.widget)
        self.popup = popup
        popup.withdraw()
        popup.overrideredirect(True)
        ttk.Label(
            popup, text=self.text, wraplength=320, padding=(8, 5), relief="solid"
        ).pack()
        popup.update_idletasks()
        x = self.widget.winfo_rootx()
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 3
        popup.geometry(f"{x:+d}{y:+d}")
        popup.deiconify()

    def hide(self, _event=None):
        if self.pending is not None:
            self.widget.after_cancel(self.pending)
            self.pending = None
        if self.popup is not None:
            self.popup.destroy()
            self.popup = None



def replay_limits(mode):
    if mode == "Quick probe":
        return "100000", "120"
    if mode in ("Full replay", "Matched neutral/Cross"):
        return "6000000", "600"
    return "", ""


def default_settings():
    dispatches, timeout = replay_limits("Full replay")
    runner = ROOT / "build" / "ee_game_runner.exe"
    release_runner = runner.parent / "Release" / runner.name
    if not runner.is_file() and release_runner.is_file():
        runner = release_runner
    return {
        "disc": str(ROOT / "PS2_GAME"),
        "card": "",
        "mode": "Full replay",
        "pad": "Neutral",
        "accept_ir": False,
        "window": True,
        "embed_game_window": True,
        "snapshot": True,
        "speed": "100%",
        "display_aspect": "4:3 (PS2)",
        "deinterlace": "Automatic",
        "cache_miss_resume": True,
        "precompile_all": True,
        "fast_overlays": True,
        **{key: default for key, (_, default, _) in RUNTIME_SETTINGS.items()},
        "diagnostics": {label: "" for label in DIAGNOSTIC_SETTINGS},
        "tool": COMMAND_TOOLS[0],
        "executable": str(runner),
        "arguments": "",
        "tool_options": {},
        "numeric": {
            "SDBZ_PAD0_CROSS_FIRST_POLL": "200",
            "SDBZ_PAD0_CROSS_POLL_COUNT": "8",
            "MAX_DISPATCHES": dispatches,
            "BOOT_TIMEOUT_SECONDS": timeout,
            "EXPANSION_PASSES": "8",
            "EXPANSION_PER_PASS": "128",
            "EMIT_JOBS": "",
            "SIF_POLL_LIMIT": "2048",
            "LOOP_YIELD_RESUMES": "16",
            "SDBZ_DISPATCH_TRACE_EVERY": "1024",
            "SDBZ_REPORT_SNAPSHOT_MS": "2000",
        },
    }


def batch_tool_defaults(disc):
    """Map the batch launcher's full-replay defaults to each tool's CLI."""
    numeric = default_settings()["numeric"]
    work = Path("build") / "ps2-game-hle"
    disc = Path(disc).expanduser()
    common = {
        "--disc-folder": str(disc),
        "--report": str(work / "first-boot.json"),
        "--gs-snapshot": str(work / "gs-display.ppm"),
        "--gs-upload-snapshot": str(work / "gs-upload.ppm"),
        "--gs-webgpu-snapshot": str(work / "gs-webgpu.ppm"),
        "--gs-webgpu-window": True,
        "--max-dispatches": numeric["MAX_DISPATCHES"],
        "--loop-budget": "1000000",
        "--loop-yield-resumes": numeric["LOOP_YIELD_RESUMES"],
        "--sif-poll-limit": numeric["SIF_POLL_LIMIT"],
        "--hle-startup": True,
        "--kseg-ram": True,
    }
    try:
        common["elf"] = str(resolve_boot_elf(disc))
    except (OSError, ValueError):
        pass
    python_defaults = {
        **common,
        "--build-dir": "build",
        "--config": "Release",
        "--work-dir": str(work),
        "--emit-jobs": numeric["EMIT_JOBS"] or str(min(32, os.cpu_count() or 8)),
        "--emit-opt": "0",
        "--functions": "all",
        "--timeout-seconds": numeric["BOOT_TIMEOUT_SECONDS"],
    }
    defaults = {
        "Boot session": {
            **python_defaults,
            "--disc-root": str(disc),
            "--expansion-passes": numeric["EXPANSION_PASSES"],
            "--expansion-per-pass": numeric["EXPANSION_PER_PASS"],
            "--replay-after-expand": False,
            "--live-full-relink": False,
            "--ll-progress": True,
        },
        "Game runner (Python)": {
            **python_defaults,
            "--auto-expand": numeric["EXPANSION_PER_PASS"],
            "--replay-after-expand": False,
            "--live-full-relink": False,
            "--trace": True,
        },
        "Native runner": {
            **common,
            "library": str(work / "game-translated.dll"),
            "--trace": True,
        },
    }
    for tool, values in defaults.items():
        for option in TOOL_OPTIONS[tool]["Trace"]:
            if option.kind != "flag":
                values[option.name] = option.default
    return defaults


def load_settings(path):
    settings = default_settings()
    try:
        saved = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return settings, ""
    except (OSError, ValueError):
        return settings, f"Could not read {path.name}. Loaded defaults."
    if not isinstance(saved, dict):
        return settings, f"Invalid {path.name}. Loaded defaults."
    for key, default in settings.items():
        value = saved.get(key)
        if type(value) is type(default) and key not in ("numeric", "tool_options", "diagnostics"):
            settings[key] = value
    # Older versions used a nonempty field as the opt-in. Preserve those
    # choices while filling previously blank fields with the new defaults.
    for key in RUNTIME_GATED_FIELDS:
        enabled = f"{key}_enabled"
        if enabled not in saved:
            settings[enabled] = isinstance(saved.get(key), str) and bool(saved[key].strip())
        if not settings[key].strip():
            settings[key] = RUNTIME_SETTINGS[key][1]
    if not settings["pad_script"].strip():
        settings["pad_script"] = RUNTIME_SETTINGS["pad_script"][1]
    if not settings["frame_interval"].strip():
        settings["frame_interval"] = RUNTIME_SETTINGS["frame_interval"][1]
    for key, choices in (("mode", SESSION_MODES), ("pad", PAD_PROFILES), ("tool", COMMAND_TOOLS), ("speed", SPEEDS), ("display_aspect", DISPLAY_ASPECTS), ("deinterlace", DEINTERLACE_MODES)):
        if settings[key] not in choices:
            settings[key] = default_settings()[key]
    saved_diagnostics = saved.get("diagnostics")
    if isinstance(saved_diagnostics, dict):
        for label in DIAGNOSTIC_SETTINGS:
            if isinstance(saved_diagnostics.get(label), str):
                settings["diagnostics"][label] = saved_diagnostics[label]
    saved_numeric = saved.get("numeric")
    numeric = settings["numeric"]
    numeric["MAX_DISPATCHES"], numeric["BOOT_TIMEOUT_SECONDS"] = replay_limits(settings["mode"])
    if isinstance(saved_numeric, dict):
        for key in numeric:
            if isinstance(saved_numeric.get(key), str):
                numeric[key] = saved_numeric[key]
    saved_tools = saved.get("tool_options")
    if isinstance(saved_tools, dict):
        for tool, groups in TOOL_OPTIONS.items():
            raw = saved_tools.get(tool)
            if not isinstance(raw, dict):
                continue
            typed = {
                option.name: raw[option.name]
                for options in groups.values() for option in options
                if type(raw.get(option.name)) is (bool if option.kind == "flag" else str)
            }
            if isinstance(raw.get("extra"), str):
                typed["extra"] = raw["extra"]
            settings["tool_options"][tool] = typed
    if settings["arguments"]:
        selected_options = settings["tool_options"].setdefault(settings["tool"], {})
        selected_options.setdefault("extra", settings["arguments"])
        # Old manual argv already contains its own positionals and flags.
        for options in TOOL_OPTIONS[settings["tool"]].values():
            for option in options:
                selected_options.setdefault(option.name, False if option.kind == "flag" else "")
        settings["arguments"] = ""
    return settings, ""


def save_settings(path, settings):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with NamedTemporaryFile(
            mode="w", encoding="utf-8", dir=path.parent, delete=False
        ) as output:
            temporary = Path(output.name)
            json.dump(settings, output, indent=2, ensure_ascii=False)
            output.write("\n")
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def launcher_options(mode, pad, accept_new_ir, window, snapshot, values,
                     resume_on_cache_miss=True, precompile_all=True,
                     fast_overlays=True, diagnostics=None, runtime=None):
    args = []
    mode_flag = SESSION_MODES[mode]
    if mode_flag:
        args.append(mode_flag)
    if mode == "Recover translation cache" and accept_new_ir:
        args.append("--accept-new-ir")
    supports_expansion = mode not in (
        "Matched neutral/Cross", "Recover translation cache"
    )
    if not resume_on_cache_miss and supports_expansion:
        args.append("--replay-after-expand")
    if resume_on_cache_miss and supports_expansion and not fast_overlays:
        args.append("--live-full-relink")
    env = {}
    if supports_expansion:
        env["PRECOMPILE_ALL"] = str(int(precompile_all))
        env.update(diagnostic_overrides(diagnostics or {}))
    if mode in ("Automatic", "Full replay"):
        env["EE_WEBGPU_WINDOW"] = str(int(window))
        env["EE_WEBGPU_SNAPSHOT"] = str(int(snapshot))
    uses_pad_profile = mode not in ("Matched neutral/Cross", "Recover translation cache")
    if uses_pad_profile and PAD_PROFILES[pad]:
        args.append(PAD_PROFILES[pad])

    for label, (key, minimum, maximum) in NUMERIC_SETTINGS.items():
        value = values[label].strip()
        if key.startswith("SDBZ_PAD0_CROSS_") and not (uses_pad_profile and pad == "Custom"):
            continue
        if not value:
            continue
        if not value.isascii() or not value.isdecimal():
            raise ValueError(f"{label} must be a whole number.")
        number = int(value)
        if number < minimum or (maximum is not None and number > maximum):
            limit = f"{minimum}–{maximum}" if maximum is not None else f"{minimum} or more"
            raise ValueError(f"{label} must be {limit}.")
        env[key] = str(number)
    if uses_pad_profile and pad == "Custom":
        if not all(key in env for key in ("SDBZ_PAD0_CROSS_FIRST_POLL", "SDBZ_PAD0_CROSS_POLL_COUNT")):
            raise ValueError("Set both values for the custom Cross pulse.")
    env.update(runtime_overrides(mode, pad, window, snapshot, runtime or {}))
    return args, env


def configure_game_window_environment(env, parent_handle=None):
    """Pass an optional live Tk Win32 parent HWND to the native game runner.

    Batch/Python boot layers inherit the environment and do not need an
    additional CLI argument. Never inherit a stale handle from a prior run.
    """
    env.pop("SDBZ_GS_PARENT_HWND", None)
    if parent_handle is not None:
        if isinstance(parent_handle, bool) or not isinstance(parent_handle, int) or parent_handle <= 0:
            raise ValueError("Game window parent must be a valid Win32 HWND.")
        env["SDBZ_GS_PARENT_HWND"] = str(parent_handle)


def direct_command(tool, executable, arguments, help_only=False):
    if tool == "Native runner":
        path = Path(executable).expanduser().resolve()
        if not path.is_file():
            raise ValueError("Choose an existing native runner executable.")
        command = [str(path)]
    else:
        script = "run_boot_session.py" if tool == "Boot session" else "run_game.py"
        command = [sys.executable, str(ROOT / "tools" / script)]
    if help_only:
        return command + ["--help"]
    # One argument per line keeps Windows paths and whitespace literal.
    return command + arguments


def write_presentation_setting(path, value):
    """Publish one complete presentation setting to the current native session."""
    temporary = None
    try:
        with NamedTemporaryFile(mode="w", encoding="ascii", dir=path.parent,
                                delete=False) as output:
            temporary = Path(output.name)
            output.write(value)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def write_deinterlace_mode(path, selection):
    write_presentation_setting(path, DEINTERLACE_MODES[selection])


def write_display_aspect(path, selection):
    write_presentation_setting(path, DISPLAY_ASPECTS[selection])


def request_embedded_close(parent_handle):
    """Request a normal close of the native child so the runner can finalize."""
    if os.name != "nt" or not parent_handle:
        return False
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    find_window = user32.FindWindowExW
    find_window.argtypes = (wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR)
    find_window.restype = wintypes.HWND
    post_message = user32.PostMessageW
    post_message.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
    post_message.restype = wintypes.BOOL
    child = find_window(parent_handle, None, None, "Super Dragon Ball Z - native WebGPU")
    return bool(child and post_message(child, 0x0010, 0, 0))  # WM_CLOSE


def stop_process_tree(process):
    """Stop the batch/Python/native/compile tree, not just its cmd.exe root."""
    if process.poll() is not None:
        return None
    return subprocess.Popen(
        ["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW,
    )


class Launcher(ttk.Frame):
    def __init__(self, window):
        super().__init__(window, padding=18)
        self.grid(sticky="nsew")
        self.columnconfigure(1, weight=1)
        self.rowconfigure(0, weight=2)
        self.rowconfigure(4, weight=1)
        settings, warning = load_settings(SETTINGS_PATH)
        self.fields = {
            key: (tk.BooleanVar if isinstance(value, bool) else tk.StringVar)(value=value)
            for key, value in settings.items() if key not in ("numeric", "arguments", "tool_options", "diagnostics")
        }
        self.disc = self.fields["disc"]
        self.card = self.fields["card"]
        self.mode = self.fields["mode"]
        self.pad = self.fields["pad"]
        self.accept_ir = self.fields["accept_ir"]
        self.window = self.fields["window"]
        self.embed_game_window = self.fields["embed_game_window"]
        self.snapshot = self.fields["snapshot"]
        self.speed = self.fields["speed"]
        self.speed_status = tk.StringVar()
        self.cache_miss_resume = self.fields["cache_miss_resume"]
        self.precompile_all = self.fields["precompile_all"]
        self.fast_overlays = self.fields["fast_overlays"]
        self.diagnostics = {
            label: tk.StringVar(value=settings["diagnostics"][label])
            for label in DIAGNOSTIC_SETTINGS
        }
        self.performance_description = tk.StringVar()
        self.tool = self.fields["tool"]
        self.executable = self.fields["executable"]
        self.status = tk.StringVar(value=warning)
        self.process = None
        self.stop_process = None
        self.close_requested = False
        self.deinterlace_file = None
        self.aspect_file = None
        self.log = None
        self.embedded_running = False
        self.previous_mode = self.mode.get()
        self.numeric = {
            label: tk.StringVar(value=settings["numeric"][key])
            for label, (key, _, _) in NUMERIC_SETTINGS.items()
        }
        self.tabs = ttk.Notebook(self)
        self.tabs.grid(row=0, column=0, columnspan=3, sticky="nsew", pady=(0, 10))
        self.setup = ttk.Frame(self.tabs, padding=10)
        self.game_view = ttk.Frame(self.tabs, padding=6)
        self.performance = ttk.Frame(self.tabs, padding=10)
        self.limits = ttk.Frame(self.tabs, padding=10)
        self.runtime = ttk.Frame(self.tabs, padding=10)
        self.direct = ttk.Frame(self.tabs, padding=10)
        for tab, title in (
            (self.setup, "Game"), (self.game_view, "Game window"),
            (self.performance, "Performance"),
            (self.limits, "Limits and diagnostics"),
            (self.runtime, "Runtime"), (self.direct, "Direct command"),
        ):
            tab.columnconfigure(1, weight=1)
            self.tabs.add(tab, text=title)
        # Other form tabs use a second column, but the game viewport
        # occupies the full width of this tab.
        self.game_view.columnconfigure(1, weight=0)
        self.game_view.columnconfigure(0, weight=1)
        self.game_view.rowconfigure(0, weight=1)
        # A real Win32 child HWND is parented to this black Tk viewport.
        # tk.Frame, rather than a canvas image, keeps controller input and
        # native WebGPU presentation in the game's own process.
        self.game_host = tk.Frame(
            self.game_view, background="black", width=800, height=450,
            highlightthickness=0, borderwidth=0,
        )
        self.game_host.grid(row=0, column=0, sticky="nsew")
        ttk.Label(
            self.game_view,
            text="The native game will appear here after Launch. "
                 "The log remains visible below.",
        ).grid(row=1, column=0, sticky="w", pady=(6, 0))

        ttk.Label(self.game_view, textvariable=self.speed_status).grid(row=2, column=0, sticky="w")

        self.path_row(self.setup, 0, "Extracted disc", self.disc, filedialog.askdirectory)
        self.path_row(self.setup, 1, "Memory card", self.card, filedialog.askopenfilename)
        self.choice_row(self.setup, 2, "Session mode", self.mode, SESSION_MODES)
        self.pad_picker = self.choice_row(self.setup, 3, "Controller profile", self.pad, PAD_PROFILES)
        self.window_picker = ttk.Checkbutton(self.setup, text="WebGPU window", variable=self.window)
        self.window_picker.grid(row=4, column=1, sticky="w")
        Tooltip(self.window_picker, OPTION_HELP["WebGPU window"])
        self.embed_picker = ttk.Checkbutton(
            self.setup, text="Show game in launcher tab", variable=self.embed_game_window
        )
        self.embed_picker.grid(row=5, column=1, sticky="w")
        Tooltip(self.embed_picker, OPTION_HELP["Show game in launcher tab"])
        self.snapshot_picker = ttk.Checkbutton(self.setup, text="WebGPU snapshot", variable=self.snapshot)
        self.snapshot_picker.grid(row=6, column=1, sticky="w")
        Tooltip(self.snapshot_picker, OPTION_HELP["WebGPU snapshot"])
        self.cache_miss_picker = ttk.Checkbutton(
            self.setup, text="Resume after cache miss", variable=self.cache_miss_resume
        )
        self.cache_miss_picker.grid(row=7, column=1, sticky="w")
        Tooltip(self.cache_miss_picker, OPTION_HELP["Resume after cache miss"])
        ttk.Label(
            self.setup, text="Preserves live state during compilation; not a disk savestate.",
            foreground="gray"
        ).grid(row=8, column=1, columnspan=2, sticky="w", pady=(0, 5))
        self.ir_picker = ttk.Checkbutton(self.setup, text="Accept changed IR hashes", variable=self.accept_ir)
        self.ir_picker.grid(row=9, column=1, sticky="w")
        Tooltip(self.ir_picker, OPTION_HELP["Accept changed IR hashes"])
        self.speed_picker = self.choice_row(self.setup, 10, "Speed", self.speed, SPEEDS)
        self.aspect_picker = self.choice_row(self.setup, 11, "Display aspect", self.fields["display_aspect"], DISPLAY_ASPECTS)
        self.deinterlace_picker = self.choice_row(self.setup, 12, "Deinterlacing", self.fields["deinterlace"], DEINTERLACE_MODES)
        ttk.Label(self.setup, textvariable=self.speed_status).grid(row=13, column=1, sticky="w")
        ttk.Label(
            self.performance,
            text="Native dispatch uses the indexed guest-PC lookup from PR #152.",
            wraplength=670,
        ).grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 10))
        self.precompile_picker = ttk.Checkbutton(
            self.performance, text="Precompile all functions on a cold cache",
            variable=self.precompile_all,
        )
        self.precompile_picker.grid(row=1, column=0, columnspan=3, sticky="w", pady=4)
        Tooltip(self.precompile_picker, OPTION_HELP["Precompile all functions on a cold cache"])
        self.overlay_picker = ttk.Checkbutton(
            self.performance, text="Fast live overlays (instead of full DLL relinks)",
            variable=self.fast_overlays,
        )
        self.overlay_picker.grid(row=2, column=0, columnspan=3, sticky="w", pady=4)
        Tooltip(self.overlay_picker, OPTION_HELP["Fast live overlays"])
        compile_caption = ttk.Label(self.performance, text="Compile jobs")
        compile_caption.grid(row=3, column=0, sticky="w", pady=(12, 0), padx=(0, 12))
        Tooltip(compile_caption, OPTION_HELP["Compile jobs"])
        self.compile_jobs_entry = ttk.Entry(
            self.performance, textvariable=self.numeric["Compile jobs"]
        )
        self.compile_jobs_entry.grid(row=3, column=1, sticky="ew", pady=(12, 0))
        Tooltip(
            self.compile_jobs_entry,
            "Leave blank to let the batch launcher use the CPU count. "
            "Enter a number to override it.",
        )
        ttk.Label(
            self.performance,
            text="Blank uses the batch launcher's automatic CPU count. "
                 "Direct command has independent translation options.",
            foreground="gray", wraplength=670,
        ).grid(row=4, column=0, columnspan=3, sticky="w", pady=(4, 14))
        ttk.Label(
            self.performance, textvariable=self.performance_description,
            wraplength=670,
        ).grid(row=5, column=0, columnspan=3, sticky="w")
        ttk.Label(
            self.performance,
            text="Optional guest memory / return-value probes (blank = off)",
        ).grid(row=6, column=0, columnspan=3, sticky="w", pady=(18, 8))
        for offset, (label, variable) in enumerate(self.diagnostics.items(), 7):
            caption = ttk.Label(self.performance, text=label)
            caption.grid(row=offset, column=0, sticky="w", padx=(0, 12), pady=3)
            entry = ttk.Entry(self.performance, textvariable=variable)
            entry.grid(row=offset, column=1, columnspan=2, sticky="ew", pady=3)
            Tooltip(caption, OPTION_HELP[label])
            Tooltip(entry, OPTION_HELP[label])

        self.cross_entries = []
        for row, (label, variable) in enumerate(self.numeric.items()):
            if label == "Compile jobs":
                continue
            caption = ttk.Label(self.limits, text=label)
            caption.grid(row=row, column=0, sticky="w", padx=(0, 12))
            Tooltip(caption, OPTION_HELP[label])
            entry = ttk.Entry(self.limits, textvariable=variable)
            entry.grid(row=row, column=1, sticky="ew", pady=2)
            Tooltip(entry, OPTION_HELP[label])
            if row < 2:
                self.cross_entries.append(entry)
        ttk.Label(self.limits, text="Blank fields use the batch launcher defaults.").grid(
            row=len(self.numeric), column=0, columnspan=2, sticky="w", pady=(6, 0)
        )
        ttk.Label(
            self.runtime, text="Runtime options for Game launches. Tick a parameter to enable it.",
        ).grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 8))
        visible_runtime = [(key, spec) for key, spec in RUNTIME_SETTINGS.items()
                           if not key.endswith("_enabled")]
        for row, (key, (_, default, label)) in enumerate(visible_runtime, 1):
            variable = self.fields[key]
            enabled = self.fields[f"{key}_enabled"] if key in RUNTIME_GATED_FIELDS else None
            if isinstance(default, bool):
                widget = ttk.Checkbutton(self.runtime, text=label, variable=variable)
                widget.grid(row=row, column=0, columnspan=3, sticky="w", pady=2)
                Tooltip(widget, OPTION_HELP[label])
            elif key in ("iop_aot", "iop_capture", "frame_dir", "dump_ram"):
                picker = filedialog.asksaveasfilename if key == "dump_ram" else filedialog.askdirectory
                self.path_row(self.runtime, row, label, variable, picker, enabled=enabled)
                if key == "iop_aot":
                    button = ttk.Button(
                        self.runtime, text="File…",
                        command=lambda: self.browse_aot_file(),
                    )
                    button.grid(row=row, column=3, padx=(4, 0))
            else:
                caption = (ttk.Checkbutton(self.runtime, text=label, variable=enabled)
                           if enabled is not None else ttk.Label(self.runtime, text=label))
                caption.grid(row=row, column=0, sticky="w", padx=(0, 12))
                entry = ttk.Entry(self.runtime, textvariable=variable)
                entry.grid(row=row, column=1, columnspan=2, sticky="ew", pady=2)
                Tooltip(caption, OPTION_HELP[label])
                Tooltip(entry, OPTION_HELP[label])
        ttk.Label(
            self.runtime, text="AOT: enable loading and capture together to build missing DLLs after the first replay. Next launch loads them. "
            "Matched runs suppress scripted input and output dumps. Cache recovery ignores runtime options.",
            wraplength=670,
        ).grid(row=len(visible_runtime) + 1, column=0, columnspan=4, sticky="w", pady=(8, 0))
        self.choice_row(self.direct, 0, "Tool", self.tool, COMMAND_TOOLS)
        self.path_row(self.direct, 1, "Native executable", self.executable, filedialog.askopenfilename)
        self.tool_forms = {}
        self.tool_values = {}
        self.tool_extra = {}
        tool_defaults = batch_tool_defaults(self.disc.get())
        for tool, groups in TOOL_OPTIONS.items():
            form = ttk.Notebook(self.direct)
            self.tool_forms[tool] = form
            saved = {**tool_defaults[tool], **settings["tool_options"].get(tool, {})}
            values = self.tool_values[tool] = {}
            for group, options in groups.items():
                page = ttk.Frame(form, padding=8)
                page.columnconfigure(1, weight=1)
                form.add(page, text=group)
                for row, option in enumerate(options):
                    self.option_row(page, row, option, saved, values)
                if group == "Trace":
                    general_trace = (
                        "The boot session enables tracing for its final replay."
                        if tool == "Boot session" else "Batch defaults enable --trace."
                    )
                    ttk.Label(
                        page, text=general_trace + " Blank fields disable targeted traces.",
                        wraplength=480,
                    ).grid(row=len(options), column=0, columnspan=3, sticky="w", pady=(8, 0))
            additional = ttk.Frame(form, padding=8)
            additional.columnconfigure(0, weight=1)
            form.add(additional, text="Extra arguments")
            ttk.Label(additional, text="One additional argument per line").grid(sticky="w")
            extra = tk.Text(additional, height=5, width=48, wrap="none")
            extra.grid(sticky="ew")
            extra.insert("1.0", saved.get("extra", ""))
            Tooltip(extra, OPTION_HELP["Arguments"])
            self.tool_extra[tool] = extra
        self.tool.trace_add("write", self.show_tool)
        self.show_tool()
        self.help_button = ttk.Button(self.direct, text="Show --help", command=lambda: self.launch(True))
        self.help_button.grid(row=3, column=1, sticky="w", pady=(6, 0))
        Tooltip(self.help_button, OPTION_HELP["Show --help"])
        defaults_button = ttk.Button(self.direct, text="Apply batch defaults", command=self.apply_batch_defaults)
        defaults_button.grid(row=3, column=2, sticky="e", pady=(6, 0))
        Tooltip(defaults_button, "Apply the full-replay defaults from run_ps2_game.bat to this tool.")
        self.mode.trace_add("write", self.change_mode)
        self.window.trace_add("write", self.update_fields)
        self.cache_miss_resume.trace_add("write", self.update_fields)
        self.precompile_all.trace_add("write", self.update_fields)
        self.fast_overlays.trace_add("write", self.update_fields)
        window.protocol("WM_DELETE_WINDOW", self.close)
        self.pad.trace_add("write", self.update_fields)
        self.fields["deinterlace"].trace_add("write", self.change_deinterlace)
        self.fields["display_aspect"].trace_add("write", self.change_display_aspect)
        self.update_fields()
        self.play = ttk.Button(self, text="Launch", command=self.toggle_launch)
        self.play.grid(row=3, column=0, sticky="w", pady=(0, 10))
        Tooltip(self.play, OPTION_HELP["Launch"])
        ttk.Label(self, textvariable=self.status).grid(
            row=3, column=1, columnspan=2, sticky="w"
        )
        self.output = tk.Text(self, height=16, width=80, wrap="word", state="disabled")
        self.output.grid(row=4, column=0, columnspan=2, sticky="nsew")
        scrollbar = ttk.Scrollbar(self, command=self.output.yview)
        scrollbar.grid(row=4, column=2, sticky="ns")
        self.output.configure(yscrollcommand=scrollbar.set)
        if os.name != "nt":
            self.play.state(["disabled"])
            self.help_button.state(["disabled"])
            if not warning:
                self.status.set("The native raylib window is available on Windows.")

    def show_tool(self, *_):
        for tool, form in self.tool_forms.items():
            if tool == self.tool.get():
                form.grid(row=2, column=0, columnspan=3, sticky="ew")
            else:
                form.grid_remove()

    def apply_batch_defaults(self):
        tool = self.tool.get()
        for key, default in batch_tool_defaults(self.disc.get())[tool].items():
            self.tool_values[tool][key].set(default)

    def option_row(self, parent, row, option, saved, values):
        label = option.name + (" *" if option.required else "")
        if option.kind == "flag":
            value = tk.BooleanVar(value=saved.get(option.name, False))
            widget = ttk.Checkbutton(parent, text=label, variable=value)
            widget.grid(row=row, column=0, columnspan=2, sticky="w")
        else:
            caption = ttk.Label(parent, text=label)
            caption.grid(row=row, column=0, sticky="nw", padx=(0, 12), pady=2)
            Tooltip(caption, option.help)
            initial = saved.get(option.name, option.default)
            if option.kind == "repeat":
                widget = tk.Text(parent, height=3, width=40, wrap="none")
                widget.insert("1.0", initial)
                value = widget
            else:
                value = tk.StringVar(value=initial)
                if option.choices:
                    widget = ttk.Combobox(parent, values=("", *option.choices),
                                          textvariable=value, state="readonly")
                else:
                    widget = ttk.Entry(parent, textvariable=value)
            widget.grid(row=row, column=1, sticky="ew", pady=2)
            pickers = {
                "positional": filedialog.askopenfilename,
                "file": filedialog.askopenfilename,
                "directory": filedialog.askdirectory,
                "required-directory": filedialog.askdirectory,
                "output": filedialog.asksaveasfilename,
                "required-output": filedialog.asksaveasfilename,
            }
            if option.kind in pickers:
                def browse():
                    selected = pickers[option.kind](parent=self)
                    if selected:
                        value.set(selected)

                button = ttk.Button(parent, text="Browse", command=browse)
                button.grid(row=row, column=2, padx=(8, 0))
                Tooltip(button, option.help)
        Tooltip(widget, option.help)
        values[option.name] = value

    def direct_values(self, tool):
        return {
            key: value.get("1.0", "end-1c") if isinstance(value, tk.Text) else value.get()
            for key, value in self.tool_values[tool].items()
        }

    def change_mode(self, *_):
        mode = self.mode.get()
        for label, previous, current in zip(
            ("Max dispatches", "Timeout (seconds)"),
            replay_limits(self.previous_mode), replay_limits(mode),
        ):
            if self.numeric[label].get() == previous:
                self.numeric[label].set(current)
        self.previous_mode = mode
        self.update_fields()

    def change_deinterlace(self, *_):
        if self.deinterlace_file is not None:
            try:
                write_deinterlace_mode(self.deinterlace_file, self.fields["deinterlace"].get())
            except OSError as error:
                self.status.set(f"Could not update deinterlacing: {error}")

    def clear_deinterlace_file(self):
        if self.deinterlace_file is not None:
            try:
                self.deinterlace_file.unlink(missing_ok=True)
            except OSError:
                pass
            self.deinterlace_file = None

    def change_display_aspect(self, *_):
        if self.aspect_file is not None:
            try:
                write_display_aspect(self.aspect_file, self.fields["display_aspect"].get())
            except OSError as error:
                self.status.set(f"Could not update display aspect: {error}")

    def clear_aspect_file(self):
        if self.aspect_file is not None:
            try:
                self.aspect_file.unlink(missing_ok=True)
            except OSError:
                pass
            self.aspect_file = None

    def persist_settings(self):
        settings = {key: variable.get() for key, variable in self.fields.items()}
        settings["numeric"] = {
            key: self.numeric[label].get() for label, (key, _, _) in NUMERIC_SETTINGS.items()
        }
        settings["diagnostics"] = {
            label: var.get() for label, var in self.diagnostics.items()
        }
        settings["tool_options"] = {
            tool: dict(self.direct_values(tool), extra=extra.get("1.0", "end-1c"))
            for tool, extra in self.tool_extra.items()
        }
        save_settings(SETTINGS_PATH, settings)

    def close(self):
        if self.process is not None and self.embedded_running:
            if not messagebox.askokcancel(
                "Close embedded game?",
                "Closing the launcher also closes the embedded game window. "
                "Unsaved game state may be lost. Close anyway?",
                parent=self,
            ):
                return
        try:
            self.persist_settings()
        except OSError as error:
            messagebox.showerror("Could not save settings", str(error), parent=self)
        self.clear_deinterlace_file()
        self.clear_aspect_file()
        self.winfo_toplevel().destroy()

    def update_fields(self, *_):
        mode = self.mode.get()
        special = mode in ("Matched neutral/Cross", "Recover translation cache")
        self.pad_picker.configure(state="disabled" if special else "readonly")
        graphics_state = ["!disabled"] if mode in ("Automatic", "Full replay") else ["disabled"]
        self.window_picker.state(graphics_state)
        self.embed_picker.state(
            ["!disabled"] if os.name == "nt" and self.window.get() and
            mode in ("Automatic", "Full replay") else ["disabled"]
        )
        self.snapshot_picker.state(graphics_state)
        self.speed_picker.configure(state="readonly" if self.window.get() and
                                    mode in ("Automatic", "Full replay") else "disabled")
        self.aspect_picker.configure(state="readonly" if self.aspect_file is not None or (
            self.window.get() and mode in ("Automatic", "Full replay")) else "disabled")
        self.deinterlace_picker.configure(state="readonly" if self.deinterlace_file is not None or (
            self.window.get() and mode in ("Automatic", "Full replay")) else "disabled")
        self.ir_picker.state(["!disabled"] if mode == "Recover translation cache" else ["disabled"])
        self.cache_miss_picker.state(
            ["disabled"] if special else ["!disabled"]
        )
        self.precompile_picker.state(["disabled"] if special else ["!disabled"])
        overlays_available = not special and self.cache_miss_resume.get()
        self.overlay_picker.state(
            ["!disabled"] if overlays_available else ["disabled"]
        )
        if special:
            self.performance_description.set(
                "This session uses existing cached code; cold precompilation "
                "and live overlay settings do not apply."
            )
        elif not self.cache_miss_resume.get():
            self.performance_description.set(
                "Cache misses restart the replay (legacy mode); "
                "the overlay setting is not used."
            )
        else:
            self.performance_description.set(
                "Live cache misses: " +
                ("fast overlay DLLs" if self.fast_overlays.get()
                 else "whole-library relinks") +
                ". Cold cache: " +
                ("precompile all discovered functions." if self.precompile_all.get()
                 else "translate entry first.")
            )
        cross_state = ["!disabled"] if not special and self.pad.get() == "Custom" else ["disabled"]
        for entry in self.cross_entries:
            entry.state(cross_state)

    def choice_row(self, parent, row, label, variable, choices):
        caption = ttk.Label(parent, text=label)
        caption.grid(row=row, column=0, sticky="w", padx=(0, 12))
        Tooltip(caption, OPTION_HELP[label])
        picker = ttk.Combobox(parent, textvariable=variable, values=list(choices), state="readonly")
        picker.grid(row=row, column=1, columnspan=2, sticky="ew", pady=4)
        Tooltip(picker, OPTION_HELP[label])
        return picker

    def path_row(self, parent, row, label, variable, picker, enabled=None):
        def browse():
            selected = picker(parent=self)
            if selected:
                variable.set(selected)

        caption = (ttk.Checkbutton(parent, text=label, variable=enabled)
                   if enabled is not None else ttk.Label(parent, text=label))
        caption.grid(row=row, column=0, sticky="w", padx=(0, 12))
        Tooltip(caption, OPTION_HELP[label])
        entry = ttk.Entry(parent, textvariable=variable)
        entry.grid(row=row, column=1, sticky="ew", pady=4)
        button = ttk.Button(parent, text="Browse", command=browse)
        button.grid(row=row, column=2, padx=(10, 0))
        Tooltip(entry, OPTION_HELP[label])
        Tooltip(button, OPTION_HELP[label])

    def browse_aot_file(self):
        selected = filedialog.askopenfilename(parent=self, filetypes=[("Native library", "*.dll *.so"), ("All files", "*")])
        if selected:
            self.fields["iop_aot"].set(selected)

    def game_command(self, env):
        disc = Path(self.disc.get()).expanduser().resolve()
        if not (disc / "SYSTEM.CNF").is_file():
            raise ValueError("Choose the extracted disc folder containing SYSTEM.CNF.")
        args, overrides = launcher_options(
            self.mode.get(), self.pad.get(), self.accept_ir.get(), self.window.get(),
            self.snapshot.get(), {label: value.get() for label, value in self.numeric.items()},
            resume_on_cache_miss=self.cache_miss_resume.get(),
            precompile_all=self.precompile_all.get(),
            fast_overlays=self.fast_overlays.get(),
            diagnostics={label: var.get() for label, var in self.diagnostics.items()},
            runtime={key: self.fields[key].get() for key in RUNTIME_SETTINGS},
        )
        for key, _, _ in NUMERIC_SETTINGS.values():
            env.pop(key, None)
        for key, _ in DIAGNOSTIC_SETTINGS.values():
            env.pop(key, None)
        for key, _, _ in RUNTIME_SETTINGS.values():
            if key is not None:
                env.pop(key, None)
        env.pop("SDBZ_IOP_AOT_BUILD_OUT", None)
        prepare_runtime_outputs(overrides)
        env.update(overrides, SDBZ_DISC_ROOT=str(disc),
                   SDBZ_SPEED_PERCENT=str(SPEEDS[self.speed.get()]),
                   EE_WEBGPU_ASPECT=DISPLAY_ASPECTS[self.fields["display_aspect"].get()],
                   EE_WEBGPU_DEINTERLACE=DEINTERLACE_MODES[self.fields["deinterlace"].get()])
        return ["cmd.exe", "/d", "/c", "run_ps2_game.bat", *args]

    def toggle_launch(self):
        if self.process is None:
            self.launch()
        else:
            self.close_session()

    def close_session(self):
        if self.process is None or self.process.poll() is not None or self.stop_process is not None:
            return
        # A running embedded game can exit normally, allowing boot logs,
        # snapshots and the optional post-capture IOP build to finish.
        if not self.close_requested and self.embedded_running:
            try:
                if request_embedded_close(self.game_host.winfo_id()):
                    self.close_requested = True
                    self.play.configure(text="Force close")
                    self.status.set("Closing game; finalizing session and diagnostics")
                    return
            except OSError:
                pass
        # During compilation, headless execution, or an unresponsive
        # window, stop the full process tree. Keep the launcher responsive.
        try:
            self.stop_process = stop_process_tree(self.process)
        except OSError as error:
            self.status.set(f"Could not close session: {error}")
            return
        if self.stop_process is not None:
            self.close_requested = True
            self.play.configure(text="Closing…")
            self.play.state(["disabled"])
            self.status.set("Stopping the running process tree")

    def launch(self, help_only=False):
        if self.process is not None:
            return
        try:
            env = os.environ.copy()
            env["SDBZ_LAUNCHER_LIVE_PROGRESS"] = "1"
            is_direct = self.tabs.select() == str(self.direct)
            if is_direct:
                numeric = default_settings()["numeric"]
                for key in ("SDBZ_DISPATCH_TRACE_EVERY", "SDBZ_REPORT_SNAPSHOT_MS"):
                    env.setdefault(key, numeric[key])
                tool = self.tool.get()
                arguments = [] if help_only else tool_arguments(
                    tool, self.direct_values(tool), self.tool_extra[tool].get("1.0", "end-1c")
                )
                command = direct_command(tool, self.executable.get(), arguments, help_only)
            else:
                command = self.game_command(env)
            embed_game = (
                not help_only and os.name == "nt" and
                self.embed_game_window.get() and
                (
                    (not is_direct and self.window.get() and
                     self.mode.get() in ("Automatic", "Full replay"))
                    or (is_direct and "--gs-webgpu-window" in command)
                )
            )
            if embed_game:
                # The parent HWND must exist before the child starts.
                self.tabs.select(self.game_view)
                self.game_host.update_idletasks()
                configure_game_window_environment(env, self.game_host.winfo_id())
            else:
                configure_game_window_environment(env)
            if not help_only:
                card_text = self.card.get().strip()
                env.pop("SDBZ_MEMCARD", None)
                if card_text and (is_direct or self.mode.get() != "Recover translation cache"):
                    card = Path(card_text).expanduser().resolve()
                    if not card.is_file() or card.stat().st_size != 8_650_752:
                        raise ValueError("Choose an 8,650,752-byte raw PS2 memory card image.")
                    env["SDBZ_MEMCARD"] = str(card)
                self.persist_settings()
            LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
            env.pop("EE_WEBGPU_DEINTERLACE_FILE", None)
            env.pop("EE_WEBGPU_ASPECT_FILE", None)
            graphical = not help_only and (
                (not is_direct and self.window.get() and
                 self.mode.get() in ("Automatic", "Full replay"))
                or (is_direct and "--gs-webgpu-window" in command)
            )
            if graphical:
                selection = self.fields["deinterlace"].get()
                with NamedTemporaryFile(mode="w", encoding="ascii", dir=LOG_PATH.parent,
                                        prefix="deinterlace-", suffix=".mode", delete=False) as output:
                    self.deinterlace_file = Path(output.name)
                    output.write(DEINTERLACE_MODES[selection])
                env["EE_WEBGPU_DEINTERLACE"] = DEINTERLACE_MODES[selection]
                env["EE_WEBGPU_DEINTERLACE_FILE"] = str(self.deinterlace_file)
                aspect = DISPLAY_ASPECTS[self.fields["display_aspect"].get()]
                with NamedTemporaryFile(mode="w", encoding="ascii", dir=LOG_PATH.parent,
                                        prefix="aspect-", suffix=".mode", delete=False) as output:
                    self.aspect_file = Path(output.name)
                    output.write(aspect)
                env["EE_WEBGPU_ASPECT"] = aspect
                env["EE_WEBGPU_ASPECT_FILE"] = str(self.aspect_file)
            with LOG_PATH.open("wb") as output:
                self.process = subprocess.Popen(
                    command,
                    cwd=ROOT,
                    env=env,
                    stdin=subprocess.DEVNULL,
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                )
            self.log = LOG_PATH.open(encoding="utf-8", errors="replace")
            self.embedded_running = embed_game
            self.update_fields()
        except (OSError, ValueError) as error:
            self.clear_deinterlace_file()
            self.clear_aspect_file()
            self.status.set(str(error))
            return

        self.output.configure(state="normal")
        self.output.delete("1.0", tk.END)
        self.output.configure(state="disabled")
        self.close_requested = False
        self.stop_process = None
        self.play.configure(text="Close")
        self.play.state(["!disabled"])
        self.help_button.state(["disabled"])
        self.speed_status.set("")
        self.status.set("Running command")
        self.read_output()

    def read_output(self):
        chunk = self.log.read(32768)
        if chunk:
            for line in chunk.splitlines():
                if line.startswith("EE speed: "):
                    self.speed_status.set(line.removeprefix("EE speed: "))
            if not self.close_requested and "live cache-miss: PC=" in chunk:
                self.status.set("Compiling missing EE code; game state paused")
            if not self.close_requested and "EE hot-expand: resumed guest pc=" in chunk:
                self.status.set("Guest execution resumed after live IR expansion")
            self.output.configure(state="normal")
            self.output.insert(tk.END, chunk)
            lines = int(self.output.index("end-1c").split(".")[0])
            if lines > 2000:
                self.output.delete("1.0", f"{lines - 2000}.0")
            self.output.see(tk.END)
            self.output.configure(state="disabled")

        result = self.process.poll()
        if self.stop_process is not None and self.stop_process.poll() is not None:
            stop_result = self.stop_process.returncode
            self.stop_process = None
            if stop_result != 0 and result is None:
                self.play.configure(text="Force close")
                self.play.state(["!disabled"])
                self.status.set(f"Could not stop process tree ({stop_result}); retry Close")
        if chunk or result is None or self.stop_process is not None:
            self.after(150, self.read_output)
        else:
            self.log.close()
            self.log = None
            self.process = None
            self.clear_deinterlace_file()
            self.clear_aspect_file()
            self.embedded_running = False
            self.close_requested = False
            self.play.configure(text="Launch")
            self.update_fields()
            self.play.state(["!disabled"])
            self.help_button.state(["!disabled"])
            self.status.set(f"Launcher exited ({result}). Log: {LOG_PATH.name}")


def main():
    window = tk.Tk()
    window.title("Native PS2 Launcher")
    window.geometry("1080x920")
    window.minsize(800, 650)
    window.columnconfigure(0, weight=1)
    window.rowconfigure(0, weight=1)
    Launcher(window)
    window.mainloop()


if __name__ == "__main__":
    main()
