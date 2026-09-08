# Building Project Predation

Primary target: Windows 10/11, x64, MSVC 2022, C++20.

## 1. Prerequisites

| Tool | Version | Install |
|---|---|---|
| Visual Studio 2022 Build Tools with the "Desktop development with C++" workload (MSVC v143, Windows SDK) | 17.x | `winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"` |
| CMake | 3.28+ | `winget install --id Kitware.CMake --scope user` |
| Ninja | 1.11+ | `winget install --id Ninja-build.Ninja --scope user` |
| Git with Git LFS | 2.40+ | `winget install --id Git.Git` then `git lfs install` |
| vcpkg | any recent | `git clone https://github.com/microsoft/vcpkg.git C:\Dev\vcpkg` then `C:\Dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics` |

Set the `VCPKG_ROOT` environment variable to the vcpkg checkout (the scripts default to `C:\Dev\vcpkg`):

```
setx VCPKG_ROOT C:\Dev\vcpkg
```

The Build Tools installer needs administrator rights. The other tools install per-user.

## 2. Build from the command line

The helper scripts locate MSVC through `vswhere`, set up the x64 environment, and drive CMake presets:

```
Scripts\build.cmd                    configure + build   (windows-debug)
Scripts\build.cmd windows-release    other presets: windows-relwithdebinfo, windows-release
Scripts\test.cmd                     run unit tests through CTest
Scripts\run.cmd                      launch the game
```

The first configure builds every dependency through vcpkg (SDL3, bgfx, ImGui, spdlog, Catch2, ...).
Expect 15 to 30 minutes on a laptop. Later configures reuse the vcpkg binary cache.

Equivalent manual steps from an "x64 Native Tools Command Prompt for VS 2022":

```
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
build\windows-debug\bin\ProjectPredation.exe
```

## 3. VS Code

Recommended extensions are listed in `.vscode/extensions.json` (CMake Tools, C/C++). With CMake Tools:

1. Open the repository folder.
2. Pick a configure preset when prompted (`Windows Debug`).
3. Build with F7, launch with the "Project Predation" configuration in `.vscode/launch.json`.

CMake Tools sets up the MSVC environment itself, so no developer prompt is needed inside VS Code.

## 4. Build directory and OneDrive

By default the build lands in `build/<preset>`. If the repository lives inside a OneDrive folder, keep build
output out of the synced tree by creating a `CMakeUserPresets.json` (git-ignored) next to `CMakePresets.json`.
User presets must use new names; they cannot override a name that `CMakePresets.json` already defines:

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "local-debug", "inherits": "windows-debug", "binaryDir": "C:/Dev/build/Project-Predation/${presetName}" }
  ],
  "buildPresets": [ { "name": "local-debug", "configurePreset": "local-debug" } ],
  "testPresets": [ { "name": "local-debug", "configurePreset": "local-debug", "output": { "outputOnFailure": true } } ]
}
```

Then build with `Scripts\build.cmd local-debug` and run with `set PRED_BUILD_DIR=C:\Dev\build\Project-Predation\local-debug` before `Scripts\run.cmd`.

## 5. Triplets and linking

The presets use the `x64-windows-static-md` triplet: static third-party libraries with the dynamic CRT.
The result is a single executable with no DLLs to ship, and no CRT mismatch problems.

The bgfx tools (`shaderc`) are built for the host triplet `x64-windows` and used at build time to compile
every `Engine/Shaders/**/*.sc` into `build/<preset>/GeneratedAssets/Shaders/<profile>/`.

## 6. Command line options

```
--assets <dir>        Use this Assets directory
--frames <n>          Exit after n frames
--screenshot <file>   Save a PNG before exiting (use with --frames, n >= 4)
--backend <name>      auto | dx11 | dx12 | vulkan | opengl
--log-level <level>   trace | debug | info | warn | error | critical | off
--set <cvar>=<value>  Override a cvar
+<cvar> <value>       Same as --set
```

Example smoke test that needs no interaction:

```
build\windows-debug\bin\ProjectPredation.exe --frames 30 --screenshot build\shot.png
```

## 7. Where things go at runtime

| What | Where |
|---|---|
| Logs | `%APPDATA%\ACRD\ProjectPredation\Logs\predation.log` |
| User settings (archived cvars) | `%APPDATA%\ACRD\ProjectPredation\settings.json` |
| Screenshots (F12 or `screenshot`) | `%APPDATA%\ACRD\ProjectPredation\Screenshots\` |
| ImGui layout | `%APPDATA%\ACRD\ProjectPredation\imgui.ini` |

## 8. Troubleshooting

- **"bgfx shaderc not found"**: the vcpkg host build of bgfx with the `tools` feature failed or was skipped.
  Check `vcpkg_installed/x64-windows/tools/bgfx/shaderc.exe` under the build directory.
- **"Shader not found: Shaders/dx11/..."**: the shader target did not run. Build the `PredationEngineShaders`
  target, and make sure the executable can find `GeneratedAssets` (the path is baked in at configure time).
- **SDL_CreateWindow failed**: run with `--backend dx11` and check the log; on remote desktop sessions the
  D3D11 backend is the most reliable.
- **Very slow builds**: the repository or build directory is inside OneDrive. See section 4.
