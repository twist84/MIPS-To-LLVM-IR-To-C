"""Option metadata for the launcher's three direct tools."""

from dataclasses import dataclass


@dataclass(frozen=True)
class Option:
    name: str
    help: str
    kind: str = "value"
    default: str = ""
    choices: tuple = ()

    @property
    def required(self):
        return self.kind in ("positional", "required-directory", "required-output")


ELF = Option("elf", "Path to the PS2 executable.", "positional")
WORK_DIR = Option("--work-dir", "Directory for the translation cache and session artifacts.", "directory", "build/game-run")
REPORT = Option("--report", "Write a JSON summary to this path.", "output")
GS_OPTIONS = (
    Option("--gs-snapshot", "Save the configured guest display buffer as a PPM.", "output"),
    Option("--gs-upload-snapshot", "Save the last completed GS upload as a PPM.", "output"),
    Option("--gs-webgpu-snapshot", "Save a native WebGPU raster as a PPM.", "output"),
    Option("--speed-percent", "WebGPU window pacing target. Zero is unlimited."),
    Option("--gs-webgpu-window", "Open the raylib/WebGPU game window on Windows.", "flag"),
)
LIMITS = (
    Option("--max-dispatches", "Maximum native function dispatches.", default="10000"),
    Option("--loop-budget", "Guest loop watchdog budget. Zero disables it.", default="0"),
    Option("--loop-yield-resumes", "Resume after watchdog yields. Requires a positive loop budget.", default="0"),
    Option("--sif-poll-limit", "Stop after repeated unchanged SIF SMFLAG reads. Zero disables this limit.", default="0"),
    Option("--entry", "Override the EE entry address, for example 0x00100008."),
    Option("--stack", "Initial EE stack pointer, for example 0x01fff000."),
    Option("--ee-clock", "Choose the EE compatibility clock mode.", default="reference-compat",
           choices=("reference-compat", "off")),
    Option("--kseg-ram", "Enable direct-mapped EE RAM aliases.", "flag"),
    Option("--hle-startup", "Handle supported startup calls in the host.", "flag"),
    Option("--memcard-discard-writes", "Keep memory card changes in memory for this replay.", "flag"),
    Option("--kernel-ram", "Load a captured 512 KiB EE kernel RAM image.", "file"),
)
DISC_OPTIONS = (
    Option("--disc-folder", "Mount the extracted disc folder for HLE.", "directory"),
    Option("--disc-image", "Attach a raw 2048-byte-sector ISO to CDVD.", "file"),
    Option("--iop-irx", "Load IRX modules in dependency order. Enter one path per line.", "repeat"),
    Option("--iop-start-all", "Start each loaded IRX. By default, only the last starts.", "flag"),
    Option("--iop-quantum", "Maximum R3000 instructions per SIF poll.", default="256"),
)
TRACE_OPTIONS = (
    Option("--trace", "Print guest PCs and dispatch results.", "flag"),
    Option("--trace-instructions", "Write a bounded native guest instruction trace.", "output"),
    Option("--trace-instructions-window", "Limit instruction tracing to START:END."),
    Option("--trace-entry", "Trace arguments and pointed-to words at this guest PC."),
    Option("--watch-word", "Report changes to the guest word at this address."),
    Option("--trace-v0", "Trace low-64-bit V0 changes within START:END."),
    Option("--trace-store", "Trace stores overlapping the guest word at this address."),
)
BUILD_OPTIONS = (
    Option("--build-dir", "Directory containing the built host tools.", "directory", "build"),
    Option("--config", "CMake build configuration.", default="Release",
           choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel")),
    Option("--functions", "Translate entry, all functions, or a positive function cap.", default="entry"),
    Option("--extra-functions", "Comma-separated guest PCs to translate."),
    Option("--auto-expand", "Maximum new functions (0–1024); pauses and resumes live by default.", default="0"),
    Option("--replay-after-expand", "Use legacy restart after each cache miss instead of live resume.", "flag"),
    Option("--live-full-relink", "Relink the full game library after each live cache miss instead of loading fast overlays.", "flag"),
    Option("--auto-expand-table", "Maximum sibling function pointers prefetched per replay.", default="24"),
    Option("--pdb", "Generate a Windows PDB with Clang/LLD.", "flag"),
    Option("--emit-opt", "LLVM native object optimization level.", default="0", choices=("0", "1", "2", "3")),
    Option("--emit-jobs", "Parallel LLVM object emission jobs.", default="2"),
    Option("--no-recompile", "Reuse compiled guest code without rebuilding it.", "flag"),
    Option("--relink-existing", "Relink the existing native objects.", "flag"),
    Option("--link-only", "Build the translated library without running the game.", "flag"),
)
TIMEOUT = Option("--timeout-seconds", "Hard wall-time limit for each replay.", default="10")
IRX_LIST = Option("--iop-irx-list", "Read ordered IRX paths from a UTF-8 file.", "file")
IRX_ROM = Option("--iop-rom0", "Boot a caller-supplied 4 MiB PS2 BIOS image.", "file")

TOOL_OPTIONS = {
    "Boot session": {
        "Session": (
            ELF,
            Option("--work-dir", WORK_DIR.help, "required-directory", "build/ps2-game-hle"),
            Option("--report", REPORT.help, "required-output", "build/ps2-game-hle/first-boot.json"),
            Option("--expansion-passes", "Maximum auto-expansion passes (1–64).", default="2"),
            Option("--expansion-per-pass", "Maximum new functions per pass (1–1024).", default="128"),
            Option("--replay-after-expand", "Legacy mode: restart the game after each cache miss rather than resuming its live state.", "flag"),
            Option("--live-full-relink", "Disable fast overlays; relink the entire translated library at every live miss.", "flag"),
            Option("--no-recompile", "Reuse compiled guest code without rebuilding it.", "flag"),
            Option("--quick-probe", "Use the last expansion pass as the final report. Skips traced replay.", "flag"),
            Option("--ll-progress", "Show the live LLVM IR file count on a terminal.", "flag"),
            Option("--driver", "Override the Python translation driver.", "file"),
        ),
        "Display": GS_OPTIONS,
        "Disc and IOP": DISC_OPTIONS + (
            Option("--disc-root", "Disc catalog root. Defaults to the ELF directory.", "directory"),
            Option("--no-iop-catalog", "Skip IOP disc catalog discovery.", "flag"),
            Option("--iop-auto-irx", "Resolve disc IRX names in provider-first order. Enter one per line.", "repeat"),
            IRX_LIST,
        ),
        "Runtime": LIMITS + (TIMEOUT,),
        "Translation": tuple(option for option in BUILD_OPTIONS if option.name not in
                             ("--auto-expand", "--no-recompile", "--link-only",
                              "--replay-after-expand", "--live-full-relink")),
        "Trace": TRACE_OPTIONS[1:],
    },
    "Game runner (Python)": {
        "Inputs": (ELF, WORK_DIR, REPORT, TIMEOUT),
        "Translation": BUILD_OPTIONS,
        "Runtime": LIMITS,
        "Disc and IOP": DISC_OPTIONS + (IRX_LIST,),
        "Display": GS_OPTIONS,
        "Trace": TRACE_OPTIONS,
    },
    "Native runner": {
        "Inputs": (ELF, Option("library", "Path to the linked native game library.", "positional"), REPORT),
        "Runtime": LIMITS + (
            Option("--hot-expand-dir", "Internal live cache-miss transport; requires a Python compiler supervisor.", "directory"),
        ),
        "Disc and IOP": DISC_OPTIONS + (IRX_ROM,),
        "Display": GS_OPTIONS + (
            Option("--gs-field-probe", "Write bounded GS field diagnostics as JSONL.", "output"),
        ),
        "Trace": TRACE_OPTIONS,
    },
}


def tool_arguments(tool, values, extra):
    argv = []
    legacy = [line for line in extra.splitlines() if line.strip()]
    for group in TOOL_OPTIONS[tool].values():
        for option in group:
            value = values.get(option.name, option.default)
            if option.kind == "flag":
                if value is True:
                    argv.append(option.name)
                continue
            if not value.strip():
                if option.required and not legacy:
                    raise ValueError(f"{option.name} is required for {tool}.")
                continue
            if option.kind == "positional":
                argv.append(value)
                continue
            items = value.splitlines() if option.kind == "repeat" else [value]
            argv.extend(f"{option.name}={item}" for item in items if item.strip())
    return argv + legacy
