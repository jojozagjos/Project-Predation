@echo off
rem Holds the window open when this is double-clicked. See Scripts/README.md for why.
if defined PRED_KEEP_OPEN goto :body
if defined PRED_NO_PAUSE goto :body
echo %cmdcmdline% | "%SystemRoot%\System32\find.exe" /i "%~nx0" >nul || goto :body
set "PRED_KEEP_OPEN=1"
call "%~f0" %*
echo.
echo [%~n0] finished with code %errorlevel%. Press any key to close this window.
pause >nul
exit /b %errorlevel%
:body
rem Usage: Scripts\Windows\run.cmd [preset] [game args...]
rem Launches the game. With no preset named it plays whichever build is actually there, preferring
rem release, because somebody who double-clicked this wants to play the game rather than to be told
rem that a preset they have never heard of has not been built.
rem Set PRED_BUILD_DIR to override the build directory (for CMakeUserPresets overrides).
rem %~dp0 is this folder. Capture it before the shift below: shift moves %0 along with the
rem rest, so after it %~dp0 would be the current directory instead of this script's folder.
set "HERE=%~dp0"
set "PRESET=%~1"
if not "%~1"=="" shift

if "%PRESET%"=="" (
    for %%p in (windows-release windows-relwithdebinfo windows-debug) do (
        if not defined PRESET if exist "%HERE%..\..\build\%%p\bin\ProjectPredation.exe" set "PRESET=%%p"
    )
)
if "%PRESET%"=="" set "PRESET=windows-release"

set "BUILD_DIR=%PRED_BUILD_DIR%"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%HERE%..\..\build\%PRESET%"

set "EXE=%BUILD_DIR%\bin\ProjectPredation.exe"
if not exist "%EXE%" (
    echo [run] Nothing built yet: %EXE%
    echo [run] Double-click Play.cmd in this folder: it builds and then runs.
    exit /b 1
)
echo [run] Playing the %PRESET% build.
"%EXE%" %1 %2 %3 %4 %5 %6 %7 %8 %9
exit /b %errorlevel%
