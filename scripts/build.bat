@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0.."
if errorlevel 1 exit /b 1

set "BUILD_MODE=build"
if not "%~1"=="" set "BUILD_MODE=%~1"
if /I "%BUILD_MODE%"=="--help" goto :usage
if /I not "%BUILD_MODE%"=="build" if /I not "%BUILD_MODE%"=="--test" if /I not "%BUILD_MODE%"=="--native-tests" if /I not "%BUILD_MODE%"=="--quick-tests" if /I not "%BUILD_MODE%"=="--cdvd" if /I not "%BUILD_MODE%"=="--sif-probe" goto :invalid
if not "%~2"=="" goto :invalid
if defined VCToolsInstallDir goto :toolchain_ready
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

:toolchain_ready
if /I "%BUILD_MODE%"=="--native-tests" goto :native_toolchain
if /I "%BUILD_MODE%"=="--quick-tests" goto :native_toolchain
goto :configure

:native_toolchain
if not defined SDBZ_CLANG_CXX set "SDBZ_CLANG_CXX=C:\Program Files\LLVM\bin\clang++.exe"
if not exist "%SDBZ_CLANG_CXX%" (
    echo ERROR: LLVM clang++.exe not found at "%SDBZ_CLANG_CXX%"
    echo Set SDBZ_CLANG_CXX to the absolute path of clang++.exe.
    exit /b 1
)
set "CXX=%SDBZ_CLANG_CXX%"
for %%I in ("%CXX%") do set "PATH=%%~dpI;%PATH%"

where python >nul 2>nul
if errorlevel 1 (
    echo ERROR: Python is missing from PATH.
    exit /b 1
)

:configure
if exist "build\CMakeCache.txt" (
    cmake -S . -B build
) else (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
)
if errorlevel 1 exit /b 1
if /I "%BUILD_MODE%"=="--cdvd" goto :cdvd
if /I "%BUILD_MODE%"=="--sif-probe" goto :sif_probe
if /I "%BUILD_MODE%"=="--native-tests" goto :native_tests
if /I "%BUILD_MODE%"=="--quick-tests" goto :native_tests
cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1
if /I "%BUILD_MODE%"=="--test" goto :all_tests
exit /b 0

:all_tests
ctest --test-dir build -C Release --output-on-failure --no-tests=error
exit /b %ERRORLEVEL%

:cdvd
rem run_sdbz_cdvd.bat regenerates guest IR via ee_recomp.exe; build it with
rem the runner so native boot probes never silently use a stale lifter.
cmake --build build --config Release --target iop_cdvd_test iop_hw_test ee_decoder_test ee_vu0_macro_fmac_test ee_recomp ee_game_runner -j 8
if errorlevel 1 exit /b %errorlevel%

ctest --test-dir build -C Release --output-on-failure --no-tests=error -R "^(iop_cdvd|iop_hw|decoder|vu0_macro_fmac)$"
if errorlevel 1 exit /b %errorlevel%

echo CDVD focused build and tests passed.
exit /b 0

:sif_probe
echo === Building focused SIF, recompilation, and native-runner targets ===
rem run_game.py invokes ee_recomp.exe to produce the LLVM modules.  Keeping
rem that executable out of this target list can replay stale lowering code
rem even when ee_game_runner has been rebuilt.
cmake --build build --config Release --target iop_cpu_test iop_hw_test sif_dma_roundtrip_test iop_sif1_bridge_test ee_decoder_test ee_vu0_macro_fmac_test ee_recomp ee_game_runner -j 8
if errorlevel 1 exit /b %errorlevel%

echo === Running focused SIF regressions ===
ctest --test-dir build -C Release --output-on-failure --no-tests=error -R "^(iop_cpu|iop_hw|sif_dma_roundtrip|iop_sif1_bridge|decoder|vu0_macro_fmac)$"
if errorlevel 1 exit /b %errorlevel%

echo === Running the real OpenPS2BIOS / SDBZ probe ===
call "%~dp0run_sdbz_cdvd.bat"
set "RUN_RESULT=%ERRORLEVEL%"

echo.
echo Focused SIF tests passed. Real boot probe exit code: %RUN_RESULT%
exit /b %RUN_RESULT%

:native_tests
echo === Build required native host tools ===
cmake --build build --config Release --target ee_recomp ee_game_runner -j 8
if errorlevel 1 (
    echo ERROR: Building EE host tools failed.
    exit /b 1
)

echo.
rem Do not put regex alternation bars inside a parenthesized IF block.
rem cmd.exe treats those pipes as command operators while parsing the block.
if /I "%BUILD_MODE%"=="--quick-tests" goto quick_tests
goto full_tests

:quick_tests
set "TEST_LOG=%CD%\build\sdbz-native-regression-quick-ctest.log"
echo === Quick: Windows linker selection and guest PC trace ===
ctest --test-dir build -C Release --output-on-failure --no-tests=error --output-log "%TEST_LOG%" -R "^(ee_chunked_dispatcher|ee_instruction_trace_comparator)$"
set "TEST_EXIT=%ERRORLEVEL%"
goto report_tests

:full_tests
set "TEST_LOG=%CD%\build\sdbz-native-regression-ctest.log"
echo === Full: Windows linker selection, guest PC trace and runner ===
ctest --test-dir build -C Release --output-on-failure --no-tests=error --output-log "%TEST_LOG%" -R "^(ee_chunked_dispatcher|ee_instruction_trace_comparator|game_runner_end_to_end_[0-3])$"
set "TEST_EXIT=%ERRORLEVEL%"

:report_tests
echo.
echo CTest log: "%TEST_LOG%"
if not "%TEST_EXIT%"=="0" (
    echo ERROR: At least one native EE regression failed. Share the CTest log.
    exit /b %TEST_EXIT%
)

echo PASS: Native EE regression tests passed.
exit /b 0

:invalid
echo ERROR: Unknown build mode or extra argument.
call :usage
exit /b 2

:usage
echo Usage: scripts\build.bat [--test or --native-tests or --quick-tests or --cdvd or --sif-probe]
echo Default: configure and build all host targets. --test also runs CTest.
echo --native-tests / --quick-tests: focused native runner regression.
echo --cdvd: focused CDVD build and tests. --sif-probe: legacy real-disc probe.
exit /b 0
