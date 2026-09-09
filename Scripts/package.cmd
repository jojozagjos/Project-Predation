@echo off
rem Builds a release and lays out a folder somebody else can run, then zips it.
rem
rem The game finds its assets in an "Assets" folder next to the executable, so packaging is a copy
rem rather than a build step: the exe, the data files, and the shaders the build compiled.
rem
rem Usage: Scripts\package.cmd [preset]      default preset: windows-release
setlocal EnableDelayedExpansion

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-release"

set "ROOT=%~dp0.."
set "BUILD_DIR=%ROOT%\build\%PRESET%"
set "STAGE=%ROOT%\build\package\ProjectPredation"
set "ZIP=%ROOT%\build\package\ProjectPredation-%PRESET%.zip"

echo [package] Building %PRESET%...
call "%~dp0build.cmd" %PRESET%
if errorlevel 1 (
    echo [package] FAIL: build failed
    exit /b 1
)

if not exist "%BUILD_DIR%\bin\ProjectPredation.exe" (
    echo [package] FAIL: no executable at %BUILD_DIR%\bin\ProjectPredation.exe
    exit /b 1
)

echo [package] Laying out %STAGE%...
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%" 2>nul

rem The executable and anything the build deployed beside it.
copy /y "%BUILD_DIR%\bin\ProjectPredation.exe" "%STAGE%\" >nul
for %%F in ("%BUILD_DIR%\bin\*.dll") do copy /y "%%F" "%STAGE%\" >nul 2>nul

rem The data files, and then the compiled shaders on top of them. Both end up under Assets, which
rem is the first place the game looks, so the folder runs anywhere without the build tree.
xcopy /e /i /q /y "%ROOT%\Assets" "%STAGE%\Assets" >nul
if exist "%BUILD_DIR%\GeneratedAssets\Shaders" (
    xcopy /e /i /q /y "%BUILD_DIR%\GeneratedAssets\Shaders" "%STAGE%\Assets\Shaders" >nul
) else (
    echo [package] FAIL: no compiled shaders in %BUILD_DIR%\GeneratedAssets\Shaders
    exit /b 1
)

> "%STAGE%\README.txt" (
    echo PROJECT PREDATION
    echo ACRD // Anomalous Containment ^& Research Directorate
    echo.
    echo Run ProjectPredation.exe.
    echo.
    echo Playing together:
    echo   One person picks "Open a game" and tells the others their address.
    echo   Everyone else types that address into "Join a game" and presses Join.
    echo   On the same house or office network that is the host's local address,
    echo   something like 192.168.1.20. Over the internet the host has to forward
    echo   UDP port 27015 to their machine; there is no matchmaking yet.
    echo.
    echo Controls:
    echo   WASD move, Space jump, Ctrl or C crouch, Z prone, Shift sprint, Alt walk
    echo   Q and E lean, left mouse fire, right mouse aim, R reload
    echo   F interact, G drop, 1-6 and the wheel select, Tab inventory
    echo   P cycles first person, third person and free camera
    echo   F3 debug overlay, backtick console, Escape frees the cursor and then leaves
    echo.
    echo If it will not start, install the Microsoft Visual C++ Redistributable for x64.
)

echo [package] Zipping...
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 (
    echo [package] FAIL: could not create the zip
    exit /b 1
)

for %%F in ("%ZIP%") do set "ZIPSIZE=%%~zF"
echo [package] PASS
echo [package]   folder: %STAGE%
echo [package]   zip:    %ZIP% ^(!ZIPSIZE! bytes^)
exit /b 0
