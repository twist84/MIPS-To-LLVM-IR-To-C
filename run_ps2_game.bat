@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem Boot the repo's extracted PS2_GAME disc through the BIOS-free HLE path.
rem BOOT2 in SYSTEM.CNF chooses the executable; no region/ELF is hardcoded.
rem Never switch branches, fetch ROM0, or require an ISO.
cd /d "%~dp0"
if errorlevel 1 exit /b 1

rem Usage: run_ps2_game.bat [--full^|--quick^|--matched^|--recover-cache] [--cross^|--neutral]
rem --recover-cache reconstructs known IR without a game replay. Optional
rem --accept-new-ir explicitly permits changed lowering IR hashes; this never
rem deletes or overwrites the previous guest cache.
rem --replay-after-expand opts out of in-process live cache-miss resume.
rem --live-full-relink disables PR #152's fast overlay DLLs for live misses.
rem PRECOMPILE_ALL=0 disables cold whole-image precompilation.
rem --matched executes a CPU-only neutral/Cross pair with known cached IR,
rem never running guest function discovery or auto-expansion.
rem Full retail diagnostics use the usual local replay settings below.
rem --cross injects eight Cross polls starting at poll 200; --neutral
rem clears any pad injection left in the caller's environment.
rem Without either switch, retain explicitly configured pad variables.
set "REQUESTED_SESSION_MODE="
set "REQUESTED_PAD_PROFILE="
set "ACCEPT_NEW_IR="
set "CACHE_MISS_ARGS="
set "LIVE_RELINK_ARG="
:parse_launcher_options
if "%~1"=="" goto :launcher_options_ready
if /I "%~1"=="--full" (
    set "REQUESTED_SESSION_MODE=full"
    shift
    goto :parse_launcher_options
)
if /I "%~1"=="--quick" (
    set "REQUESTED_SESSION_MODE=quick"
    shift
    goto :parse_launcher_options
)
if /I "%~1"=="--matched" (
    set "REQUESTED_SESSION_MODE=matched"
    shift
    goto :parse_launcher_options
)
if /I "%~1"=="--recover-cache" (
    set "REQUESTED_SESSION_MODE=recover"
    shift
    goto :parse_launcher_options
)
if /I "%~1"=="--accept-new-ir" (
    set "ACCEPT_NEW_IR=--allow-ir-changes"
    shift
    goto :parse_launcher_options
)
if /I "%~1"=="--replay-after-expand" (
    set "CACHE_MISS_ARGS=--replay-after-expand"
    shift
    goto :parse_launcher_options
)
if /I "%~1"=="--live-full-relink" (
    set "LIVE_RELINK_ARG=--live-full-relink"
    shift
    goto :parse_launcher_options
)
if /I "%~1"=="--cross" (
    set "REQUESTED_PAD_PROFILE=cross"
    shift
    goto :parse_launcher_options
)
if /I "%~1"=="--neutral" (
    set "REQUESTED_PAD_PROFILE=neutral"
    shift
    goto :parse_launcher_options
)
echo ERROR: Unknown launcher option "%~1".
echo Usage: run_ps2_game.bat [--full or --quick or --matched or --recover-cache] [--cross or --neutral] [--accept-new-ir] [--replay-after-expand] [--live-full-relink]
exit /b 2

