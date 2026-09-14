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
rem Usage: Scripts\Windows\clean.cmd [all]
rem
rem Reclaims the disk space a C++ build leaves lying about. By default it removes only work that
rem can be regenerated without downloading anything: the intermediate trees vcpkg keeps after it
rem has finished building a library, which on this project are about twenty gigabytes and are
rem consulted by nothing afterwards.
rem
rem "all" additionally removes the compiled output of both presets, which means the next build is a
rem full one. It does not touch vcpkg's downloads, so even that stays offline.

setlocal
set "ROOT=%~dp0..\.."

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
