@echo off
rem Double-clicked from Explorer, a script window closes the instant the script finishes and
rem takes everything it printed with it, so a run that worked and a run that failed look exactly
rem the same: nothing. Run again under a shell that waits, and say so at the end.
if defined PRED_KEEP_OPEN goto :pred_body
rem Explorer launches a double-clicked script by its full path; anybody typing one at a prompt or
rem calling it from another script uses a relative one or no quotes, so the full path is what tells
rem the two apart. PRED_NO_PAUSE opts out of it for anything automated that does use a full path.
if defined PRED_NO_PAUSE goto :pred_body
echo %cmdcmdline% | find /i "%~f0" >nul || goto :pred_body
set "PRED_KEEP_OPEN=1"
call "%~f0" %*
echo.
echo [%~n0] finished with code %errorlevel%. Press any key to close this window.
pause >nul
exit /b %errorlevel%
:pred_body
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
