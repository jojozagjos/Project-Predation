@echo off
rem Double-clicked from Explorer, a script window closes the instant the script finishes and
rem takes everything it printed with it, so a run that worked and a run that failed look exactly
rem the same: nothing. Run again under a shell that waits, and say so at the end.
if defined PRED_KEEP_OPEN goto :pred_body
rem Anything started through "cmd /c", which is how Explorer starts a double-clicked script, has the
rem script named on the shell's own command line. A script typed at a prompt that is already open
rem does not: that shell was started for the person, not for this file. Matching the name rather
rem than the full path is deliberate, because Explorer does not always hand over the same spelling
rem of a path that the script sees for itself, and a missed pause is a window that vanishes.
rem
rem Pausing too often costs nothing: with no console to read a key from, pause returns at once.
if defined PRED_NO_PAUSE goto :pred_body
echo %cmdcmdline% | find /i "%~nx0" >nul || goto :pred_body
set "PRED_KEEP_OPEN=1"
call "%~f0" %*
echo.
echo [%~n0] finished with code %errorlevel%. Press any key to close this window.
pause >nul
exit /b %errorlevel%
:pred_body
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
