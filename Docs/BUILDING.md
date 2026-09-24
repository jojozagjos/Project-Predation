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
Scripts\Windows\build.cmd                    configure + build   (windows-debug)
Scripts\Windows\build.cmd windows-release    other presets: windows-relwithdebinfo, windows-release
Scripts\Windows\test.cmd                     run unit tests through CTest
Scripts\Windows\run.cmd                      launch the game
Scripts\Windows\smoke.cmd                    headless 60-frame run: screenshot + log check
```

`smoke.cmd` is the fastest way to answer "is it still fundamentally working?" without looking at a window.
It runs the game headless for 60 frames, requires a real screenshot to come out the other side, and fails if
anything logged an error. Run it before every commit.

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

## 4. Where the repository should live

Keep the working copy on a **local, non-synced path**. The reference checkout is `C:\Dev\Project-Predation`.

Do not put it inside OneDrive, Dropbox, or a similar synced folder. A C++ build writes tens of thousands of
intermediate files; a sync client will try to upload every one of them, slow the build to a crawl, and
occasionally hold a file open at the moment the compiler wants to replace it, producing failures that look
like random compiler bugs.

The build lands in `build/<preset>` inside the repository, which `.gitignore` excludes.

If you ever do need the build output somewhere else, add a git-ignored `CMakeUserPresets.json` next to
`CMakePresets.json`. User presets must use **new** names; they cannot redefine one that `CMakePresets.json`
already declares:

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "local-debug", "inherits": "windows-debug", "binaryDir": "D:/build/Project-Predation/${presetName}" }
  ],
  "buildPresets": [ { "name": "local-debug", "configurePreset": "local-debug" } ],
  "testPresets": [ { "name": "local-debug", "configurePreset": "local-debug", "output": { "outputOnFailure": true } } ]
}
```

