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

SESSION_MODES = {
    "Full replay": "--full",
    "Automatic": None,
    "Quick probe": "--quick",
    "Matched neutral/Cross": "--matched",
    "Recover translation cache": "--recover-cache",
}
PAD_PROFILES = {"Neutral": "--neutral", "Cross replay": "--cross", "Custom": None}
NUMERIC_SETTINGS = {
    "Cross first poll": ("SDBZ_PAD0_CROSS_FIRST_POLL", 1, 0xFFFFFFFF),
    "Cross poll count": ("SDBZ_PAD0_CROSS_POLL_COUNT", 1, 0xFFFFFFFF),
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
        "eight polls starting at poll 200. Custom uses the values under Limits and diagnostics."
    ),
    "WebGPU window": "Open the raylib game window with live controller input. Available for Full replay and Automatic.",
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
    "Tool": "Choose the boot session orchestrator, Python game runner, or native executable for a direct command.",
    "Native executable": "Path to ee_game_runner.exe for the Native runner tool.",
    "Arguments": "Enter one argument per line. Keep paths containing spaces on one line without surrounding quotes.",
    "Show --help": "Print the selected tool’s command-line help in the output pane.",
    "Launch": "Run the command configured in the selected tab.",
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
        "cache_miss_resume": True,
        "precompile_all": True,
        "fast_overlays": True,
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
        if type(value) is type(default) and key not in ("numeric", "tool_options"):
            settings[key] = value
    for key, choices in (("mode", SESSION_MODES), ("pad", PAD_PROFILES), ("tool", COMMAND_TOOLS), ("speed", SPEEDS)):
        if settings[key] not in choices:
            settings[key] = default_settings()[key]
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
                     fast_overlays=True):
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
            for key, value in settings.items() if key not in ("numeric", "arguments", "tool_options")
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
        self.performance_description = tk.StringVar()
        self.tool = self.fields["tool"]
        self.executable = self.fields["executable"]
        self.status = tk.StringVar(value=warning)
        self.process = None
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
        self.direct = ttk.Frame(self.tabs, padding=10)
        for tab, title in (
            (self.setup, "Game"), (self.game_view, "Game window"),
            (self.performance, "Performance"),
            (self.limits, "Limits and diagnostics"),
            (self.direct, "Direct command"),
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
        ttk.Label(self.setup, textvariable=self.speed_status).grid(row=11, column=1, sticky="w")
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
        self.update_fields()
        self.play = ttk.Button(self, text="Launch", command=self.launch)
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

    def persist_settings(self):
        settings = {key: variable.get() for key, variable in self.fields.items()}
        settings["numeric"] = {
            key: self.numeric[label].get() for label, (key, _, _) in NUMERIC_SETTINGS.items()
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

    def path_row(self, parent, row, label, variable, picker):
        def browse():
            selected = picker(parent=self)
            if selected:
                variable.set(selected)

        caption = ttk.Label(parent, text=label)
        caption.grid(row=row, column=0, sticky="w", padx=(0, 12))
        Tooltip(caption, OPTION_HELP[label])
        entry = ttk.Entry(parent, textvariable=variable)
        entry.grid(row=row, column=1, sticky="ew", pady=4)
        button = ttk.Button(parent, text="Browse", command=browse)
        button.grid(row=row, column=2, padx=(10, 0))
        Tooltip(entry, OPTION_HELP[label])
        Tooltip(button, OPTION_HELP[label])

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
        )
        for key, _, _ in NUMERIC_SETTINGS.values():
            env.pop(key, None)
        env.update(overrides, SDBZ_DISC_ROOT=str(disc),
                   SDBZ_SPEED_PERCENT=str(SPEEDS[self.speed.get()]))
        return ["cmd.exe", "/d", "/c", "run_ps2_game.bat", *args]

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
        except (OSError, ValueError) as error:
            self.status.set(str(error))
            return

        self.output.configure(state="normal")
        self.output.delete("1.0", tk.END)
        self.output.configure(state="disabled")
        self.play.state(["disabled"])
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
            if "live cache-miss: PC=" in chunk:
                self.status.set("Compiling missing EE code; game state paused")
            if "EE hot-expand: resumed guest pc=" in chunk:
                self.status.set("Guest execution resumed after live IR expansion")
            self.output.configure(state="normal")
            self.output.insert(tk.END, chunk)
            lines = int(self.output.index("end-1c").split(".")[0])
            if lines > 2000:
                self.output.delete("1.0", f"{lines - 2000}.0")
            self.output.see(tk.END)
            self.output.configure(state="disabled")

        result = self.process.poll()
        if chunk or result is None:
            self.after(150, self.read_output)
        else:
            self.log.close()
            self.log = None
            self.process = None
            self.embedded_running = False
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
