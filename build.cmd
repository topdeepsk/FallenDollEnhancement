@echo off
rem Build GameEnhancement (injector exe + injected dll) with CMake + Ninja.
rem Run from a VS Developer Command Prompt, or let this script locate VS itself.

setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto novs

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto novs

:build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

cmake --build build
if errorlevel 1 exit /b 1

echo.
echo Build finished. Artifacts in build\bin :
dir /b build\bin
exit /b 0

:novs
echo [ERROR] Visual Studio C++ toolchain not found.
echo         Run this from a Developer Command Prompt, or install the MSVC x64 toolset.
exit /b 1