:launcher_options_ready
rem Optimized native object variants never replace the O0 baseline.
if not defined SDBZ_EE_OPT set "SDBZ_EE_OPT=0"
if not "%SDBZ_EE_OPT%"=="0" if not "%SDBZ_EE_OPT%"=="1" if not "%SDBZ_EE_OPT%"=="2" if not "%SDBZ_EE_OPT%"=="3" if not "%SDBZ_EE_OPT%"=="4" if not "%SDBZ_EE_OPT%"=="5" (
    echo ERROR: SDBZ_EE_OPT must be 0..5 ^(4=O2 IR + O2 backend, 5=host-native O2 backend^).
    exit /b 2
)
if not "%SDBZ_EE_OPT%"=="0" if /I not "%REQUESTED_SESSION_MODE%"=="full" (
    echo ERROR: Optimized EE cache experiments require --full.
    exit /b 2
)
if defined ACCEPT_NEW_IR if /I not "%REQUESTED_SESSION_MODE%"=="recover" (
    echo ERROR: --accept-new-ir requires --recover-cache.
    exit /b 2
)
if /I "%REQUESTED_SESSION_MODE%"=="recover" if defined REQUESTED_PAD_PROFILE (
    echo ERROR: Cache recovery does not run the game; omit --cross or --neutral.
    exit /b 2
)
if /I "%REQUESTED_SESSION_MODE%"=="matched" if defined REQUESTED_PAD_PROFILE (
    echo ERROR: --matched runs both profiles; omit --cross and --neutral.
    exit /b 2
)
if defined CACHE_MISS_ARGS if /I "%REQUESTED_SESSION_MODE%"=="recover" (
    echo ERROR: --replay-after-expand has no meaning during cache recovery.
    exit /b 2
)
if defined CACHE_MISS_ARGS if /I "%REQUESTED_SESSION_MODE%"=="matched" (
    echo ERROR: --replay-after-expand has no meaning during matched replays.
    exit /b 2
)
if defined LIVE_RELINK_ARG if /I "%REQUESTED_SESSION_MODE%"=="recover" (
    echo ERROR: --live-full-relink has no meaning during cache recovery.
    exit /b 2
)
if defined LIVE_RELINK_ARG if /I "%REQUESTED_SESSION_MODE%"=="matched" (
    echo ERROR: --live-full-relink has no meaning during matched replays.
    exit /b 2
)
if defined LIVE_RELINK_ARG if defined CACHE_MISS_ARGS (
    echo ERROR: --live-full-relink requires live cache-miss resume.
    exit /b 2
)
if /I "%REQUESTED_PAD_PROFILE%"=="cross" (
    set "SDBZ_PAD0_SCRIPT="
    set "SDBZ_PAD0_CROSS_FIRST_POLL=200"
    set "SDBZ_PAD0_CROSS_POLL_COUNT=8"
)
if /I "%REQUESTED_PAD_PROFILE%"=="neutral" (
    set "SDBZ_PAD0_SCRIPT="
    set "SDBZ_PAD0_CROSS_FIRST_POLL="
    set "SDBZ_PAD0_CROSS_POLL_COUNT="
)
if /I "%REQUESTED_SESSION_MODE%"=="matched" set "SDBZ_PAD0_SCRIPT="
set "CAPTURE_PROFILE=neutral"
if defined SDBZ_PAD0_CROSS_FIRST_POLL set "CAPTURE_PROFILE=pad-custom"
if defined SDBZ_PAD0_CROSS_POLL_COUNT set "CAPTURE_PROFILE=pad-custom"
if "%SDBZ_PAD0_CROSS_FIRST_POLL%"=="200" if "%SDBZ_PAD0_CROSS_POLL_COUNT%"=="8" set "CAPTURE_PROFILE=cross"
if defined SDBZ_PAD0_SCRIPT set "CAPTURE_PROFILE=pad-script"

set "DISC_ROOT=%CD%\PS2_GAME"
if defined SDBZ_DISC_ROOT set "DISC_ROOT=%SDBZ_DISC_ROOT%"
set "BASE_WORK_DIR=build\ps2-game-hle"
set "WORK_DIR=%BASE_WORK_DIR%"
if not defined EMIT_JOBS set "EMIT_JOBS=%NUMBER_OF_PROCESSORS%"
if not defined EMIT_JOBS set "EMIT_JOBS=8"
rem Cold builds translate every statically discovered EE function once
rem (about two minutes with parallel object emission) instead of pausing the
rem guest for a ~3 s compile and relink at each cache miss. Live cache-miss
rem expansion still covers PCs that static discovery missed.
rem PRECOMPILE_ALL=0 restores the entry-only cold start.
if not defined PRECOMPILE_ALL set "PRECOMPILE_ALL=1"
rem Pure guest poll loops (e.g. the SDBZ VSync wait) run one real iteration
rem per native slice; the runner charges the remaining budgeted iterations.
rem Guest-visible results are identical. SDBZ_IDLE_SKIP=0 disables it.
if not defined SDBZ_IDLE_SKIP set "SDBZ_IDLE_SKIP=1"
if not defined SIF_POLL_LIMIT set "SIF_POLL_LIMIT=2048"
rem Bound each native auto-expansion session; both limits are configurable.
rem EXPANSION_PASSES: 1..64, number of bounded passes (default 8).
rem EXPANSION_PER_PASS: 1..1024, newly emitted functions per pass (default 128).
rem Stop early at genuine guest blockers; never fabricate completion.
if not defined EXPANSION_PASSES set "EXPANSION_PASSES=8"
if not defined EXPANSION_PER_PASS set "EXPANSION_PER_PASS=128"
if not defined SDBZ_REPORT_SNAPSHOT_MS set "SDBZ_REPORT_SNAPSHOT_MS=2000"

