@echo off
rem Sets up the MSVC x64 environment and VCPKG_ROOT for the current shell.
rem Intended to be invoked with "call" from the other scripts or a terminal.
if not defined VCPKG_ROOT set "VCPKG_ROOT=C:\Dev\vcpkg"

rem CMake and Ninja installed per-user through winget live under WinGet\Links (portable packages).
rem Add them when they are not already reachable on PATH (fresh shells before a restart).
where cmake >nul 2>nul || set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
where cmake >nul 2>nul || for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\Kitware.CMake*\cmake-*") do set "PATH=%%~d\bin;%PATH%"
where ninja >nul 2>nul || for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja*") do set "PATH=%%~d;%PATH%"

if defined VCINSTALLDIR exit /b 0

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [vsenv] vswhere.exe not found. Install Visual Studio 2022 Build Tools with the C++ workload.
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [vsenv] No Visual Studio installation with C++ tools found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
exit /b %errorlevel%
