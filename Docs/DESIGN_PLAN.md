# Project Predation: Design Plan

Approved 2026-09-08. This is the plan the codebase follows. The original brief's requirements are the source
of truth; this document records the interpretation, the technology choices, the roadmap, and the
recommendations that were raised. Recommendations are labeled as such.

## 1. Summary

Project Predation is a 1 to 4 player first-person survival horror extraction game on a custom C++ engine,
Windows first. Players are field teams of a clinical containment organization dropped into a facility that
contains one alien organism. The organism is generated from a seed: anatomy, capabilities, senses, and
temperament are reproducible, but its behavior drifts with experience during a mission. Horror comes from
uncertainty, sound, isolation, and a creature that stalks, hides, ambushes, captures, and adapts.

Priorities in order: player movement and full-body embodiment, atmosphere and audio, readable but unpredictable
creature behavior, multiplayer tension, physical firearms, polished menus and settings, performance, and
debugging tools built from the start. The creature is the signature feature but is built last, on a proven
game. Every system is inspectable, data-driven, and small enough to understand.

## 2. Organization

**ACRD: Anomalous Containment & Research Directorate.** In-world shorthand: "the Directorate". Field units:
Recovery Teams.

Why: "Directorate" implies orders from above with no explanation. "Anomalous" never says "alien". "Containment &
Research" tells the player the organization wants the organism alive, which is a gameplay hook and quietly
unsettling: teams are replaceable, specimens are not.

Other candidates considered: OBCA Office of Biological Containment Affairs; DAOR Division of Aberrant
Organism Response; ICOS Interagency Commission on Organism Security; SIRC Specimen Isolation & Recovery
Command; ASOC Adaptive Specimen Oversight Commission; BDB Bureau of Divergent Biology; XRCB Xenobiological
Recovery & Containment Bureau; CBRA Containment & Biological Response Authority; NTOC Non-Terrestrial
Organism Commission.

## 3. Technology stack

Compiler: MSVC 2022, C++20, x64. Clang-cl later for a second opinion on warnings.

| Area | Pick | License | Why | Alternatives |
|---|---|---|---|---|
| Build | CMake 3.28+ presets, Ninja | BSD | One command from a fresh clone | Premake, Meson |
| Packages | vcpkg manifest | MIT | Every library is one JSON line, versions locked | Conan, submodules |
| Window, input | SDL3 | zlib | Window, keyboard, relative mouse, gamepad, high-DPI, portable | GLFW, Win32 |
| Rendering backend | bgfx | BSD-2 | Multi-backend GPU abstraction, GPU timers, mature | Diligent, SDL3 GPU, Vulkan, D3D11 |
| Physics | Jolt | MIT | Modern, multithreaded, shape casts, character collision, ragdolls | PhysX 5, Bullet |
| Audio device | miniaudio | MIT-0 | Devices, decoding, mixing graph | FMOD, Wwise, SoLoud, OpenAL Soft |
| Voice codec | libopus | BSD-3 | The codec for game voice | Speex |
| Debug UI | Dear ImGui, ImPlot | MIT | Every inspector, editor, profiler graph | Nuklear |
| Transport | ENet | MIT | Tiny reliable/unreliable UDP behind our interface | GameNetworkingSockets, Steam, yojimbo |
| Data | nlohmann/json | MIT | Human-readable, diffable | toml++, yaml-cpp |
| Packets | in-house bit writer | ours | Game-specific, size matters | FlatBuffers, protobuf |
| Math | GLM | MIT | Header-only, GLSL conventions | DirectXMath |
| Images | stb_image | MIT/PD | PNG, JPG, HDR; KTX2 later | FreeImage |
| Models | fastgltf | MIT | glTF 2.0 with skins, animations, PBR | cgltf, Assimp |
| Navigation | Recast/Detour | zlib | Navmesh generation, pathfinding, off-mesh links | own |
| Logging | spdlog | MIT | Fast, named categories, sinks | own |
| Profiling | Tracy | BSD-3 | Frame profiler, CPU, memory, GPU zones | Optick, Superluminal |
| Tests | Catch2 v3 | BSL-1.0 | Expressive, CTest integration | doctest, GoogleTest |
| Jobs | enkiTS | zlib | Small task scheduler | own pool |
| Compression | zstd | BSD | Cooked assets, large payloads, later | LZ4 |
| Entities | in-house | ours | Handles, component pools, explicit order | EnTT |

Rendering rationale: raw Vulkan or D3D12 costs weeks before a lit mesh and slows every feature. D3D11 is simple
but Windows-only. bgfx handles the GPU API and shader cross-compilation with a decade of production use. What
matters visually (deferred or clustered lighting, shadow maps, PBR, SSAO, volumetric fog, bloom, tonemapping,
anti-aliasing) is our code either way. Game code never touches bgfx.

Audio rationale: miniaudio for devices and mixing; the event system, tag-based banks, occlusion, and reverb
zones are ours because that is the data-driven layer the brief asks for. Steam Audio (Apache-2.0) can be added
later for occlusion, reflections, and HRTF. FMOD is closed source and lives outside the repository.

Networking rationale: ENet gets the multiplayer prototype running early behind a five-call transport
interface. GameNetworkingSockets or Steam networking replace it for encryption and NAT traversal.

Physics rationale: Jolt's character collision solves collide-and-slide, stairs, and slopes; the movement
feel, acceleration curves, mantling, prone, and camera are ours on top of Jolt shape casts.

## 4. Repository structure