where python >nul 2>nul
if errorlevel 1 (
    echo ERROR: Python was not found in PATH.
    exit /b 1
)
rem A recovered cache is selected by an atomically written pointer, checked
rem against the safe sibling namespace. Never trust the raw pointer file.
for /f "usebackq delims=" %%I in (`python "tools\recover_translation_cache.py" --base-work-dir "%CD%\%BASE_WORK_DIR%" --print-active`) do set "WORK_DIR=%%I"
set "GAME_ELF="
for /f "usebackq delims=" %%I in (`python "tools\resolve_ps2_boot.py" "%DISC_ROOT%"`) do set "GAME_ELF=%%I"
if not defined GAME_ELF (
    echo ERROR: Could not resolve BOOT2 from PS2_GAME\SYSTEM.CNF.
    exit /b 1
)
if not exist "%GAME_ELF%" (
    echo ERROR: BOOT2 executable does not exist: "%GAME_ELF%"
    exit /b 1
)
echo Disc folder: "%DISC_ROOT%"
echo Boot ELF: "%GAME_ELF%"
if /I "%PS2_GAME_RESOLVE_ONLY%"=="1" exit /b 0

rem The CMake/LLVM toolchain on Windows uses the Visual Studio x64 SDK.
if defined VCToolsInstallDir goto :toolchain_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio vswhere.exe not found.
    exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
    echo ERROR: Visual Studio C++ build tools not found.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

:toolchain_ready
if not defined CXX (
    if exist "C:\Program Files\LLVM\bin\clang++.exe" (
        set "CXX=C:\Program Files\LLVM\bin\clang++.exe"
    )
)
rem CMake 4.x refuses to configure when C/ASM resolve to MSVC cl.exe
rem while CXX is clang++ ("mixes Clang and MSVC"). Pair CC with clang.
if not defined CC (
    if exist "C:\Program Files\LLVM\bin\clang.exe" (
        set "CC=C:\Program Files\LLVM\bin\clang.exe"
    )
)
where cmake >nul 2>nul
if errorlevel 1 (
    echo ERROR: CMake was not found in PATH.
    exit /b 1
)

