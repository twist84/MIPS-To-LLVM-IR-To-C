@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem Boot the repo's extracted PS2_GAME disc through the BIOS-free HLE path.
rem BOOT2 in SYSTEM.CNF chooses the executable; no region/ELF is hardcoded.
rem Never switch branches, fetch ROM0, or require an ISO.
cd /d "%~dp0"
if errorlevel 1 exit /b 1

rem Usage: run_ps2_game.bat [--full^|--quick] [--cross^|--neutral]
rem Full retail diagnostics use the usual local replay settings below.
rem --cross injects eight Cross polls starting at poll 200; --neutral
rem clears any pad injection left in the caller's environment.
rem Without either switch, retain explicitly configured pad variables.
set "REQUESTED_SESSION_MODE="
set "REQUESTED_PAD_PROFILE="
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
echo Usage: run_ps2_game.bat [--full or --quick] [--cross or --neutral]
exit /b 2

:launcher_options_ready
if /I "%REQUESTED_PAD_PROFILE%"=="cross" (
    set "SDBZ_PAD0_CROSS_FIRST_POLL=200"
    set "SDBZ_PAD0_CROSS_POLL_COUNT=8"
)
if /I "%REQUESTED_PAD_PROFILE%"=="neutral" (
    set "SDBZ_PAD0_CROSS_FIRST_POLL="
    set "SDBZ_PAD0_CROSS_POLL_COUNT="
)
set "CAPTURE_PROFILE=neutral"
if defined SDBZ_PAD0_CROSS_FIRST_POLL set "CAPTURE_PROFILE=pad-custom"
if defined SDBZ_PAD0_CROSS_POLL_COUNT set "CAPTURE_PROFILE=pad-custom"
if "%SDBZ_PAD0_CROSS_FIRST_POLL%"=="200" if "%SDBZ_PAD0_CROSS_POLL_COUNT%"=="8" set "CAPTURE_PROFILE=cross"

set "DISC_ROOT=%CD%\PS2_GAME"
set "WORK_DIR=build\ps2-game-hle"
if not defined EMIT_JOBS set "EMIT_JOBS=8"
if not defined SIF_POLL_LIMIT set "SIF_POLL_LIMIT=2048"
rem Bound each native auto-expansion session; both limits are configurable.
rem EXPANSION_PASSES: 1..64, number of bounded passes (default 8).
rem EXPANSION_PER_PASS: 1..128, newly emitted functions per pass (default 128).
rem Stop early at genuine guest blockers; never fabricate completion.
if not defined EXPANSION_PASSES set "EXPANSION_PASSES=8"
if not defined EXPANSION_PER_PASS set "EXPANSION_PER_PASS=128"
if not defined SDBZ_REPORT_SNAPSHOT_MS set "SDBZ_REPORT_SNAPSHOT_MS=2000"

where python >nul 2>nul
if errorlevel 1 (
    echo ERROR: Python was not found in PATH.
    exit /b 1
)
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
cmake --build build --config Release --parallel 8 --target ee_recomp ee_game_runner
if errorlevel 1 exit /b 1

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

rem This is the only fallback that can wipe and regenerate old guest IR.
rem A failed exact-cache validation AND failed safe relink are not permission
rem to delete saved translations. Require an explicit affirmative response
rem if ANY previously generated guest translation artifact still exists.
if not defined CACHE_FLAG (
    call :confirm_guest_cache_rebuild
    if errorlevel 1 exit /b 1
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
echo Dispatch budget: %MAX_DISPATCHES% per pass; timeout: %BOOT_TIMEOUT_SECONDS% seconds
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
    --emit-opt=0 ^
    --max-dispatches=%MAX_DISPATCHES% ^
    --loop-budget=1000000 ^
    --loop-yield-resumes=%LOOP_YIELD_RESUMES% ^
    --sif-poll-limit=%SIF_POLL_LIMIT% ^
    --timeout-seconds=%BOOT_TIMEOUT_SECONDS% %CACHE_FLAG% %SESSION_ARGS% %EE_WEBGPU_RUN_ARG% %EE_WEBGPU_WINDOW_ARG%
set "EXIT_CODE=%ERRORLEVEL%"
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

:confirm_guest_cache_rebuild
rem Fresh builds with no saved guest output need no confirmation.
rem Detect remnants even when translation-cache.json itself was removed.
set "SAVED_GUEST_TRANSLATION="
if exist "%WORK_DIR%\translation-cache.json" set "SAVED_GUEST_TRANSLATION=1"
if exist "%WORK_DIR%\ir\ee_functions.json" set "SAVED_GUEST_TRANSLATION=1"
if exist "%WORK_DIR%\ir\*.ll" set "SAVED_GUEST_TRANSLATION=1"
if exist "%WORK_DIR%\objects\objects.json" set "SAVED_GUEST_TRANSLATION=1"
if exist "%WORK_DIR%\objects\*.o" set "SAVED_GUEST_TRANSLATION=1"
if exist "%WORK_DIR%\game-translated.dll" set "SAVED_GUEST_TRANSLATION=1"
if not defined SAVED_GUEST_TRANSLATION exit /b 0

echo.
echo ================================================================
echo WARNING: The existing guest LLVM IR cache could not be reused.
echo Both exact cache validation and safe host-only relinking failed,
echo or saved translation metadata is missing.
echo.
echo A full rebuild may DELETE existing .ll modules and REPLACE the
echo translated game library, objects, and translation-cache.json in:
echo   "%WORK_DIR%"
echo.
echo Back up this directory before continuing if you need its contents.
echo Press Y to authorize the rebuild, or N to abort without rebuilding.
echo ================================================================
choice /C YN /N /M "Rebuild saved guest translations? [Y/N] "
rem CHOICE: Y=1; N=2; error/no-console=255. Anything but Y aborts.
if errorlevel 2 (
    echo Guest translation rebuild cancelled; saved IR was not regenerated.
    exit /b 1
)
if errorlevel 1 (
    echo Guest translation rebuild explicitly authorized.
    exit /b 0
)
echo ERROR: Cache rebuild confirmation was not received.
exit /b 1
