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
rem Builds the game and lays out a folder somebody else can run, then zips it.
rem
rem Two of them by default. The shipping build is what you hand to somebody who wants to play; the
rem dev build is the same game with the console, the model editor and the debug commands still in
rem it, which is what you want when the person you are sending it to is going to tell you what went
rem wrong. They are separate builds in separate folders because the developer tools are a cached
rem CMake variable: one folder cannot hold both answers, and on the day it seems to, one of the two
rem is stale.
rem
rem The game finds its assets in an "Assets" folder next to the executable, so packaging is a copy
rem rather than a build step: the exe, the data files, and the shaders the build compiled.
rem
rem Usage: Scripts\Windows\package.cmd [preset]
rem        no preset  -- both, windows-shipping and then windows-relwithdebinfo
setlocal EnableDelayedExpansion

set "ROOT=%~dp0..\.."

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

if not "%~1"=="" (
    call :one "%~1"
    exit /b !errorlevel!
)

call :one windows-shipping
if errorlevel 1 exit /b 1
call :one windows-relwithdebinfo
if errorlevel 1 exit /b 1
exit /b 0


rem --- One preset, from configure to zip ----------------------------------------------------------
:one
set "PRESET=%~1"
set "BUILD_DIR=%ROOT%\build\%PRESET%"

rem What the folder inside the zip is called. The plain name is the one to hand out; anything with
rem the developer tools in it says so on the tin, so the two cannot be mixed up on somebody else's
rem desktop.
if /i "%PRESET%"=="windows-shipping" (
    set "NAME=ProjectPredation"
    set "WANT_TOOLS=OFF"
) else (
    set "NAME=ProjectPredation-dev"
    set "WANT_TOOLS=ON"
)
set "STAGE=%ROOT%\dist\!NAME!"
set "ZIP=%ROOT%\dist\!NAME!.zip"

echo.
echo [package] === %PRESET% ===
cmake --preset %PRESET%
if errorlevel 1 (
    echo [package] FAIL: configure failed
    exit /b 1
)

rem What gets handed out must have the tools its preset asked for and no others: no model editor in
rem the menu and no editor commands in the console for the shipping build, and the console still
rem there in the dev one. The preset says which; this checks it actually happened, because a stale
rem cache is silent and neither a leaked editor nor a missing console shows from the outside.
"%SystemRoot%\System32\find.exe" "PRED_DEV_TOOLS:BOOL=!WANT_TOOLS!" "%BUILD_DIR%\CMakeCache.txt" >nul
if errorlevel 1 (
    echo [package] FAIL: %PRESET% does not have PRED_DEV_TOOLS=!WANT_TOOLS!.
    echo [package]       Delete %BUILD_DIR% and run this again.
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

echo [package] Laying out !STAGE!...
if exist "!STAGE!" rmdir /s /q "!STAGE!"
mkdir "!STAGE!" 2>nul

rem The executable and anything the build deployed beside it.
copy /y "%BUILD_DIR%\bin\ProjectPredation.exe" "!STAGE!\" >nul
for %%F in ("%BUILD_DIR%\bin\*.dll") do copy /y "%%F" "!STAGE!\" >nul 2>nul

rem Not the .pdb, in either build. It is forty-five megabytes of symbol names, it makes the dev zip
rem ten times the size of the game, and nothing in the build reads it: there is no crash handler
rem writing a dump for it to name the frames of. What a tester sends back is the log. The symbols
rem stay in the build folder, where a debugger attached here can find them.

rem The data files, and then the compiled shaders on top of them. Both end up under Assets, which
rem is the first place the game looks, so the folder runs anywhere without the build tree.
xcopy /e /i /q /y "%ROOT%\Assets" "!STAGE!\Assets" >nul
rem The raw downloads the model editor imports from are tens of megabytes and no use without it.
if exist "!STAGE!\Assets\Models\Source" rmdir /s /q "!STAGE!\Assets\Models\Source"
if exist "%BUILD_DIR%\GeneratedAssets\Shaders" (
    xcopy /e /i /q /y "%BUILD_DIR%\GeneratedAssets\Shaders" "!STAGE!\Assets\Shaders" >nul
) else (
    echo [package] FAIL: no compiled shaders in %BUILD_DIR%\GeneratedAssets\Shaders
    exit /b 1
)

> "!STAGE!\README.txt" (
    echo PROJECT PREDATION
    echo CIRRA // Critical Incident Response ^& Research Agency
    echo.
    echo Run ProjectPredation.exe.
    echo.
    echo If it will not start, install the Microsoft Visual C++ Redistributable for x64.
    echo.
    echo.
    echo PLAYING TOGETHER
    echo.
    echo   1. One of you presses Play, then "Host a game", then Start.
    echo   2. The lobby shows a six character code like 7KMQX3. Send it to the others.
    echo   3. They press Play, type the code into the box at the top, and press Join.
    echo   4. When everybody is in the lobby, the host presses "Start the game".
    echo.
    echo Nobody has to forward a port or change a router setting. The code works from
    echo anywhere, and on the same wifi the game also shows up in "On your network".
    echo People can still join after the game has started.
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

if /i not "!WANT_TOOLS!"=="OFF" (
    >> "!STAGE!\README.txt" (
        echo.
        echo.
        echo THIS IS THE DEVELOPER BUILD
        echo.
        echo The same game with the tools left in. It is a little slower than the one
        echo people play and it writes a great deal more to its log.
        echo.
        echo   The key left of 1 opens the console. Type help.
        echo   F3   frame time, ping and network counters
        echo   F4   the debug windows: AI, audio, physics, rendering
        echo.
        echo Console commands worth knowing:
        echo.
        echo   spawn_creature       one creature in front of you
        echo   lab                  the test map: lockers, doors, a balcony, a crawlspace
        echo   testmap              back to the ordinary map
        echo   ai.freeze 1          stop every creature where it stands
        echo   debug.navigation 1   draw where creatures can walk and jump
        echo   creature_mind        what the creature you are looking at is thinking
        echo   net_host, net_join   open a game, or join one at an address
        echo.
        echo The log is at %%APPDATA%%\CIRRA\ProjectPredation\Logs\predation.log. Send that
        echo with any report: it is the whole session rather than only the crash.
    )
)

echo [package] Zipping...
if exist "!ZIP!" del /q "!ZIP!"
powershell -NoProfile -Command "Compress-Archive -Path '!STAGE!' -DestinationPath '!ZIP!' -Force"
if errorlevel 1 (
    echo [package] FAIL: could not create the zip
    exit /b 1
)

for %%F in ("!ZIP!") do set "ZIPSIZE=%%~zF"
echo [package] PASS %PRESET%
echo [package]   folder: !STAGE!
echo [package]   zip:    !ZIP! ^(!ZIPSIZE! bytes^)
exit /b 0