rem Default full runs to the WebGPU snapshot and native Win32 window
rem used in the local graphics investigations. Preserve explicit 0/1
rem overrides, and keep --quick headless regardless of prior settings.
rem Only the final traced replay opens a window; expansion is headless.
if /I "%REQUESTED_SESSION_MODE%"=="quick" (
    set "EE_WEBGPU_SNAPSHOT=0"
    set "EE_WEBGPU_WINDOW=0"
)
if /I "%REQUESTED_SESSION_MODE%"=="matched" (
    rem Force CPU-only; do not download/configure WebGPU for paired replays.
    set "EE_WEBGPU_SNAPSHOT=0"
    set "EE_WEBGPU_WINDOW=0"
)
if /I "%REQUESTED_SESSION_MODE%"=="recover" (
    rem Cache recovery never runs the game or starts WebGPU.
    set "EE_WEBGPU_SNAPSHOT=0"
    set "EE_WEBGPU_WINDOW=0"
)
if /I "%REQUESTED_SESSION_MODE%"=="full" (
    if not defined EE_WEBGPU_SNAPSHOT set "EE_WEBGPU_SNAPSHOT=1"
    if not defined EE_WEBGPU_WINDOW set "EE_WEBGPU_WINDOW=1"
)
set "EE_WEBGPU_MODE=OFF"
set "EE_RAYLIB_MODE=OFF"
set "EE_WEBGPU_RUN_ARG="
set "EE_WEBGPU_WINDOW_ARG="
if /I "%EE_WEBGPU_SNAPSHOT%"=="1" (
    set "EE_WEBGPU_MODE=ON"
    set "EE_WEBGPU_RUN_ARG=--gs-webgpu-snapshot=%WORK_DIR%\gs-webgpu.ppm"
)
if /I "%EE_WEBGPU_WINDOW%"=="1" (
    set "EE_WEBGPU_MODE=ON"
    set "EE_RAYLIB_MODE=ON"
    set "EE_WEBGPU_WINDOW_ARG=--gs-webgpu-window"
)
rem Reuse the pinned MSVC SDK in build\_deps or download it once.
rem Set this outside the block: cmd.exe expands %%VARS%% before executing
rem a parenthesized block, so setting and reading inside it passes an empty root.
if "%EE_WEBGPU_MODE%"=="ON" if not defined EE_WEBGPU_ROOT set "EE_WEBGPU_ROOT=%CD%\build\_deps\wgpu-native"
if "%EE_WEBGPU_MODE%"=="ON" (
    python "tools\ensure_wgpu_native.py" --root "%EE_WEBGPU_ROOT%"
    if errorlevel 1 exit /b 1
)
echo === Configuring native host tools ===
if exist "build\CMakeCache.txt" (
    cmake -S . -B build -DEE_ENABLE_WEBGPU=%EE_WEBGPU_MODE% -DEE_ENABLE_RAYLIB_WINDOW=%EE_RAYLIB_MODE% "-DEE_WEBGPU_ROOT=%EE_WEBGPU_ROOT%"
) else (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DEE_ENABLE_WEBGPU=%EE_WEBGPU_MODE% -DEE_ENABLE_RAYLIB_WINDOW=%EE_RAYLIB_MODE% "-DEE_WEBGPU_ROOT=%EE_WEBGPU_ROOT%"
)
if errorlevel 1 exit /b 1

echo === Building native EE runner and recompiler ===
cmake --build build --config Release --parallel 8 --target ee_recomp ee_game_runner vu1_recomp
if errorlevel 1 exit /b 1
if defined SDBZ_IOP_AOT_CAPTURE (
    cmake --build build --config Release --parallel 8 --target iop_recomp
    if errorlevel 1 exit /b 1
)

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if errorlevel 1 exit /b 1

rem Git checkout may rebuild host tools without changing guest LLVM IR.
rem Try exact reuse first; if host inputs changed, relink existing guest objects.
rem --quick forces the non-traced probe even if no guest cache survives.
rem Without --quick, WebGPU requests still require full replay snapshots.
rem Only rebuild guest IR when preserved provenance cannot be verified.
set "CACHE_FLAG="
if exist "%WORK_DIR%\translation-cache.json" (
    echo === Validating saved EE translation ===
    python tools\run_game.py "%GAME_ELF%" ^
        --build-dir build --config Release ^
        --work-dir "%WORK_DIR%" --emit-opt=0 ^
        --no-recompile --link-only >nul 2>nul
    if not errorlevel 1 (
        echo EE translation cache valid; reusing existing functions.
        set "CACHE_FLAG=--no-recompile"
    ) else (
        echo Native cache changed; checking preserved guest IR and objects...
        python tools\run_game.py "%GAME_ELF%" ^
            --build-dir build --config Release ^
            --work-dir "%WORK_DIR%" --emit-opt=0 ^
            --relink-existing --link-only
        if not errorlevel 1 (
            echo Preserved guest IR and objects; relinked host library.
            set "CACHE_FLAG=--no-recompile"
        ) else (
            echo Guest translation provenance changed; full IR rebuild required.
        )
    )
)

rem Cache recovery is reconstruction-only: no guest replay or discovery.
rem Do not enter the interactive destructive rebuild path on failure.
if /I "%REQUESTED_SESSION_MODE%"=="recover" (
    if defined CACHE_FLAG (
        echo Translation cache is already valid; no recovery needed.
        exit /b 0
    )
    echo === Reconstructing known EE functions in a new cache directory ===
    python tools\recover_translation_cache.py --elf "%GAME_ELF%" ^
        --build-dir build --config Release ^
        --base-work-dir "%CD%\%BASE_WORK_DIR%" ^
        --emit-jobs=%EMIT_JOBS% %ACCEPT_NEW_IR%
    exit /b
)

