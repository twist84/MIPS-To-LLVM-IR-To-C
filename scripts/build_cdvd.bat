@echo off
setlocal

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
if errorlevel 1 exit /b %errorlevel%

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%

rem run_sdbz_cdvd.bat regenerates guest IR via ee_recomp.exe; build it with
rem the runner so native boot probes never silently use a stale lifter.
cmake --build build --target iop_cdvd_test iop_hw_test ee_decoder_test ee_vu0_macro_fmac_test ee_recomp ee_game_runner -j 8
if errorlevel 1 exit /b %errorlevel%

ctest --test-dir build -C Release --output-on-failure -R "^(iop_cdvd|iop_hw|decoder|vu0_macro_fmac)$"
if errorlevel 1 exit /b %errorlevel%

echo CDVD focused build and tests passed.
exit /b 0
