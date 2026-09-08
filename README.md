# Project Predation

A first-person, 1 to 4 player survival horror extraction game built on a custom C++ engine.

Players are field teams of the **Anomalous Containment & Research Directorate (ACRD)**, deployed into
facilities that contain a single alien organism. Each organism is generated from a seed: its anatomy,
capabilities, senses, and temperament are reproducible, but its behavior drifts with what it experiences
during a mission.

The design brief and the approved plan live in [Docs/DESIGN_PLAN.md](Docs/DESIGN_PLAN.md).

## Status

**Milestone 1: runnable skeleton.** Window, renderer bootstrap, debug line drawing, fly camera,
F3 debug overlay, developer console, layered configuration, input bindings, hot reload of config,
unit tests, and documentation. No gameplay yet.

## Quick start

Prerequisites and the full build walkthrough are in [Docs/BUILDING.md](Docs/BUILDING.md).

```
Scripts\build.cmd            (configure + build, preset windows-debug)
Scripts\test.cmd             (run unit tests)
Scripts\run.cmd              (launch the game)
```

In the game: hold the right mouse button to look, WASD to move, F3 for the debug overlay,
grave (`) for the console. Type `help` in the console.

## Layout

| Directory | Contents |
|---|---|
| `Engine/` | Engine library: core, platform, render, physics, audio, animation, navigation, network, scene, assets, debug |
| `Game/` | Project Predation gameplay: player, weapons, interaction, creature, missions, session |
| `Tools/` | Standalone tools (asset cook, shader scripts) |
| `Assets/` | Config, data, maps, models, textures, audio |
| `Tests/` | Unit tests (Catch2) |
| `Docs/` | Design and engineering documentation |
| `Scripts/` | Build, test, and run helpers |
| `cmake/` | CMake modules |

## Documentation

- [Docs/DESIGN_PLAN.md](Docs/DESIGN_PLAN.md): the approved plan, tech stack, roadmap
- [Docs/BUILDING.md](Docs/BUILDING.md): toolchain setup and build instructions
- [Docs/CPP_PRIMER.md](Docs/CPP_PRIMER.md): C++ and build-system orientation for this codebase
- [Docs/ARCHITECTURE.md](Docs/ARCHITECTURE.md): engine and game architecture
- [Docs/DEBUGGING.md](Docs/DEBUGGING.md): overlay, console, cvars, screenshots, logs
- [Docs/DECISIONS.md](Docs/DECISIONS.md): architecture decision records
- [Docs/NETWORKING.md](Docs/NETWORKING.md), [Docs/AI.md](Docs/AI.md), [Docs/ANIMATION.md](Docs/ANIMATION.md): system designs

## License

Not yet decided. All rights reserved until a license is chosen.
