# Architecture

This document describes what exists now and the shape of what comes next. Sections marked
**(planned)** are design, not code. Update this file when a major system lands.

## Layers

```
+------------------------------------------------------------------------+
|  GAME  (Game/)                                                         |
|  Player | Weapons | Interaction | Creature | Missions | Session        |
|  Game debug panels: AI brain, nav, anim, spawn, console commands       |
+------------------------------------------------------------------------+
|  ENGINE FRAMEWORK  (Engine/)                                           |
|  Scene/Entities  Assets  Animation  Navigation  Network                |
|  Render  Physics  Audio  Debug  UI                                     |
|  Core: log, config, cvars, time, paths, jobs, events, properties        |
+------------------------------------------------------------------------+
|  PLATFORM AND THIRD PARTY                                              |
|  SDL3  bgfx  Jolt  miniaudio  Recast  ENet  ImGui  spdlog  Tracy       |
+------------------------------------------------------------------------+
```

Rules:

- Game code depends on Engine. Engine never depends on Game.
- Game code never calls bgfx, SDL, or Jolt directly. It goes through `Renderer`, `Window`/`Input`, and the
  physics wrapper. Swapping a backend is then an engine change, not a game rewrite.
- Every tunable value is a cvar (engine/application settings) or lives in a JSON data file
  (gameplay tuning). Nothing worth changing at runtime is a bare constant in C++.
- Debug tooling is part of each system's definition of done, not a later polish pass.

## Frame loop (implemented)

```
Application::Run
  FrameStats.BeginFrame
  dt = clock.Tick()                         clamped to 0.25 s
  PumpEvents                                SDL events -> hotkeys, ImGui, Input, Game::OnEvent
  FixedUpdate  x N                          N = accumulator steps at app.fixed_hz (max 5, then drop time)
      Game::OnFixedUpdate(step)             simulation: movement, AI, missions, physics (later)
  Game::OnUpdate(dt, alpha)                 camera, animation params, audio listener
  Renderer.BeginFrame                       view rects, clear, camera matrices
  Game::OnRender                            submit geometry, debug draw
  DebugDraw.Flush
  ImGui: overlay, console, Game::OnImGui
  Renderer.EndFrame                         bgfx::frame (screenshot request if pending)
  FileWatcher.Poll                          hot reload of config and bindings
  FrameStats.EndFrame
```

Two deliberate properties. Simulation runs on a fixed tick so host and clients execute identical movement
code, which network prediction requires. Camera rotation is sampled per frame outside the tick so aiming never
feels tied to the simulation rate.

## Engine modules

### Core (implemented)

| File | Responsibility |
|---|---|
| `Core/Log` | spdlog wrapper with one logger per category: ENGINE, PLATFORM, RENDER, AUDIO, NETWORK, AI, ANIMATION, PHYSICS, GAMEPLAY, ASSET, DEBUG. Sinks: colored console, file, in-game console. |
| `Core/CVar` | Typed console variables with flags (Archive, ReadOnly, Cheat, Hidden), change callbacks, and a registry that holds values for cvars that register later. |
| `Core/Config` | Layered JSON configuration on top of cvars: code defaults < `Assets/Config/defaults.json` < user `settings.json` < command line < console. Saves Archive cvars. |
| `Core/Time` | `FrameClock` and `FixedStepAccumulator`. |
| `Core/Paths` | Executable, assets, and user data directories; ordered asset search roots; `Resolve()`. |
| `Core/SystemInfo` | Process memory, CPU name, RAM, thread count. |

### Platform (implemented)

`Window` wraps an SDL3 window (native handle, pixel size, relative mouse). `Input` tracks raw key and mouse
state and maps named actions from `Assets/Config/input.json`. Input can be blocked when the console or an
ImGui widget captures the keyboard or mouse; raw queries bypass blocking for global hotkeys.

### Render (bootstrap implemented)

`Renderer` owns the bgfx device: init, resize, vsync, clear, camera matrices for the main and debug views,
stats, and PNG screenshots via a bgfx callback. `ShaderLibrary` loads compiled shaders from
`Shaders/<profile>/<name>.bin` through the asset search roots. `DebugDraw` batches colored lines into one
transient buffer per frame. `FlyCamera` is the developer camera.

View ids: 0 main scene, 1 debug lines, 250 ImGui. Views render in id order.

**(planned)** Static and skinned meshes from glTF, PBR materials, clustered or deferred lighting, shadow maps
for spot and point lights, cascaded shadows for exteriors, SSAO, volumetric fog, bloom, tonemapping, TAA or
SMAA, decals. Tracy GPU zones through bgfx profiler callbacks.

### Debug (implemented)

| File | Responsibility |
|---|---|
| `Debug/ImGuiLayer` | ImGui context, SDL3 platform backend, bgfx renderer backend with dynamic texture support. |
| `Debug/DebugOverlay` | F3 overlay: FPS, frame time graph, CPU/GPU time, per-phase timings, fixed step info, renderer stats, memory, debug category toggles. |
| `Debug/Console` | Command registry, cvar get/set, log mirror, history, tab completion. |
| `Debug/DebugCategories` | AI, NAVIGATION, PERCEPTION, ANIMATION, PHYSICS, NETWORK, RENDERING, AUDIO, PLAYER toggles backed by `debug.*` cvars. |
| `Debug/FrameStats` | Frame history ring and scoped CPU timers (`PRED_PROFILE_SCOPE`). |

### Assets (bootstrap implemented)

`FileWatcher` polls watched files and fires callbacks on change. Config and input bindings hot reload today.

**(planned)** Asset registry with path-based ids, glTF import via fastgltf, texture loading via stb_image and
KTX2, audio banks, data files for creatures, weapons, items, and missions, hot reload for all of them.

### Physics, Audio, Animation, Navigation, Network, Scene **(planned)**

See [DESIGN_PLAN.md](DESIGN_PLAN.md) sections 7 through 11 and the system documents
[NETWORKING.md](NETWORKING.md), [AI.md](AI.md), [ANIMATION.md](ANIMATION.md).

## Entity model (planned)

In-house: generational entity handles, typed component pools, and systems run in an explicit order. Components
may be rich C++ objects. A component registry feeds the inspector, serialization, and replication. Deliberately
small; EnTT is the fallback if it grows past a few hundred lines.

## Game modules (planned)

`Player`, `Weapons`, `Interaction`, `Creature`, `Missions`, `Session`, and `Debug` (game-specific panels and
console commands such as `spawn_creature <seed>`). Milestone 1 contains only `PredationGame`, which drives the
fly camera and draws a reference scene.

## Conventions

- Namespace `pred`. Types and functions PascalCase, members `m_`, constants `k`.
- Includes are repository-relative: `#include "Engine/Core/Log.h"`.
- One responsibility per file, header and source side by side.
- Errors are logged with a category and a useful message. Nothing fails silently.
- `.clang-format` defines formatting (Allman braces, 4 spaces, 120 columns).
