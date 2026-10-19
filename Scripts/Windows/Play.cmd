@echo off
rem Holds the window open when this is double-clicked and something went wrong. See Scripts/README.md for why.
if defined PRED_KEEP_OPEN goto :body
if defined PRED_NO_PAUSE goto :body
echo %cmdcmdline% | "%SystemRoot%\System32\find.exe" /i "%~nx0" >nul || goto :body
set "PRED_KEEP_OPEN=1"
call "%~f0" %*
rem The game started: nothing to read here, so the window goes and only the game is left.
if not errorlevel 1 exit /b 0
echo.
echo [%~n0] finished with code %errorlevel%. Press any key to close this window.
pause >nul
exit /b %errorlevel%
:body

setlocal
set "ROOT=%~dp0..\..\"
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-release"
set "EXE=%ROOT%build\%PRESET%\bin\ProjectPredation.exe"

if exist "%EXE%" (
    echo [play] Building anything that has changed...
) else (
    echo [play] First build. This one takes a while; every one after it is seconds.
)

call "%~dp0build.cmd" %PRESET%
if errorlevel 1 (
    rem Once more before giving up: a file held for a moment by something else (an antivirus looking at what was just
    rem written, LNK1104 "cannot open file") fails a build that goes through the second time.
    echo.
    echo [play] The build failed; trying once more in case a file was only held for a moment...
    call "%~dp0build.cmd" %PRESET%
)
if errorlevel 1 (
    echo.
    echo [play] The build failed. The errors are above, and the first one is the one that matters.
    exit /b 1
)

if not exist "%EXE%" (
    echo [play] The build said it worked but there is no game at %EXE%
    exit /b 1
)

echo.
echo [play] Starting.
rem On its own, so this window can close: the game has no console of its own, and what it says is in its log,
rem %APPDATA%\CIRRA\ProjectPredation\Logs\predation.log.
start "" "%EXE%"
endlocal & exit /b 0
