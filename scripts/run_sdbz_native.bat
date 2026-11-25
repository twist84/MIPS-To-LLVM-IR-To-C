@echo off
setlocal

rem Build and run the local Super Dragon Ball Z USA ELF.
rem Real-game execution happens only on this Windows PC.
cd /d "%~dp0.."
if errorlevel 1 goto :failed

set "ELF=PS2_GAME\SLUS_214.42"
set "WORK_DIR=build\sdbz"
rem Parallel LLVM object emission (tools/run_game.py allows 1..32).
rem Default to 8 workers; callers can override with set EMIT_JOBS=16.
rem Auto-expansion usually emits one module at a time, so this mostly
rem benefits cache rebuilds and bulk translation.
if not defined EMIT_JOBS set "EMIT_JOBS=8"
echo LLVM emit workers: %EMIT_JOBS%
rem Stop a known no-IOP SIF busy wait instead of tracing 100000
rem identical kernel calls; 0 disables this diagnostic for comparisons.
if not defined SIF_POLL_LIMIT set "SIF_POLL_LIMIT=256"
echo SIF SMFLAG poll diagnostic limit: %SIF_POLL_LIMIT%

rem IOP IRX execution is explicitly opt-in. A single IRX can be
rem specified via IOP_IRX. For dependencies, use IOP_IRX_LIST: a UTF-8
rem manifest with provider paths first and the entry module last.
rem Unlisted kernel HLE services / accurate EE-IOP timing are not provided.
set "IOP_IRX_FLAGS="
if not defined IOP_QUANTUM set "IOP_QUANTUM=256"
if defined IOP_IRX if defined IOP_IRX_LIST (
    echo ERROR: IOP_IRX and IOP_IRX_LIST cannot both be set.
    goto :failed
)
if defined IOP_IRX (
    if not exist "%IOP_IRX%" (
        echo Missing IOP IRX: "%IOP_IRX%"
        goto :failed
    )
    set IOP_IRX_FLAGS=--iop-irx="%IOP_IRX%" --iop-quantum=%IOP_QUANTUM%
    echo IOP IRX execution enabled: "%IOP_IRX%" quantum=%IOP_QUANTUM%
)
if defined IOP_IRX_LIST (
    if not exist "%IOP_IRX_LIST%" (
        echo Missing IOP module manifest: "%IOP_IRX_LIST%"
        goto :failed
    )
    set IOP_IRX_FLAGS=--iop-irx-list="%IOP_IRX_LIST%" --iop-quantum=%IOP_QUANTUM%
    echo IOP module list: "%IOP_IRX_LIST%"
)
if "%IOP_START_ALL%"=="1" (
    if not defined IOP_IRX_FLAGS (
        echo ERROR: IOP_START_ALL requires IOP_IRX or IOP_IRX_LIST.
        goto :failed
    )
    set "IOP_IRX_FLAGS=%IOP_IRX_FLAGS% --iop-start-all"
    echo IOP start mode: all listed modules
)

if not exist "%ELF%" (
    echo Missing local game ELF: "%ELF%"
    goto :failed
)
if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if errorlevel 1 goto :failed

echo === Updating repository ===
git switch main
if errorlevel 1 goto :failed
git pull --ff-only
if errorlevel 1 goto :failed

rem Use the newest stable official OpenPS2BIOS 4 MiB release by default.
rem A caller-provided SDBZ_IOP_ROM0 always wins (and is never replaced).
rem Explicit IRX bootstrap remains a separate, opt-in compatibility mode.
rem Verify the upstream checksum and cache the last verified release for
rem offline reuse; do not run an unchecked or partial BIOS image.
if not defined SDBZ_IOP_ROM0 if not defined IOP_IRX if not defined IOP_IRX_LIST (
    echo === Resolving latest official OpenPS2BIOS ROM0 ===
    python tools\ensure_openps2bios.py --cache-dir "build\openps2bios"
    if errorlevel 1 goto :failed
    set "SDBZ_IOP_ROM0=%CD%\build\openps2bios\rom0.bin"
)
if defined SDBZ_IOP_ROM0 (
    if not exist "%SDBZ_IOP_ROM0%" (
        echo ERROR: SDBZ_IOP_ROM0 does not exist: "%SDBZ_IOP_ROM0%"
        goto :failed
    )
    echo IOP ROM0 image: "%SDBZ_IOP_ROM0%"
) else (
    echo IOP ROM0 automatic bootstrap skipped: explicit IRX mode
)


