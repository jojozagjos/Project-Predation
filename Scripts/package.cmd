@echo off
rem Double-clicked from Explorer, a script window closes the instant the script finishes and
rem takes everything it printed with it, so a run that worked and a run that failed look exactly
rem the same: nothing. Run again under a shell that waits, and say so at the end.
if defined PRED_KEEP_OPEN goto :pred_body
rem Explorer launches a double-clicked script by its full path; anybody typing one at a prompt or
rem calling it from another script uses a relative one or no quotes, so the full path is what tells
rem the two apart. PRED_NO_PAUSE opts out of it for anything automated that does use a full path.
if defined PRED_NO_PAUSE goto :pred_body
echo %cmdcmdline% | find /i "%~f0" >nul || goto :pred_body
set "PRED_KEEP_OPEN=1"
call "%~f0" %*
echo.
echo [%~n0] finished with code %errorlevel%. Press any key to close this window.
pause >nul
exit /b %errorlevel%
:pred_body
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

call "%~dp0vsenv.cmd"
if errorlevel 1 (
    echo [package] FAIL: Visual Studio environment setup failed
    exit /b 1
)

rem Configured without the developer tools. What somebody else is handed has no model editor in
rem its menu and no editor commands in its console: they are for building the game, not playing it.
echo [package] Building %PRESET% without the developer tools...
cmake --preset %PRESET% -DPRED_DEV_TOOLS=OFF
if errorlevel 1 (
    echo [package] FAIL: configure failed
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
    echo Playing together:
    echo   One person picks "Open a game" and tells the others their address.
    echo   Everyone else types that address into "Join a game" and presses Join.
    echo   The host is shown two kinds of address. The local one, something like
    echo   192.168.1.20, reaches people on the same network. The one marked "from
    echo   anywhere" appears when the router agrees to forward the port, and is the
    echo   one to give somebody elsewhere.
    echo   If no such address appears, the router has UPnP switched off or there is
    echo   more than one router in the way. Forward UDP 27015 to the host machine
    echo   by hand, or play on one network. There is no matchmaking yet.
    echo.
    echo Controls:
    echo   WASD move, Space jump, Ctrl or C crouch, Z prone, Shift sprint, Alt walk
    echo   Q and E lean, left mouse fire, right mouse aim, R reload
    echo   F interact, G drop, 1-6 and the wheel select, Tab inventory
    echo   P cycles first person, third person and free camera
    echo   F3 debug overlay, backtick console, Escape pauses
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
