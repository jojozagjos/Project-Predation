@echo off
rem Usage: Scripts\run.cmd [preset] [game args...]     default preset: windows-debug
rem Launches the game executable from the preset's build directory.
rem Set PRED_BUILD_DIR to override the build directory (for CMakeUserPresets overrides).
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-debug"
if not "%~1"=="" shift

set "BUILD_DIR=%PRED_BUILD_DIR%"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%~dp0..\build\%PRESET%"

set "EXE=%BUILD_DIR%\bin\ProjectPredation.exe"
if not exist "%EXE%" (
    echo [run] Executable not found: %EXE%
    echo [run] Build first: Scripts\build.cmd %PRESET%
    exit /b 1
)
"%EXE%" %1 %2 %3 %4 %5 %6 %7 %8 %9
exit /b %errorlevel%
