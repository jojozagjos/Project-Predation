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
rem Builds a shipping release and lays out a folder somebody else can run, then zips it.
rem
rem The game finds its assets in an "Assets" folder next to the executable, so packaging is a copy
rem rather than a build step: the exe, the data files, and the shaders the build compiled.
rem
rem This builds the windows-shipping preset, which has its own build folder. That matters: the
rem developer tools are a cached CMake variable, so building a shipping exe inside the same folder
rem as the everyday one used to leave that folder without its tools until somebody noticed.
rem
rem Usage: Scripts\Windows\package.cmd [preset]      default preset: windows-shipping
setlocal EnableDelayedExpansion

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-shipping"

rem The staged folder and the zip go in dist, not in build. build holds the dependencies
rem and one folder per preset, and the thing you send somebody is not another build.
set "ROOT=%~dp0..\.."
set "BUILD_DIR=%ROOT%\build\%PRESET%"
set "STAGE=%ROOT%\dist\ProjectPredation"
set "ZIP=%ROOT%\dist\ProjectPredation-%PRESET%.zip"

rem Run from the repository root, because cmake --preset reads CMakePresets.json out of the working
rem directory and nothing else. Every path below is absolute, so this is only about that lookup --
rem but without it the script works when it is run from the root and fails when it is double-clicked,
rem which is the one way it is meant to be used. build.cmd and test.cmd already did this; this one
rem did not, and the error it produced blamed a missing presets file in Scripts\Windows.
rem
rem setlocal restores the working directory on exit, so there is nothing to undo.
pushd "%ROOT%"

call "%~dp0vsenv.cmd"
if errorlevel 1 (
    echo [package] FAIL: Visual Studio environment setup failed
    exit /b 1
)

echo [package] Building %PRESET%...
cmake --preset %PRESET%
if errorlevel 1 (
    echo [package] FAIL: configure failed
    exit /b 1
)

rem What gets handed out must not have the model editor in its menu or the editor commands in its
rem console: they are for building the game, not for playing it. The preset asks for that; this
rem checks it actually happened, because a stale cache is silent and a leaked editor is not obvious
rem from the outside.
"%SystemRoot%\System32\find.exe" "PRED_DEV_TOOLS:BOOL=OFF" "%BUILD_DIR%\CMakeCache.txt" >nul
if errorlevel 1 (
    echo [package] FAIL: %PRESET% still has the developer tools switched on.
    echo [package]       Package the windows-shipping preset, or delete %BUILD_DIR% and retry.
    exit /b 1
)

cmake --build --preset %PRESET%
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
rem And the relay, which is not for players but is for whoever is going to run one. A quarter of a
rem megabyte, and the alternative is a second download nobody can find when they need it.
if exist "%BUILD_DIR%\bin\PredationRelay.exe" copy /y "%BUILD_DIR%\bin\PredationRelay.exe" "%STAGE%\" >nul

rem The data files, and then the compiled shaders on top of them. Both end up under Assets, which
rem is the first place the game looks, so the folder runs anywhere without the build tree.
xcopy /e /i /q /y "%ROOT%\Assets" "%STAGE%\Assets" >nul
rem The raw downloads the model editor imports from are tens of megabytes and no use without it.
if exist "%STAGE%\Assets\Models\Source" rmdir /s /q "%STAGE%\Assets\Models\Source"
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
    echo If it will not start, install the Microsoft Visual C++ Redistributable for x64.
    echo.
    echo.
    echo PLAYING TOGETHER, OVER THE INTERNET
    echo.
    echo Neither of you has to forward a port or change a router setting. One of you
    echo gets a six character code and everybody else types it in.
    echo.
    echo   1. One of you presses "Open a game", then "Over the internet".
    echo   2. That gives a code like 7KMQX3. Send it to the others.
    echo   3. They press "Join a game", type it in, and press Join.
    echo.
    echo People can join at any time, before or after the host goes in.
    echo.
    echo This needs a relay: one machine both of you can reach, which forwards
    echo between you. PredationRelay.exe in this folder is it. Whoever is running it
    echo needs a machine the others can reach - a cheap virtual server, a free tier,
    echo or a home machine with UDP forwarded to it - and everybody else points the
    echo game at it from the console with:
    echo.
    echo   net.relay_host your.relay.address
    echo.
    echo That is remembered, so it only has to be typed once.
    echo.
    echo PLAYING TOGETHER, SAME HOUSE
    echo.
    echo   The host presses "Open a game", then "On this network", and reads
    echo   out the address shown, something like 192.168.1.20:27015.
    echo   Everyone else types that into "Join a game".
    echo.
    echo Windows will ask once whether to let the game through the firewall. Say yes
    echo to both boxes.
    echo.
    echo.
    echo CONTROLS
    echo.
    echo   WASD move, Space jump, Ctrl or C crouch, Z prone, Shift sprint, Alt walk
    echo   Q and E lean, left mouse fire, right mouse aim, R reload
    echo   F interact, G drop, 1-6 and the wheel select, Tab inventory
    echo   P cycles first person, third person and free camera
    echo   V talk to anybody near you
    echo   F3 shows frame time and ping, Escape pauses
    echo.
    echo Crouch and prone are hold-to-activate. There is a toggle for that in Settings,
    echo on the pause menu and on the title screen.
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
