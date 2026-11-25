@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Batch launchers use CRLF in the working tree (.gitattributes).

rem Uncapped P40 retail profiling replay; requires an existing EE cache
rem and the previously built VU1 AOT library. Pass --overwrite-ir to refresh EE IR.
cd /d "%~dp0"
if errorlevel 1 exit /b 1

git pull --ff-only
if errorlevel 1 exit /b 1

set "SDBZ_EE_OPT=5"
set "SDBZ_SKIP_HEADLESS_EXPANSION=1"
set "SDBZ_SPEED_PERCENT=0"

set "MAX_DISPATCHES=40000000"
set "MAX_EXPANSION_DISPATCHES=40000000"
set "MAX_REPLAY_DISPATCHES=40000000"
set "BOOT_TIMEOUT_SECONDS=600"

set "SDBZ_RUNTIME_PROFILE=1"
set "SDBZ_TRACE_BUFFERED=1"
set "SDBZ_REPORT_SNAPSHOT_MS=2000"
rem Progress text only; retain exact dispatch counters and periodic snapshots.
set "SDBZ_REPORT_PROGRESS_EVERY=8192"
set "SDBZ_DISPATCH_TRACE_EVERY=1024"
rem Host polling gate; every observed guest field advance is still serviced.
set "SDBZ_WINDOW_PUMP_STRIDE=64"

set "SDBZ_EE_DIRECT_DISPATCH=1"
set "SDBZ_VU1_ASYNC=1"
set "SDBZ_GS_ASYNC=1"
rem Retail-validated experimental mode; regular runner defaults remain unchanged.
set "SDBZ_GS_GPU_VRAM=1"
rem Disable byte-wise image hashing, retain committed event/QWC counters.
set "SDBZ_GIF_IMAGE_DIGEST=0"

set "SDBZ_VU1_AOT=%CD%\build\vu1-native-v20"
set "SDBZ_VU1_AOT_CAPTURE="
set "SDBZ_VU1_AOT_CAPTURE_MISSES=%CD%\build\vu1-misses"

set "EE_WEBGPU_SNAPSHOT=1"
set "EE_WEBGPU_WINDOW=1"

call run_ps2_game.bat --full --neutral %*
exit /b %ERRORLEVEL%
