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
rem Usage: Scripts\clean.cmd [all]
rem
rem Reclaims the disk space a C++ build leaves lying about. By default it removes only work that
rem can be regenerated without downloading anything: the intermediate trees vcpkg keeps after it
rem has finished building a library, which on this project are about twenty gigabytes and are
rem consulted by nothing afterwards.
rem
rem "all" additionally removes the compiled output of both presets, which means the next build is a
rem full one. It does not touch vcpkg's downloads, so even that stays offline.

setlocal
set "ROOT=%~dp0.."

echo [clean] vcpkg intermediates
for %%d in (
    "%ROOT%\build\windows-debug\vcpkg_installed\vcpkg\blds"
    "%ROOT%\build\windows-debug\vcpkg_installed\vcpkg\pkgs"
    "%ROOT%\build\windows-release\vcpkg_installed\vcpkg\blds"
    "%ROOT%\build\windows-release\vcpkg_installed\vcpkg\pkgs"
    "%ROOT%\build\windows-relwithdebinfo\vcpkg_installed\vcpkg\blds"
    "%ROOT%\build\windows-relwithdebinfo\vcpkg_installed\vcpkg\pkgs"
) do (
    if exist %%d (
        echo   %%~d
        rmdir /s /q %%d
    )
)

if not defined VCPKG_ROOT set "VCPKG_ROOT=C:\Dev\vcpkg"
for %%d in ("%VCPKG_ROOT%\buildtrees" "%VCPKG_ROOT%\packages") do (
    if exist %%d (
        echo   %%~d
        rmdir /s /q %%d
    )
)

if /i "%~1"=="all" (
    echo [clean] compiled output
    for %%d in (
        "%ROOT%\build\windows-debug\bin"
        "%ROOT%\build\windows-debug\Engine"
        "%ROOT%\build\windows-debug\Game"
        "%ROOT%\build\windows-debug\Tools"
        "%ROOT%\build\windows-debug\Tests"
        "%ROOT%\build\windows-release\bin"
        "%ROOT%\build\windows-release\Engine"
        "%ROOT%\build\windows-release\Game"
        "%ROOT%\build\windows-release\Tools"
        "%ROOT%\build\windows-release\Tests"
    ) do (
        if exist %%d (
            echo   %%~d
            rmdir /s /q %%d
        )
    )
)

echo [clean] done
endlocal
