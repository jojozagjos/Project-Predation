@echo off
rem Usage: Scripts\test.cmd [preset]      default preset: windows-debug
rem Runs the unit tests for the given preset (build first with build.cmd).
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-debug"

call "%~dp0vsenv.cmd"
if errorlevel 1 exit /b 1

pushd "%~dp0.."
ctest --preset %PRESET%
set "RC=%errorlevel%"
popd
exit /b %RC%