rem Identify candidate IOP module path strings from this *local* game ELF.
rem This is static evidence only; SIF LOADFILE RPC events will still need
rem to be traced before declaring a genuine module load sequence.
if not defined SDBZ_IOP_DISCOVERY set "SDBZ_IOP_DISCOVERY=1"
if "%SDBZ_IOP_DISCOVERY%"=="1" (
    echo === Discovering static IOP IRX path candidates from the US ELF ===
    python tools\analyze_iop_module_paths.py "%ELF%" ^
        --json "%WORK_DIR%\iop-module-paths.json"
    if errorlevel 1 goto :failed
)

rem CMake may have cached cl.exe even if the batch was opened in a
rem regular console with no MSVC C++ headers configured. Explicitly load
rem the VS x64 developer environment before compiling.
echo === Initializing Visual Studio x64 build environment ===
set "VS_DEV_CMD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%I\Common7\Tools\VsDevCmd.bat" (
            set "VS_DEV_CMD=%%I\Common7\Tools\VsDevCmd.bat"
        )
    )
)

rem Fallback for machines without vswhere.
if not defined VS_DEV_CMD (
    for %%V in (18 2026 2022 2019) do (
        for %%E in (Community Professional Enterprise BuildTools) do (
            if not defined VS_DEV_CMD (
                if exist "%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\Common7\Tools\VsDevCmd.bat" (
                    set "VS_DEV_CMD=%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\Common7\Tools\VsDevCmd.bat"
                )
            )
        )
    )
)

if not defined VS_DEV_CMD (
    echo ERROR: Visual Studio C++ build tools were not found.
    echo Install the "Desktop development with C++" workload in Visual Studio Installer.
    goto :failed
)
call "%VS_DEV_CMD%" -arch=amd64 -host_arch=amd64 -no_logo
if errorlevel 1 goto :failed

if not defined VCToolsInstallDir (
    echo ERROR: The MSVC developer environment was not initialized.
    goto :failed
)
if not exist "%VCToolsInstallDir%include\cstdint" (
    echo ERROR: MSVC C++ headers are missing: "%VCToolsInstallDir%include\cstdint"
    echo Install the MSVC x64 compiler and C++ libraries in Visual Studio Installer.
    goto :failed
)

echo === Building native tools ===
cmake --build build --config Release --parallel 8 --target ee_recomp ee_game_runner
if errorlevel 1 goto :failed

rem The guest DLL linker uses CXX; use installed LLVM clang++ when available.
if not defined CXX (
    if exist "C:\Program Files\LLVM\bin\clang++.exe" (
        set "CXX=C:\Program Files\LLVM\bin\clang++.exe"
    )
)

rem Reuse the exact previously translated guest DLL when its provenance
rem is still valid. --link-only validates its complete input fingerprint
rem without launching the game or recompiling any guest code.
set "REUSE_FLAG="
if exist "%WORK_DIR%\translation-cache.json" (
    python tools\run_game.py "%ELF%" ^
        --build-dir build ^
        --config Release ^
        --work-dir "%WORK_DIR%" ^
        --emit-opt=0 ^
        --no-recompile ^
        --link-only >nul 2>&1
    if not errorlevel 1 set "REUSE_FLAG=--no-recompile"
)

if not defined REUSE_FLAG (
    if exist "%WORK_DIR%\translation-cache.json" (
        rem Runtime or linker changes do not require rediscovering the ELF or
        rem retranslating the already reached guest functions. Repair the
        rem host DLL from the verified IR/object set when the guest generator
        rem and ELF are unchanged.
        echo === Checking preserved guest IR for a host-only relink ===
        python tools\run_game.py "%ELF%" ^
            --build-dir build ^
            --config Release ^
            --work-dir "%WORK_DIR%" ^
            --emit-opt=0 ^
            --emit-jobs=%EMIT_JOBS% ^
            --relink-existing ^
            --link-only >nul 2>&1
        if not errorlevel 1 set "REUSE_FLAG=--no-recompile"
    )
)
if defined REUSE_FLAG (
    echo === Reusing validated guest translation cache ===
) else (
    echo === First translation or changed guest generator: full guest translation ===
)

