@echo off
setlocal EnableExtensions

cd /d "%~dp0.."

rem Capture only the genuine original-game VBlank polling guest PCs.
rem This is a read-only cached replay: no 8k-function IR regeneration,
rem no native DLL relinking, no synthetic cycles, no synthetic VBlank.
set "SDBZ_WORK=build\sdbz-all-native"
set "SDBZ_TRACE=%SDBZ_WORK%\vblank-pcs.csv"
set "SDBZ_TRACE_REPORT=%SDBZ_WORK%\vblank-trace-report.json"
if not defined SDBZ_ISO set "SDBZ_ISO=D:\Games\PS2_ISO\Super Dragon Ball Z (USA).iso"
if not defined SDBZ_IOP_ROM0 set "SDBZ_IOP_ROM0=%CD%\build\openps2bios\rom0.bin"
if not defined SDBZ_REPORT_SNAPSHOT_MS set "SDBZ_REPORT_SNAPSHOT_MS=2000"

if not exist "%SDBZ_ISO%" (
    echo ERROR: USA disc image not found: "%SDBZ_ISO%"
    exit /b 1
)
if not exist "%SDBZ_IOP_ROM0%" (
    echo ERROR: OpenPS2BIOS ROM0 not found: "%SDBZ_IOP_ROM0%"
    exit /b 1
)
if not exist "build\ee_game_runner.exe" (
    echo ERROR: Native EE runner not built.
    exit /b 1
)
if not exist "%SDBZ_WORK%\game-translated.dll" (
    echo ERROR: Linked SDBZ guest DLL not found. Build all-native once.
    exit /b 1
)
if not exist "%SDBZ_WORK%\translation-cache.json" (
    echo ERROR: Fingerprint-validated native translation cache not found.
    exit /b 1
)

echo === Authentic US SDBZ native VBlank instruction-PC trace ===
echo === Using validated 8046-module cache if current fingerprints match ===
echo Expected guest stop: 0x00175210, LOOP_BUDGET(5); not a frame.
echo Output: %SDBZ_TRACE%
echo Report: %SDBZ_TRACE_REPORT%
python tools\run_game.py PS2_GAME\SLUS_214.42 ^
    --work-dir=%SDBZ_WORK% ^
    --build-dir=build ^
    --config=Release ^
    --no-recompile ^
    --max-dispatches=100000 ^
    --loop-budget=1000000 ^
    --sif-poll-limit=2048 ^
    --timeout-seconds=120 ^
    --trace-instructions=%SDBZ_TRACE% ^
    --trace-instructions-window=0x001751ec:0x00175228 ^
    --report=%SDBZ_TRACE_REPORT% ^
    --hle-startup ^
    --kseg-ram ^
    --disc-image="%SDBZ_ISO%"

set "TRACE_EXIT=%ERRORLEVEL%"
if exist "%SDBZ_TRACE%" (
    echo Native instruction trace written: %SDBZ_TRACE%
) else (
    echo ERROR: No instruction trace was produced. Check the command output.
)
echo Native runner exit code: %TRACE_EXIT%
exit /b %TRACE_EXIT%
