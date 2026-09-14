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
rem Usage: Scripts\Windows\linux-build.cmd [preset]      default preset: linux-release
rem
rem Builds the Linux version from this Windows machine, inside a container. You cannot produce a
rem Linux binary with MSVC, so the only honest way to know whether the port compiles is to run a
rem Linux compiler over it.
rem
rem The source is mounted from here, so it builds what is on disk right now, uncommitted changes
rem included. The build tree and the dependencies live in a Docker volume rather than in build\,
rem because they are Linux artifacts, they are gigabytes, and a bind mount into Windows is slow
rem enough that it would dominate the build.

setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=linux-release"

set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

docker version >nul 2>&1
if errorlevel 1 (
    echo [linux] Docker is not running. Start Docker Desktop and try again.
    exit /b 1
)

echo [linux] Building the image ^(quick after the first time^)...
docker build -t predation-linux -f "%ROOT%\Scripts\Linux\Docker\Dockerfile" "%ROOT%\Scripts\Linux\Docker"
if errorlevel 1 (
    echo [linux] FAIL: could not build the image
    exit /b 1
)

echo [linux] Configuring and building %PRESET%...
docker run --rm ^
    -v "%ROOT%":/src ^
    -v predation-linux-build:/build ^
    predation-linux ^
    bash -c "set -e; cmake --preset %PRESET% -B /build/%PRESET% -DVCPKG_INSTALLED_DIR=/build/vcpkg_installed && cmake --build /build/%PRESET%"
if errorlevel 1 (
    echo [linux] FAIL: the Linux build did not succeed. The first error above is the one that matters.
    exit /b 1
)

echo [linux] Running the tests...
docker run --rm ^
    -v "%ROOT%":/src ^
    -v predation-linux-build:/build ^
    predation-linux ^
    bash -c "cd /build/%PRESET% && ctest --output-on-failure"
set "RC=%errorlevel%"

echo.
if "%RC%"=="0" (
    echo [linux] PASS: it compiles on Linux and the tests pass.
) else (
    echo [linux] The build succeeded but the tests did not all pass.
)
echo [linux] The binary is inside the container volume, at /build/%PRESET%/bin.
echo [linux] To copy it out:
echo [linux]   Scripts\Windows\linux-build.cmd %PRESET% ^&^& docker run --rm -v predation-linux-build:/build -v "%%CD%%":/out predation-linux cp /build/%PRESET%/bin/ProjectPredation /out/
exit /b %RC%
