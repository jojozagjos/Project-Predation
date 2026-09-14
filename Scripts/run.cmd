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
rem Usage: Scripts\run.cmd [preset] [game args...]
rem Launches the game. With no preset named it plays whichever build is actually there, preferring
rem release, because somebody who double-clicked this wants to play the game rather than to be told
rem that a preset they have never heard of has not been built.
rem Set PRED_BUILD_DIR to override the build directory (for CMakeUserPresets overrides).
set "PRESET=%~1"
if not "%~1"=="" shift

if "%PRESET%"=="" (
    for %%p in (windows-release windows-relwithdebinfo windows-debug) do (
        if not defined PRESET if exist "%~dp0..\build\%%p\bin\ProjectPredation.exe" set "PRESET=%%p"
    )
)
if "%PRESET%"=="" set "PRESET=windows-release"

set "BUILD_DIR=%PRED_BUILD_DIR%"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%~dp0..\build\%PRESET%"

set "EXE=%BUILD_DIR%\bin\ProjectPredation.exe"
if not exist "%EXE%" (
    echo [run] Nothing built yet: %EXE%
    echo [run] Double-click Play.cmd in the folder above this one: it builds and then runs.
    exit /b 1
)
echo [run] Playing the %PRESET% build.
"%EXE%" %1 %2 %3 %4 %5 %6 %7 %8 %9
exit /b %errorlevel%
