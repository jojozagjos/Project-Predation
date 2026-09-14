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