Then build with `Scripts\Windows\build.cmd local-debug`, and set `PRED_BUILD_DIR` before `Scripts\Windows\run.cmd`.

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
| Logs | `%APPDATA%\CIRRA\ProjectPredation\Logs\predation.log` |
| User settings (archived cvars) | `%APPDATA%\CIRRA\ProjectPredation\settings.json` |
| Screenshots (F12 or `screenshot`) | `%APPDATA%\CIRRA\ProjectPredation\Screenshots\` |
| ImGui layout | `%APPDATA%\CIRRA\ProjectPredation\imgui.ini` |

## 8. Troubleshooting

- **"bgfx shaderc not found"**: the vcpkg host build of bgfx with the `tools` feature failed or was skipped.
  Check `vcpkg_installed/x64-windows/tools/bgfx/shaderc.exe` under the build directory.
- **"Shader not found: Shaders/dx11/..."**: the shader target did not run. Build the `PredationEngineShaders`
  target, and make sure the executable can find `GeneratedAssets` (the path is baked in at configure time).
- **SDL_CreateWindow failed**: run with `--backend dx11` and check the log; on remote desktop sessions the
  D3D11 backend is the most reliable.
- **Very slow builds**: the repository or build directory is inside a synced folder. See section 4.
- **`'vswhere.exe' is not recognized`**: harmless. Microsoft's own `vcvarsall.bat` prints it. `vsenv.cmd`
  silences it.
- **vcpkg using the wrong copy of itself**: `vcvarsall.bat` overwrites `VCPKG_ROOT` with the vcpkg bundled
  inside Visual Studio. `vsenv.cmd` saves and restores the value so your `C:\Dev\vcpkg` wins. If you set up
  the environment by hand, re-set `VCPKG_ROOT` *after* calling `vcvarsall.bat`.

## 9. The two builds

| Preset | What it is |
|---|---|
| `windows-debug` | Everyday development. Assertions on, no optimisation, slow. |
| `windows-relwithdebinfo` | Optimised but debuggable. For chasing something that only happens fast. |
| `windows-release` | Optimised. What you play while working on the game. |
| `windows-shipping` | What other people get. Same code with `PRED_DEV_TOOLS=OFF`. |

The first three are developer builds: the model editor is on the title screen and its commands are
in the console. The shipping build has neither, and the packaging script leaves out the raw model
downloads the importer works on. See ADR-031 and ADR-049.

Each preset has its own folder under `build/`, including the shipping one. That separation is not
cosmetic: `PRED_DEV_TOOLS` is a cached CMake variable, so producing a shipping exe inside a
developer folder leaves that folder without its tools until the cache is cleared, and nothing says
so. The developer presets pin the variable back on for the same reason.

To tell which one is running: the first line of the log says so, and the developer build says
"developer build" beside the byline on the title screen.

All presets share one vcpkg tree at `build/vcpkg_installed`, set by `VCPKG_INSTALLED_DIR` in the
base presets. vcpkg's default is a tree per build folder, which here was 3.1 GB each and byte for
byte the same. A triplet's tree already contains both the debug and the release libraries, so there
is nothing to keep apart.

## 10. What is in build, and why there is so much of it

```
build/vcpkg_installed/     the dependencies: SDL, bgfx, Jolt, spdlog and the rest
build/windows-debug/       one folder per preset you have configured
build/windows-release/
build/windows-shipping/
dist/                      what you send somebody: the folder and the zip
```

Only two kinds of thing are in there. `vcpkg_installed` is the libraries the game is built against,
built once and shared by every preset; everything else is one folder per build you have made.
`dist` is deliberately not under `build`, because the thing you hand somebody is not another build.

Rough sizes: the dependencies are about 3 GB, a debug build is about 1 GB, and an optimised build
is about 120 MB. Most of the 3 GB is the host tree that exists to produce one program, the bgfx
shader compiler; it is the price of building the dependencies from source instead of trusting a
binary someone else made.

None of it is in git. Deleting `build` entirely costs a full rebuild and nothing else.

```
Scripts\Windows\clean.cmd        vcpkg's staging area: a few gigabytes, nothing needs it
Scripts\Windows\clean.cmd all    also every preset folder, keeping the dependencies
```

`clean.cmd all` is the one to reach for when a build folder is not needed for a while: the preset
comes back with one `build.cmd`, and because the dependency tree stays it is a compile rather than
a download.


## Sending it to somebody

```
Scripts\Windows\package.cmd
```

Builds two of them:

| Zip | Built from | What is in it |
|---|---|---|
| `dist\ProjectPredation.zip` | `windows-shipping` | The game. No console, no model editor, no debug commands. |
| `dist\ProjectPredation-dev.zip` | `windows-relwithdebinfo` | The same game with the console, the debug windows and the editor still in it. |

Each folder holds the executable, the data files, and the shaders the build compiled, all under an
`Assets` folder beside the exe, which is the first place the game looks. It runs from anywhere with
nothing else installed except the Microsoft Visual C++ Redistributable for x64. A `README.txt` goes
in the folder with the controls and how to connect; the dev one also lists the console commands
worth knowing and says where the log is.

`package.cmd windows-shipping` or `package.cmd windows-relwithdebinfo` does one of them on its own.

The script checks the CMake cache actually says what the preset asked for -- `PRED_DEV_TOOLS:BOOL=OFF`
for the shipping build, `ON` for the dev one -- before it stages anything, and stops if it does not.
A stale cache is silent, and neither an editor that leaked into somebody else's copy nor a dev build
with no console in it is visible from the outside.

Neither zip carries the `.pdb`. It is forty-five megabytes, it makes the dev zip ten times the size
of the game, and nothing reads it: there is no crash handler writing a dump for it to name the
frames of. What a tester sends back is the log.

To play over the internet, the host presses **Play → Host a game → Start** and sends the code the
lobby shows; the other person types it into the box at the top of **Play**. That needs a lobby
server, which introduces the two PCs so they can connect straight to each other (HOSTING.md,
SERVER.md). On one network the game appears in the list without one.

## The lobby server

It is not part of the game's build: it is a Cloudflare Worker in `Tools/LobbyWorker`, a few hundred
lines of JavaScript, deployed from GitHub (SERVER.md walks through it). GitHub runs its tests after
every change to it.

To run it on this PC for testing -- plain Node, nothing to install:

```
node Tools/LobbyWorker/local-server.js
```

and in each copy of the game's console, `lobby_use http://127.0.0.1:8787 127.0.0.1:3478`.
