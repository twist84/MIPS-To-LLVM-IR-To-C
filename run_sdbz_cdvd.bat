@echo off
setlocal

cd /d "%~dp0"
call build_cdvd.bat
if errorlevel 1 exit /b %errorlevel%

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
    echo ERROR: Visual Studio C++ tools were not found.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%

set "CXX=C:\Program Files\LLVM\bin\clang++.exe"
if not exist "%CXX%" (
    echo ERROR: LLVM clang++.exe was not found at %CXX%.
    exit /b 1
)

set "SDBZ_IOP_ROM0=%CD%\build\openps2bios\rom0.bin"
set "SDBZ_ISO=D:\Games\PS2_ISO\Super Dragon Ball Z (USA).iso"
set "SDBZ_WORK=build\sdbz-cdvd"
if not exist "%SDBZ_WORK%" mkdir "%SDBZ_WORK%"

del /q "%SDBZ_WORK%\run.log" 2>nul
set "RUN_RESULT=1"
for /l %%P in (1,1,4) do call :run_pass %%P
goto show_result

:run_pass
if "%RUN_RESULT%"=="0" exit /b 0
echo ==== cached expansion pass %1 ==== >> "%SDBZ_WORK%\run.log"
python tools\run_game.py PS2_GAME\SLUS_214.42 ^
    --work-dir="%SDBZ_WORK%" ^
    --report="%SDBZ_WORK%\first-boot.json" ^
    --build-dir=build ^
    --config=Release ^
    --hle-startup ^
    --kseg-ram ^
    --emit-jobs=8 ^
    --emit-opt=0 ^
    --max-dispatches=100000 ^
    --loop-budget=1000000 ^
    --sif-poll-limit=256 ^
    --timeout-seconds=120 ^
    --auto-expand=128 ^
    --no-recompile ^
    --disc-image="%SDBZ_ISO%" >> "%SDBZ_WORK%\run.log" 2>&1
set "RUN_RESULT=%errorlevel%"
exit /b 0

:show_result

type "%SDBZ_WORK%\run.log"
echo run_game exit code: %RUN_RESULT%
exit /b %RUN_RESULT%