rem Strict unattended recovery: hashes AND lowering sources must match.
rem The user's first post-update invalidation may predate snapshot support;
rem the existing cache record itself is also a known-entry manifest.
rem Any changed generator or differing IR remains an explicit user decision.
set "HAVE_KNOWN_MANIFEST="
if exist "build\ee-known-manifests\*.json" set "HAVE_KNOWN_MANIFEST=1"
if exist "%WORK_DIR%\translation-cache.json" set "HAVE_KNOWN_MANIFEST=1"
if exist "%BASE_WORK_DIR%\translation-cache.json" set "HAVE_KNOWN_MANIFEST=1"
if not defined CACHE_FLAG (
    if defined HAVE_KNOWN_MANIFEST (
        echo === Attempting strict, non-destructive known-IR recovery ===
        python tools\recover_translation_cache.py --elf "%GAME_ELF%" ^
            --build-dir build --config Release ^
            --base-work-dir "%CD%\%BASE_WORK_DIR%" ^
            --emit-jobs=%EMIT_JOBS%
        if not errorlevel 1 (
            for /f "usebackq delims=" %%I in (`python "tools\recover_translation_cache.py" --base-work-dir "%CD%\%BASE_WORK_DIR%" --print-active`) do set "WORK_DIR=%%I"
            set "CACHE_FLAG=--no-recompile"
            echo Recovered translated library validated; guest discovery skipped.
        )
    )
)

rem A paired experiment MUST use the existing guest IR, never prompt for
rem a destructive rebuild or replay to discover additional functions.
rem Resolve defaults before the parenthesized command expands variables.
if /I "%REQUESTED_SESSION_MODE%"=="matched" if not defined MAX_DISPATCHES set "MAX_DISPATCHES=6000000"
if /I "%REQUESTED_SESSION_MODE%"=="matched" if not defined MAX_REPLAY_DISPATCHES set "MAX_REPLAY_DISPATCHES=%MAX_DISPATCHES%"
if /I "%REQUESTED_SESSION_MODE%"=="matched" if not defined BOOT_TIMEOUT_SECONDS set "BOOT_TIMEOUT_SECONDS=600"
if /I "%REQUESTED_SESSION_MODE%"=="matched" (
    if not defined CACHE_FLAG (
        echo ERROR: Known-function replay requires a valid guest translation cache.
        echo Preserve existing IR; use --reconstruct with tools\run_known_replays.py
        echo in a separate work directory if you need to rebuild from a manifest.
        exit /b 1
    )
    echo === Running matched CPU-only known-IR neutral/Cross replays ===
    python tools\run_known_replays.py ^
        --disc-root "%DISC_ROOT%" ^
        --build-dir build --config Release ^
        --work-dir "%WORK_DIR%" ^
        --emit-jobs=%EMIT_JOBS% ^
        --max-dispatches=%MAX_REPLAY_DISPATCHES% ^
        --timeout-seconds=%BOOT_TIMEOUT_SECONDS%
    rem EXIT /B without a number preserves Python's actual ERRORLEVEL.
    rem Percent-expanded %%ERRORLEVEL%% inside this block would be stale.
    exit /b
)

rem Normal boots may regenerate guest IR automatically when exact reuse,
rem host-only relinking and strict non-destructive recovery all fail.
rem --matched and --recover-cache already take their separate paths above.
if not defined CACHE_FLAG (
    echo Guest translation cache is not reusable; regenerating IR automatically.
)