```
Project-Predation/
  CMakeLists.txt  CMakePresets.json  vcpkg.json  .gitattributes (LFS)  .clang-format  .editorconfig
  Engine/   Core Platform Render Shaders Physics Audio Animation Navigation Network Scene Assets Debug UI
  Game/     Player Weapons Interaction Creature Missions Session Debug Main.cpp
  Tools/    standalone tools
  Assets/   Config Data Maps Models Textures Audio
  Tests/    Catch2
  Docs/     README ARCHITECTURE BUILDING NETWORKING AI ANIMATION DEBUGGING DECISIONS
  Scripts/  build test run helpers
  cmake/    modules
```

## 5. Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for the layer diagram, frame loop, and module list.

## 6. AI, animation, networking

See [AI.md](AI.md), [ANIMATION.md](ANIMATION.md), [NETWORKING.md](NETWORKING.md).

## 7. Roadmap

| Milestone | Brief phase | Exit criterion |
|---|---|---|
| M1 Runnable skeleton | 0 and start of 1 | Fresh clone builds with one command. Window, grid, F3 overlay, console, tests, docs. |
| M2 Scene and physics playground | 1 | Lit meshes from glTF, free camera, Jolt bodies, debug draw, profiler. |
| M3 Player controller | 2 | Walk, run, crouch, prone, jump, mantle, stairs, slopes, fall damage on the test map. Feels good. |
| M4 Player body and procedural animation | 3 | Look down and see a body that moves convincingly. Tuning panel saves to JSON. |
| M5 Interaction | 4 | Pickup, drop, doors, a locker to hide in, a flashlight. |
| M6 Firearm | 5 | Aim, fire, recoil, reload, ammo, damage to targets. |
| M7 Multiplayer prototype | 6 | Two players host and join, see each other's bodies, interact, shoot. Prediction works under simulated lag. |
| M8 Creature prototype | 7 | Placeholder body navigates, sees, hears, pursues, investigates, attacks, retreats. Brain inspector and timeline live. |
| M9 AI architecture | 8 | Traits, utility, memory, internal state, stalking, hiding, curiosity. Reproducible from seed. |
| M10 Procedural anatomy | 9 | Seeded bodies with gameplay-relevant capabilities. Seed inspector. |
| M11 Creature procedural animation and traversal | 10 | Generated gait, climbing, vents, ceilings. |
| M12 Capture | 11 | Grab, drag, carry, lair, rescue. |
| M13 First real map | 12 | One polished facility with extraction. |
| M14 Vertical slice | all | 2 players, 1 creature, 1 facility, 1 firearm, inventory, proximity voice, stalking, hiding, damage, capture, extraction, debugger. |

Audio gets a first pass in M2 and M5 and an atmosphere pass alongside M13. Settings and menus grow
incrementally; the 3D menu comes after M14.

## 8. Milestone 1 scope

Prerequisites: Visual Studio 2022 Build Tools with the C++ workload, CMake, Ninja, vcpkg.

Deliverables: repository layout, CMake presets, vcpkg manifest, git attributes with LFS, formatting config;
categorized logging; layered JSON config on cvars; fixed-step time; asset path discovery; SDL3 window; input
action bindings from JSON; bgfx on D3D11 with clear, debug lines, grid, axes; ImGui; F3 overlay with FPS, frame
graph, CPU and GPU time, memory; category toggles; console with help, quit, cvar get/set, log levels, reload
config; file watcher hot reload of config; Catch2 tests for config and cvars; README, BUILDING, ARCHITECTURE,
DEBUGGING, DECISIONS.

Verification: clone fresh, one configure and one build command, launch, see a grid with a moving camera, press
F3, open the console and change a cvar, run the tests.

## 9. Technical risks

- Procedural creature animation quality: build gaits on a few hand-picked morphologies before generalizing.
- Procedural creature meshes: assemble from authored parametric parts before mesh synthesis.
- Player controller edge cases with prediction: test map covers every case; controller on the fixed tick from
  the start.
- Creature navigation for arbitrary anatomy: few size classes, explicit link types.
- Rendering scope: light budget, one flashlight shadow per player, profiler from M1.
- Renderer choice: game code never touches bgfx.
- Windows build friction: vcpkg build times; repository in a OneDrive folder.
- Scope: playable exit criteria per milestone and the not-yet list.
- Player voice mimicry privacy: opt-in, session memory only.

## 10. Deliberately not built yet

Procedural maps. Progression and unlocks. Weapon customization beyond a flashlight. Character customization.
The 3D main menu. Steam integration, matchmaking service, NAT traversal, host migration, late join, anti-cheat.
Steam Audio, HRTF. Global illumination, ray tracing, screen-space reflections. Smell, thermal, or vibration
senses. Third-person spectating. Downed and revive state. Creature mesh synthesis before M10. Cooked asset
pipeline. Localization. Full gamepad polish. Mod support. Multiple maps. A level editor beyond ImGui panels.

## 11. Recommendations raised (labeled, not silently applied)

1. Move the repository out of OneDrive. Mitigation in place: build output can be redirected through
   `CMakeUserPresets.json` (see BUILDING.md).
2. Build the controller network-ready from Phase 2: fixed tick, input structs, no rendering dependencies.
3. Authored locomotion clips for the human with procedural layers on top.
4. A handheld or helmet flashlight as a core tool from Phase 4, since light drives creature vision.
5. Part-based creature bodies before mesh synthesis.
6. Player voice mimicry opt-in, memory only, disclosed in settings.
7. Realism through lighting, fog, flashlight beams, and sound rather than asset fidelity.
8. First-person spectating only unless playtests demand otherwise.
