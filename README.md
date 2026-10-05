# Project Predation

A first-person, 1 to 4 player co-op horror exploration game built on a custom C++ engine.

The crew are an independent exploration and recovery crew on contract to **CIRRA**, flying a ship of their own: they
choose where to go, fly there, go down in their shuttle to places nobody has charted, and bring back what they find --
while the creatures out there, each generated from a seed, hunt them. The ship grows as they upgrade it, and the
campaign is saved by whoever hosts it.

The game is being reworked from a mission-by-mission structure into that open campaign: the design is
[Docs/REWORK_DESIGN.md](Docs/REWORK_DESIGN.md), and [Docs/ROADMAP.md](Docs/ROADMAP.md) says how far it has got. The
original plan is [Docs/DESIGN_PLAN.md](Docs/DESIGN_PLAN.md). The world, the organisation and the organisms are described in
[Docs/Project_Predation_Lore_Reference.md](Docs/Project_Predation_Lore_Reference.md), which is the source of truth for
anything the game says about them.

## Status

Playable, in development. A campaign is begun or carried on from the title and hosted (alone or with up to three
others): aboard the ship, the navigation map shows the system the ship is in; a course is flown to a planet or moon,
its scan finds places to land, and the shuttle takes the crew down to a generated site with a facility, a terminal to
download from and a creature. The campaign is saved on the host's machine.

## Quick start

Prerequisites and the full build walkthrough are in [Docs/BUILDING.md](Docs/BUILDING.md).

Double-click `Scripts\Windows\Play.cmd`. It builds whatever changed and then starts the game.

Or, from a command prompt:

```
Scripts\Windows\build.cmd            (configure + build, preset windows-debug)
Scripts\Windows\test.cmd             (run unit tests)
Scripts\Windows\run.cmd              (launch the game)
```

What each script does is in [Scripts/README.md](Scripts/README.md).

In the game: the mouse looks, WASD moves, F interacts, F3 shows the debug overlay, grave (`) opens the console in a
developer build. Type `help` in the console. All the keys are in Settings.

## Layout

| Directory | Contents |
|---|---|
| `Engine/` | Engine library: core, platform, render, physics, audio, animation, navigation, network, scene, assets, debug |
| `Game/` | Project Predation gameplay: campaign and universe, player, weapons, interaction, creature, missions, ship, session |
| `Tools/` | Standalone tools (asset cook, shader scripts) |
| `Assets/` | Config, data, maps, models, textures, audio |
| `Tests/` | Unit tests (Catch2) |
| `Docs/` | Design and engineering documentation |
| `Scripts/` | Build, test, and run helpers |
| `cmake/` | CMake modules |

## Documentation

- [Docs/REWORK_DESIGN.md](Docs/REWORK_DESIGN.md): the open exploration rework, the game's design now
- [Docs/ROADMAP.md](Docs/ROADMAP.md): what is built and what is left
- [Docs/DESIGN_PLAN.md](Docs/DESIGN_PLAN.md): the original plan and tech stack
- [Docs/RECORDING_AND_TEXTURES.md](Docs/RECORDING_AND_TEXTURES.md): the voice lines and textures to make
- [Docs/TUTORIAL_CHECKLIST.md](Docs/TUTORIAL_CHECKLIST.md): what a tutorial has to teach
- [Docs/BUILDING.md](Docs/BUILDING.md): toolchain setup and build instructions
- [Docs/CPP_PRIMER.md](Docs/CPP_PRIMER.md): C++ and build-system orientation for this codebase
- [Docs/ARCHITECTURE.md](Docs/ARCHITECTURE.md): engine and game architecture
- [Docs/DEBUGGING.md](Docs/DEBUGGING.md): overlay, console, cvars, screenshots, logs
- [Docs/DECISIONS.md](Docs/DECISIONS.md): architecture decision records
- [Docs/NETWORKING.md](Docs/NETWORKING.md), [Docs/AI.md](Docs/AI.md), [Docs/ANIMATION.md](Docs/ANIMATION.md): system designs

## License

Not yet decided. All rights reserved until a license is chosen.
