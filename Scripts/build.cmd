@echo off
rem Usage: Scripts\build.cmd [preset]      default preset: windows-debug
rem Configures (if needed) and builds the given CMake preset.
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-debug"

call "%~dp0vsenv.cmd"
if errorlevel 1 exit /b 1

pushd "%~dp0.."
cmake --preset %PRESET%
if errorlevel 1 (
    popd
    exit /b 1
)
cmake --build --preset %PRESET%
set "RC=%errorlevel%"
popd
exit /b %RC%
