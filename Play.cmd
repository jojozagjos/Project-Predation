@echo off
rem The one thing to double-click. Builds the game if it needs building, then plays it.
rem
rem Everything in Scripts\ is a separate step for a separate job, which is right when you know which
rem step you want and no use at all when you just want to play. This is that: one file, in the
rem folder anybody opens first, that does the whole thing and says what it is doing while it does it.

rem Started through "cmd /c", which is how Explorer starts a double-clicked file. Keep the window
rem open at the end, or a failure and a success look identical: a flash, and nothing.
if defined PRED_KEEP_OPEN goto :pred_body
if defined PRED_NO_PAUSE goto :pred_body
echo %cmdcmdline% | find /i "%~nx0" >nul || goto :pred_body
set "PRED_KEEP_OPEN=1"
call "%~f0" %*
echo.
echo [play] Finished with code %errorlevel%. Press any key to close this window.
pause >nul
exit /b %errorlevel%
:pred_body

setlocal
set "ROOT=%~dp0"
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-release"
set "EXE=%ROOT%build\%PRESET%\bin\ProjectPredation.exe"

if exist "%EXE%" (
    echo [play] Building anything that has changed...
) else (
    echo [play] First build. This one takes a while; every one after it is seconds.
)

call "%ROOT%Scripts\build.cmd" %PRESET%
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
"%EXE%"
set "RC=%errorlevel%"
if not "%RC%"=="0" (
    echo [play] The game exited with code %RC%. The log is in:
    echo [play]   %%APPDATA%%\ACRD\ProjectPredation\Logs\predation.log
)
endlocal & exit /b %RC%
