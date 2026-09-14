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
rem Usage: Scripts\Windows\build.cmd [preset]      default preset: windows-debug
rem Configures (if needed) and builds the given CMake preset.
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-debug"

call "%~dp0vsenv.cmd"
if errorlevel 1 exit /b 1

pushd "%~dp0..\.."
cmake --preset %PRESET%
if errorlevel 1 (
    popd
    exit /b 1
)
cmake --build --preset %PRESET%
set "RC=%errorlevel%"
popd
exit /b %RC%