rem Fork verified guest IR into an independent optimization cache.
if not "%SDBZ_EE_OPT%"=="0" (
    if not defined CACHE_FLAG (
        echo ERROR: Optimized EE replay requires a verified O0 cache.
        exit /b 1
    )
    if "%SDBZ_EE_OPT%"=="4" (
        rem Refresh the verified O2 cache and seed the pipeline variant from
        rem its full discovery map. Never replace the O0 or O2 IR/objects.
        python tools\build_optimized_cache.py "%GAME_ELF%" ^
            --build-dir build --config Release ^
            --source-work-dir "%WORK_DIR%" --work-dir "%WORK_DIR%-O2" ^
            --emit-opt=2 --emit-jobs=%EMIT_JOBS%
        if errorlevel 1 exit /b 1
        python tools\build_optimized_cache.py "%GAME_ELF%" ^
            --build-dir build --config Release ^
            --source-work-dir "%WORK_DIR%-O2" --source-opt=2 ^
            --work-dir "%WORK_DIR%-IR-O2" --emit-opt=4 --emit-jobs=%EMIT_JOBS%
        if errorlevel 1 exit /b 1
        set "WORK_DIR=%WORK_DIR%-IR-O2"
    ) else if "%SDBZ_EE_OPT%"=="5" (
        rem Isolate host CPU targeting from the IR optimization experiment.
        python tools\build_optimized_cache.py "%GAME_ELF%" ^
            --build-dir build --config Release ^
            --source-work-dir "%WORK_DIR%" --work-dir "%WORK_DIR%-O2" ^
            --emit-opt=2 --emit-jobs=%EMIT_JOBS%
        if errorlevel 1 exit /b 1
        python tools\build_optimized_cache.py "%GAME_ELF%" ^
            --build-dir build --config Release ^
            --source-work-dir "%WORK_DIR%-O2" --source-opt=2 ^
            --work-dir "%WORK_DIR%-O2-native" --emit-opt=5 --emit-jobs=%EMIT_JOBS%
        if errorlevel 1 exit /b 1
        set "WORK_DIR=%WORK_DIR%-O2-native"
    ) else (
        python tools\build_optimized_cache.py "%GAME_ELF%" ^
            --build-dir build --config Release ^
            --source-work-dir "%WORK_DIR%" --work-dir "%WORK_DIR%-O%SDBZ_EE_OPT%" ^
            --emit-opt=%SDBZ_EE_OPT% --emit-jobs=%EMIT_JOBS%
        if errorlevel 1 exit /b 1
        set "WORK_DIR=%WORK_DIR%-O%SDBZ_EE_OPT%"
    )
)
if /I "%EE_WEBGPU_SNAPSHOT%"=="1" set "EE_WEBGPU_RUN_ARG=--gs-webgpu-snapshot=%WORK_DIR%\gs-webgpu.ppm"
if "%SDBZ_EE_OPT%"=="4" (
    echo EE native object optimization: O2 IR pipeline + O2 backend
) else if "%SDBZ_EE_OPT%"=="5" (
    echo EE native object optimization: host-native O2 backend
) else (
    echo EE native object optimization: O%SDBZ_EE_OPT%
)

