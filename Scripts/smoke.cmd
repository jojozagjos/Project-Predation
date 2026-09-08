@echo off
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
