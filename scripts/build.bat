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

echo === Configuring ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%

echo === Building focused SIF, recompilation, and native-runner targets ===
rem run_game.py invokes ee_recomp.exe to produce the LLVM modules.  Keeping
rem that executable out of this target list can replay stale lowering code
rem even when ee_game_runner has been rebuilt.
cmake --build build --target iop_cpu_test iop_hw_test sif_dma_roundtrip_test iop_sif1_bridge_test ee_decoder_test ee_vu0_macro_fmac_test ee_recomp ee_game_runner -j 8
if errorlevel 1 exit /b %errorlevel%

echo === Running focused SIF regressions ===
ctest --test-dir build -C Release --output-on-failure -R "^(iop_cpu|iop_hw|sif_dma_roundtrip|iop_sif1_bridge|decoder|vu0_macro_fmac)$"
if errorlevel 1 exit /b %errorlevel%

echo === Running the real OpenPS2BIOS / SDBZ probe ===
call "%~dp0run_sdbz_cdvd.bat"
set "RUN_RESULT=%ERRORLEVEL%"

echo.
echo Focused SIF tests passed. Real boot probe exit code: %RUN_RESULT%
exit /b %RUN_RESULT%