echo === Starting extracted-disc HLE boot session ===
set "SESSION_ARGS=--quick-probe --no-iop-catalog"
set "REPORT_NAME=quick-probe.json"
set "DEFAULT_MAX_DISPATCHES=100000"
if not defined CACHE_FLAG set "SESSION_ARGS="
if /I "%REQUESTED_SESSION_MODE%"=="quick" set "SESSION_ARGS=--quick-probe --no-iop-catalog"
if /I "%REQUESTED_SESSION_MODE%"=="full" set "SESSION_ARGS="
if "%EE_WEBGPU_MODE%"=="ON" set "SESSION_ARGS="
if defined SESSION_ARGS (
    echo Using the cached HLE quick probe.
    echo Boot log: "%WORK_DIR%\quick-probe.log"
) else (
    set "REPORT_NAME=first-boot.json"
    set "DEFAULT_MAX_DISPATCHES=6000000"
    set "SESSION_ARGS=--gs-snapshot=%WORK_DIR%\gs-display.ppm --gs-upload-snapshot=%WORK_DIR%\gs-upload.ppm"
    echo Using the full traced HLE boot session.
    echo Boot log: "%WORK_DIR%\boot.log"
    echo Expansion log: "%WORK_DIR%\expansion.log"
    echo Guest display VRAM if configured: "%WORK_DIR%\gs-display.ppm"
    echo GS texture upload if complete: "%WORK_DIR%\gs-upload.ppm"
)
rem Full replays have millions of dispatch boundaries. Retain every
rem actual dispatch and the complete JSON diagnostic summaries while
rem sampling *only* ordinary verbose stdout lines. Set
rem SDBZ_DISPATCH_TRACE_EVERY=1 for the historical full text trace.
rem The environment is host-only and never fingerprints guest LLVM IR.
if not defined SDBZ_DISPATCH_TRACE_EVERY set "SDBZ_DISPATCH_TRACE_EVERY=1024"
if /I "%REPORT_NAME%"=="first-boot.json" echo Verbose dispatch trace: first 64, each %SDBZ_DISPATCH_TRACE_EVERY%, and exceptional transitions
rem Quick probes retain their short limits. Full retail diagnostics use
rem the observed 6M dispatch / 600s budget unless explicitly overridden.
if not defined MAX_DISPATCHES set "MAX_DISPATCHES=%DEFAULT_MAX_DISPATCHES%"
if not defined MAX_EXPANSION_DISPATCHES set "MAX_EXPANSION_DISPATCHES=%MAX_DISPATCHES%"
if not defined MAX_REPLAY_DISPATCHES set "MAX_REPLAY_DISPATCHES=%MAX_DISPATCHES%"
if not defined BOOT_TIMEOUT_SECONDS (
    if /I "%REPORT_NAME%"=="first-boot.json" (
        set "BOOT_TIMEOUT_SECONDS=600"
    ) else (
        set "BOOT_TIMEOUT_SECONDS=120"
    )
)
rem Preserve the 1000000-instruction native watchdog. Bounded resumes
rem reset only after genuine guest GS fields, IOP RPC commits or GIF writes,
rem not on arbitrary dispatches, guest instructions or host time.
rem Use LOOP_YIELD_RESUMES=0 to stop on the first native loop watchdog.
if not defined LOOP_YIELD_RESUMES set "LOOP_YIELD_RESUMES=16"
echo Native loop watchdog: 1000000 instructions per native slice, %LOOP_YIELD_RESUMES% resumes between real GS/GIF/IOP progress
echo Expansion budget: %EXPANSION_PASSES% passes, %EXPANSION_PER_PASS% new functions per pass
echo Dispatch budgets: %MAX_EXPANSION_DISPATCHES% per expansion pass, %MAX_REPLAY_DISPATCHES% final replay; timeout: %BOOT_TIMEOUT_SECONDS% seconds
echo Report: "%WORK_DIR%\%REPORT_NAME%"
echo Controller profile: %CAPTURE_PROFILE%
if defined SDBZ_PAD0_CROSS_FIRST_POLL echo Injected Cross: first poll %SDBZ_PAD0_CROSS_FIRST_POLL%, count %SDBZ_PAD0_CROSS_POLL_COUNT%
if /I "%EE_WEBGPU_SNAPSHOT%"=="1" echo Native WebGPU raster: "%WORK_DIR%\gs-webgpu.ppm"
if /I "%EE_WEBGPU_WINDOW%"=="1" echo Native WebGPU presentation: raylib Win32 window in final replay
rem Clear only presentation outputs before a full replay, so that a
rem cancelled run cannot accidentally archive old GS images as new ones.
rem NEVER remove the guest IR, translated DLL, or object cache here.
if /I "%REPORT_NAME%"=="first-boot.json" (
    for %%F in (gs-display.ppm gs-upload.ppm gs-webgpu.ppm) do (
        if exist "%WORK_DIR%\%%F" del /q "%WORK_DIR%\%%F"
    )
)

rem Only a cold translation (no reusable cache) honors --functions=all;
rem cached passes and the final replay always run with --no-recompile.
set "FUNCTIONS_ARG="
if not defined CACHE_FLAG if not "%PRECOMPILE_ALL%"=="0" set "FUNCTIONS_ARG=--functions=all"
if defined FUNCTIONS_ARG echo Cold translation: precompiling all statically discovered EE functions
if defined LIVE_RELINK_ARG echo Live cache misses: legacy full-library relink
if not defined LIVE_RELINK_ARG if not defined CACHE_MISS_ARGS echo Live cache misses: fast overlay DLLs

rem Optional guest memory traces. These are host-only and never repair
rem invalid pointers or alter emulated guest instructions.
set "GUEST_TRACE_ARGS="
if defined SDBZ_WATCH_WORD set "GUEST_TRACE_ARGS=%GUEST_TRACE_ARGS% --watch-word=%SDBZ_WATCH_WORD%"
if defined SDBZ_TRACE_STORE set "GUEST_TRACE_ARGS=%GUEST_TRACE_ARGS% --trace-store=%SDBZ_TRACE_STORE%"
if defined SDBZ_TRACE_V0 set "GUEST_TRACE_ARGS=%GUEST_TRACE_ARGS% --trace-v0=%SDBZ_TRACE_V0%"

