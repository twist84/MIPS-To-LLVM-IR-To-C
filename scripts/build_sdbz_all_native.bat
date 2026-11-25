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

set "CXX=C:\Program Files\LLVM\bin\clang++.exe"
if not exist "%CXX%" (
    echo ERROR: LLVM clang++.exe was not found at %CXX%.
    exit /b 1
)

echo === Configuring and building native tools ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%
cmake --build build --target ee_recomp ee_game_runner -j 8
if errorlevel 1 exit /b %errorlevel%

echo === Generating all SDBZ IR modules, compiling objects, and linking ===
echo This intentionally does not launch the game runner.
python tools\run_game.py PS2_GAME\SLUS_214.42 ^
    --work-dir=build\sdbz-all-native ^
    --build-dir=build ^
    --config=Release ^
    --functions=all ^
    --emit-jobs=8 ^
    --emit-opt=0 ^
    --link-only
if errorlevel 1 exit /b %errorlevel%

call "%~dp0probe_sdbz_all_native.bat"
exit /b %ERRORLEVEL%
