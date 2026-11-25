@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem Uncapped P40 retail profiling replay; requires a recovered EE cache
rem and the previously built VU1 AOT library. Never recover or rebuild IR here.
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
set "SDBZ_DISPATCH_TRACE_EVERY=1024"

set "SDBZ_EE_DIRECT_DISPATCH=1"
set "SDBZ_VU1_ASYNC=1"
set "SDBZ_GS_ASYNC=1"

set "SDBZ_VU1_AOT=%CD%\build\vu1-native-v19"
set "SDBZ_VU1_AOT_CAPTURE="
set "SDBZ_VU1_AOT_CAPTURE_MISSES=%CD%\build\vu1-misses"

set "EE_WEBGPU_SNAPSHOT=1"
set "EE_WEBGPU_WINDOW=1"

call run_ps2_game.bat --full --neutral
exit /b %ERRORLEVEL%