rem Display live LLVM IR file count and percentage in both quick and full
rem sessions; the orchestrator keeps in-place updates out of boot logs.
python tools\run_boot_session.py "%GAME_ELF%" ^
    --ll-progress ^
    --disc-root "%DISC_ROOT%" ^
    --disc-folder "%DISC_ROOT%" ^
    --build-dir build ^
    --config Release ^
    --work-dir "%WORK_DIR%" ^
    --report "%WORK_DIR%\%REPORT_NAME%" ^
    --expansion-passes=%EXPANSION_PASSES% ^
    --expansion-per-pass=%EXPANSION_PER_PASS% ^
    --hle-startup ^
    --kseg-ram ^
    --emit-jobs=%EMIT_JOBS% ^
    --emit-opt=%SDBZ_EE_OPT% ^
    --max-dispatches=%MAX_DISPATCHES% ^
    --max-expansion-dispatches=%MAX_EXPANSION_DISPATCHES% ^
    --max-replay-dispatches=%MAX_REPLAY_DISPATCHES% ^
    --loop-budget=1000000 ^
    --loop-yield-resumes=%LOOP_YIELD_RESUMES% ^
    --sif-poll-limit=%SIF_POLL_LIMIT% ^
    --timeout-seconds=%BOOT_TIMEOUT_SECONDS% %CACHE_FLAG% %FUNCTIONS_ARG% %SESSION_ARGS% %CACHE_MISS_ARGS% %LIVE_RELINK_ARG% %GUEST_TRACE_ARGS% %EE_WEBGPU_RUN_ARG% %EE_WEBGPU_WINDOW_ARG%
set "EXIT_CODE=%ERRORLEVEL%"
rem First-run GUI AOT bootstrap: the interpreter has now captured linked IRX.
rem Build only after the runner exits (no loaded DLLs to replace). Preserve
rem the session result even if optional AOT compilation fails.
if defined SDBZ_IOP_AOT_BUILD_OUT if defined SDBZ_IOP_AOT_CAPTURE (
    echo === Building captured IOP IRX module libraries ===
    python tools\build_iop_aot.py --build-dir build --capture "%SDBZ_IOP_AOT_CAPTURE%" --out "%SDBZ_IOP_AOT_BUILD_OUT%" --bootstrap
    if errorlevel 1 (
        echo WARNING: IOP AOT build failed; captures retained for retry.
    ) else (
        echo IOP AOT libraries ready for the next launch.
    )
)
rem Save a unique upload-ready archive after every full replay, even
rem when the guest exits at its dispatch budget or is cancelled.
rem Keep the original native exit code; archive failure is non-fatal.
if /I "%REPORT_NAME%"=="first-boot.json" call :archive_full_run
echo.
echo Native HLE boot session exit code: %EXIT_CODE%
echo Logs are in "%WORK_DIR%".
exit /b %EXIT_CODE%

:archive_full_run
rem Match the files formerly selected by the manual PowerShell command.
rem Never include guest .ll/.o files in these small diagnostic archives.
rem Distinct neutral/Cross names prevent overwriting experiment evidence.
powershell -NoProfile -Command "$files = @(Get-ChildItem -LiteralPath '%WORK_DIR%' -File | Where-Object { $_.Name -in @('boot.log','expansion.log','first-boot.json','translation-cache.json','iop-disc-catalog.json','gs-display.ppm','gs-webgpu.ppm','gs-upload.ppm') }); if ($files.Count -gt 0) { $archive = 'sdbz-graphics-pr148-%CAPTURE_PROFILE%-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff') + '.zip'; Compress-Archive -LiteralPath $files.FullName -DestinationPath $archive -ErrorAction Stop; Write-Output ('Diagnostic archive: ' + $archive) } else { Write-Output 'No diagnostic files to archive.' }"
if errorlevel 1 (
    echo WARNING: Diagnostic archive failed; original logs remain in "%WORK_DIR%".
    exit /b 1
)
exit /b 0
