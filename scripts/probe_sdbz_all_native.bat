@echo off
setlocal EnableExtensions EnableDelayedExpansion

cd /d "%~dp0.."

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe was not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
    echo ERROR: Visual Studio C++ tools were not found.
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b !ERRORLEVEL!

set "CXX=C:\Program Files\LLVM\bin\clang++.exe"
if not exist "%CXX%" (
    echo ERROR: LLVM clang++.exe was not found at %CXX%.
    exit /b 1
)

set "SDBZ_ISO=D:\Games\PS2_ISO\Super Dragon Ball Z (USA).iso"
if not exist "%SDBZ_ISO%" (
    echo ERROR: SDBZ ISO was not found at "%SDBZ_ISO%".
    exit /b 1
)

set "SDBZ_IOP_ROM0=%CD%\build\openps2bios\rom0.bin"
if not exist "%SDBZ_IOP_ROM0%" (
    echo ERROR: OpenPS2BIOS ROM0 is missing at "%SDBZ_IOP_ROM0%".
    echo Run build_openps2bios.bat or build.bat first.
    exit /b 1
)

rem Only the native host runner changes with fast snapshot pacing. Rebuild it
rem incrementally; leave the 8,000+ guest LLVM modules and DLL untouched.
echo === Updating native host runner (incremental) ===
cmake --build build --target ee_game_runner -j 8
if errorlevel 1 exit /b !ERRORLEVEL!

rem Keep --report and its one-guest-dispatch host slices for faithful IOP,
rem DMA and callback scheduling. Reduce only intermediate JSON writes. The
rem terminal report is always written in full by ee_game_runner.
if not defined SDBZ_REPORT_SNAPSHOT_MS set "SDBZ_REPORT_SNAPSHOT_MS=1000"
echo Report snapshot interval: %SDBZ_REPORT_SNAPSHOT_MS% ms

set "SDBZ_WORK=build\sdbz-all-native"
set "SDBZ_LOG=%SDBZ_WORK%\probe.log"
if not exist "%SDBZ_WORK%\game-translated.dll" (
    echo ERROR: Native SDBZ DLL is missing. Run scripts\build_sdbz_all_native.bat first.
    exit /b 1
)
if not exist "%SDBZ_WORK%\translation-cache.json" (
    echo ERROR: Translation cache is missing. Run scripts\build_sdbz_all_native.bat first.
    exit /b 1
)

rem --functions=all covers discovered functions, not every possible indirect
rem guest entry. A DISPATCH(3), status=0 at an executable ELF address means
rem the native PC map has no owner for that entry.
rem Reuse the fingerprint-validated native library and expand only genuinely
rem reached guest entries. Keep the work bounded and preserve every pass log.
if not defined SDBZ_PROBE_PASSES set "SDBZ_PROBE_PASSES=4"
set "PROBE_RESULT=1"
> "%SDBZ_LOG%" echo === All-native SDBZ coverage probe ===

for /l %%P in (1,1,%SDBZ_PROBE_PASSES%) do (
    set "PASS_LOG=%SDBZ_WORK%\probe-pass-%%P.log"
    echo === Coverage pass %%P/%SDBZ_PROBE_PASSES% ===
    python tools\run_game.py PS2_GAME\SLUS_214.42 ^
        --work-dir=%SDBZ_WORK% ^
        --build-dir=build ^
        --config=Release ^
        --no-recompile ^
        --auto-expand=128 ^
        --auto-expand-table=24 ^
        --emit-jobs=8 ^
        --emit-opt=0 ^
        --max-dispatches=100000 ^
        --loop-budget=1000000 ^
        --sif-poll-limit=2048 ^
        --timeout-seconds=120 ^
        --report=%SDBZ_WORK%\first-boot.json ^
        --hle-startup ^
        --kseg-ram ^
        --disc-image="%SDBZ_ISO%" > "!PASS_LOG!" 2>&1

    set "PROBE_RESULT=!ERRORLEVEL!"
    type "!PASS_LOG!"
    type "!PASS_LOG!" >> "%SDBZ_LOG%"

    rem Look at the LAST guest stop, not merely the fact that some entry
    rem was compiled earlier in this pass. A real LOOP_BUDGET, UNSUPPORTED or
    rem hardware wait must not replay the same terminal state four times.
    set "LAST_STOP="
    for /f "usebackq delims=" %%L in (`findstr /L /C:"EE stopped:" "!PASS_LOG!"`) do set "LAST_STOP=%%L"
    if defined LAST_STOP echo !LAST_STOP!
    if not "!PROBE_RESULT!"=="3" goto probe_complete
    if not defined LAST_STOP goto probe_complete
    echo(!LAST_STOP! | findstr /L /C:"reason=DISPATCH(3) status=0" >nul
    if errorlevel 1 goto probe_complete
    findstr /L /C:"auto-expand: compiling reachable PC " "!PASS_LOG!" >nul
    if errorlevel 1 (
        echo Coverage probe stopped without any newly translated guest entry.
        goto probe_complete
    )
)

echo Coverage probe reached its bounded pass cap; rerun to continue from the verified cache.

:probe_complete
echo Coverage probe exit code: %PROBE_RESULT%
exit /b %PROBE_RESULT%
