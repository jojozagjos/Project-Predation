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
rem "all" additionally removes every preset's compiled output, which means the next build is a full
rem one. It leaves build\vcpkg_installed alone, so the next build is a compile and not a download,
rem and it does not touch vcpkg's own downloads either.

setlocal EnableDelayedExpansion
set "ROOT=%~dp0..\.."
set "BUILD=%ROOT%\build"

echo [clean] vcpkg intermediates
rem The shared tree the presets point at, and any per-preset trees left over from before they
rem shared one. Each of those was three gigabytes of exactly the same libraries.
for %%d in ("%BUILD%\vcpkg_installed\vcpkg\blds" "%BUILD%\vcpkg_installed\vcpkg\pkgs") do (
    if exist %%d (
        echo   %%~d
        rmdir /s /q %%d
    )
)
for /d %%p in ("%BUILD%\*") do (
    if exist "%%~fp\vcpkg_installed" (
        echo   %%~fp\vcpkg_installed  ^(superseded by the shared one^)
        rmdir /s /q "%%~fp\vcpkg_installed"
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
    rem Every preset folder under build, which is to say everything except the shared dependency
    rem tree and whatever the packaging script laid out.
    for /d %%p in ("%BUILD%\*") do (
        set "NAME=%%~nxp"
        if /i not "!NAME!"=="vcpkg_installed" if /i not "!NAME!"=="package" (
            echo   %%~fp
            rmdir /s /q "%%~fp"
        )
    )
)

echo [clean] done
endlocal