rem Temporary SDBZ-specific default diagnostics for the reproducible
rem LOOP_BUDGET at 0x0018e194. The watchpoint identified the
rem formatter's resumed PC 0x00194478 as the first writer of the
rem suspicious I/O-vector length word 0x01ffec14. Trace its saved
rem registers and stack vector just before the writer runs.
rem Set SDBZ_BOOT_DIAGNOSTICS=0 to disable these defaults.
rem Explicit EE_TRACE_ENTRY / EE_WATCH_WORD / EE_TRACE_STORE / EE_TRACE_V0 values take precedence.
if not defined SDBZ_BOOT_DIAGNOSTICS set "SDBZ_BOOT_DIAGNOSTICS=1"
if "%SDBZ_BOOT_DIAGNOSTICS%"=="1" (
    if not defined EE_TRACE_ENTRY set "EE_TRACE_ENTRY=0x00194478"
    if not defined EE_WATCH_WORD set "EE_WATCH_WORD=0x01ffec14"
    if not defined EE_TRACE_STORE set "EE_TRACE_STORE=0x01ffec14"
    if not defined EE_TRACE_V0 set "EE_TRACE_V0=0x00194478:0x00194794"
)
set "GUEST_TRACE_FLAGS="
if defined EE_TRACE_ENTRY set "GUEST_TRACE_FLAGS=%GUEST_TRACE_FLAGS% --trace-entry=%EE_TRACE_ENTRY%"
if defined EE_WATCH_WORD set "GUEST_TRACE_FLAGS=%GUEST_TRACE_FLAGS% --watch-word=%EE_WATCH_WORD%"
if defined EE_TRACE_STORE set "GUEST_TRACE_FLAGS=%GUEST_TRACE_FLAGS% --trace-store=%EE_TRACE_STORE%"
if defined EE_TRACE_V0 set "GUEST_TRACE_FLAGS=%GUEST_TRACE_FLAGS% --trace-v0=%EE_TRACE_V0%"
if defined GUEST_TRACE_FLAGS (
    echo === Active SDBZ guest diagnostics: %GUEST_TRACE_FLAGS% ===
) else (
    echo === SDBZ guest diagnostics disabled ===
)

rem Keep the already compiled guest objects, even across expansion budgets.
rem Two bounded passes of 128 new functions each are allowed; when the
rem first pass hits its limit, resume using the exact verified DLL/cache.
rem Trace only the final replay so a long boot doesn't make a 50 MB log.
echo === Running resumable local ELF expansion ===
echo Trace: "%WORK_DIR%\boot.log"
echo Expansion details: "%WORK_DIR%\expansion.log"
echo Report: "%WORK_DIR%\first-boot.json"

python tools\run_boot_session.py "%ELF%" ^
    --build-dir build ^
    --config Release ^
    --work-dir "%WORK_DIR%" ^
    --expansion-passes=2 ^
    --expansion-per-pass=128 ^
    --hle-startup ^
    --kseg-ram ^
    --emit-jobs=%EMIT_JOBS% ^
    --emit-opt=0 ^
    --max-dispatches=100000 ^
    --loop-budget=1000000 ^
    --sif-poll-limit=%SIF_POLL_LIMIT% ^
    --timeout-seconds=120 ^
    --report "%WORK_DIR%\first-boot.json" %REUSE_FLAG% %GUEST_TRACE_FLAGS% %IOP_IRX_FLAGS%

set "EXIT_CODE=%ERRORLEVEL%"
echo.
echo Run completed with exit code %EXIT_CODE%.
echo Review "%WORK_DIR%\boot.log" and "%WORK_DIR%\first-boot.json".
goto :done

:failed
set "EXIT_CODE=1"
echo.
echo The build or test could not complete. Review the error above.

:done
echo.
pause
exit /b %EXIT_CODE%
