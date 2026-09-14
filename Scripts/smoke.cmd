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
rem Headless smoke test: runs the game for a fixed number of frames, saves a screenshot,
rem and checks the log for errors. Exits non-zero if anything failed.
rem
rem Usage: Scripts\smoke.cmd [preset]      default preset: windows-debug
setlocal EnableDelayedExpansion

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-debug"

set "ROOT=%~dp0.."
set "BUILD_DIR=%PRED_BUILD_DIR%"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%ROOT%\build\%PRESET%"

set "EXE=%BUILD_DIR%\bin\ProjectPredation.exe"
set "OUTDIR=%BUILD_DIR%\smoke"
set "SHOT=%OUTDIR%\smoke.png"
set "LOG=%OUTDIR%\smoke.log"

if not exist "%EXE%" (
    echo [smoke] FAIL: executable not found: %EXE%
    echo [smoke] Build first: Scripts\build.cmd %PRESET%
    exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if exist "%SHOT%" del /q "%SHOT%"

echo [smoke] Running 60 frames headless...
"%EXE%" --frames 60 --screenshot "%SHOT%" --log-level debug > "%LOG%" 2>&1
set "RC=%errorlevel%"

if not "%RC%"=="0" (
    echo [smoke] FAIL: exit code %RC%
    echo [smoke] --- last 30 log lines ---
    powershell -NoProfile -Command "Get-Content '%LOG%' -Tail 30"
    exit /b 1
)

if not exist "%SHOT%" (
    echo [smoke] FAIL: no screenshot was written to %SHOT%
    echo [smoke] --- last 30 log lines ---
    powershell -NoProfile -Command "Get-Content '%LOG%' -Tail 30"
    exit /b 1
)

rem A window that rendered nothing still produces a valid PNG, so require a plausible size.
for %%F in ("%SHOT%") do set "SHOTSIZE=%%~zF"
if !SHOTSIZE! LSS 2000 (
    echo [smoke] FAIL: screenshot is only !SHOTSIZE! bytes, expected a rendered frame
    exit /b 1
)

findstr /C:"[error]" /C:"[critical]" "%LOG%" >nul 2>&1
if not errorlevel 1 (
    echo [smoke] FAIL: log contains errors
    findstr /C:"[error]" /C:"[critical]" "%LOG%"
    exit /b 1
)

echo [smoke] PASS
echo [smoke]   screenshot: %SHOT% ^(!SHOTSIZE! bytes^)
echo [smoke]   log:        %LOG%
exit /b 0
