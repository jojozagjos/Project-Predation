# Architecture Decision Records

Short records of decisions that are expensive to reverse. Add a new entry rather than editing history; if a
decision is superseded, say so in both entries.

## ADR-001: Custom C++ engine with mature third-party libraries

**Status**: accepted, 2026-09-08

Build the engine Project Predation needs, not a general-purpose engine. Use libraries for window/input, GPU
API abstraction, physics, audio device layer, navigation mesh generation, networking transport, image and model
loading, UI, math, serialization, logging, profiling, and tests. Own the frame loop, entity model, renderer
proper, animation system, creature system, networking model, debug tools, and all game systems.

## ADR-002: bgfx as the rendering backend abstraction

**Status**: accepted, 2026-09-08

Raw Vulkan or D3D12 would cost weeks before a lit mesh and slow every later feature. D3D11 is simple but
Windows-only. bgfx handles the GPU API, shader cross-compilation, and GPU timers, with a decade of production
use. We run D3D11 on Windows first and Vulkan when Linux matters. The renderer proper (lighting, shadows,
post-processing) is ours. Game code never touches bgfx, so a later swap to Diligent Engine or a custom RHI is a
renderer rewrite, not a game rewrite.

Known costs: bgfx's GLSL-flavored shader dialect and `shaderc` build step; no bindless resources.

## ADR-003: Jolt Physics

**Status**: accepted, 2026-09-08

Modern, multithreaded, MIT-licensed, with shape casts, layers, character collision, ragdolls, and constraints.
The movement feel is ours on top of Jolt queries so the controller never feels like a stock capsule.

## ADR-004: ENet first, encrypted transport later

**Status**: accepted, 2026-09-08

ENet is tiny and lets the multiplayer prototype happen early. It sits behind a five-call transport interface
(connect, disconnect, send reliable, send unreliable, poll). GameNetworkingSockets or Steam networking replace
it when the internet service, encryption, and NAT traversal become relevant.

## ADR-005: miniaudio plus an in-house event layer

**Status**: accepted, 2026-09-08

miniaudio provides devices, decoding, and mixing. Sound events, tag-based banks, categories, occlusion, and
reverb zones are ours, because that is the data-driven layer the design calls for. Steam Audio can be added
later for real occlusion, reflections, and HRTF. FMOD was rejected for being closed source and outside the
repository.

## ADR-006: CMake presets, Ninja, vcpkg manifest, static-md triplet

**Status**: accepted, 2026-09-08

One command builds from a fresh clone. `x64-windows-static-md` links third-party code statically with the
dynamic CRT: one executable, no DLLs, no CRT mismatches. bgfx tools are built for the host triplet.

## ADR-007: In-house entity model

**Status**: accepted, 2026-09-08

Generational handles, typed component pools, explicit system order. Small enough to understand fully; the
inspector, serialization, and replication hook into a component registry. EnTT is the fallback if the
in-house version grows beyond a few hundred lines.

## ADR-008: CVars for engine settings, JSON data files for gameplay tuning

**Status**: accepted, 2026-09-08

Engine and application settings are typed cvars with layered sources (code default, defaults.json, user
settings, command line, console). Gameplay tuning (creature traits, weapon stats, movement curves, animation
parameters) lives in JSON data files loaded through the asset system and edited by in-game tools. Both avoid
burying balance values in C++.

## ADR-009: Fixed 60 Hz simulation tick with per-frame camera sampling

**Status**: accepted, 2026-09-08

Movement and gameplay run on a fixed tick so host and clients execute identical code and prediction is
possible. Camera rotation is sampled every frame outside the tick so aiming is never tied to the simulation
rate. Rendering interpolates between fixed states.

## ADR-010: Single-threaded bgfx during bootstrap

**Status**: accepted, 2026-09-08

`bgfx::renderFrame()` is called before `bgfx::init()` so bgfx does not create a render thread. Simpler to debug
while the renderer is young. The multithreaded encoder path can be enabled later without API changes.

## ADR-011: The player body is anchored to the camera, not to the feet

**Status**: accepted, 2026-09-08

The visible body was placed by adding a fixed offset to the player's feet, and the head landed near the eye
only when standing still and facing forward. It drifted off-centre whenever the hips turned away from the view,
sat too far forward, and pushed the skull through the camera when crouching or cresting a step.

The body now builds its pose, evaluates it once, then translates the root so the head bone lands exactly on
the eye, and evaluates again. Two passes over nineteen bones. The head is on the camera by construction in
every stance and every direction of travel.

The consequence is that the stance eye heights in `player.json` are the single source of truth for how low
each stance sits. The body's per-stance pose describes shape only; a hip height there would cancel out and
mislead whoever tuned it, so the field was removed.

## ADR-012: Walking lowers the hips, because otherwise a step is geometrically impossible

**Status**: accepted, 2026-09-08

With the hips at standing height this rig's leg is exactly long enough to reach the ground straight down and
no further: hip height above the ankle and the sum of the leg segments are both 0.53 of standing height, which
is anthropometrically correct and leaves zero room to place a foot anywhere but directly underneath. No
planted step was reachable at all, which is why the old gait slid the feet instead.

Walking therefore lowers the eye, by a constant amount while moving plus a deeper dip at each footfall. That
is what a real gait does, and combined with ADR-011 it is also the only thing that lets a leg reach out far
enough to plant a step. Camera and body share one stride phase in `PlayerState` so the head drops exactly as
a foot lands, and every foot target is clamped to what the leg can actually reach, so a step can never drag.

## ADR-013: Inventory icons are rendered from the item's own geometry

**Status**: accepted, 2026-09-08

Every item is rendered once into a cell of an offscreen atlas, using the same mesh and the same shader the
world uses, and the inventory draws a sub-rectangle of that texture. Adding an entry to `items.json` gives it
an icon immediately, with nobody drawing one, and an icon can never disagree with the object it stands for.

The alternative, hand-drawn 2D stand-ins, needs new art for every item and drifts from the real thing the
moment either changes.

## ADR-014: Firing is a pure simulation that never resolves its own hits

**Status**: accepted, 2026-09-08

`WeaponSim::Step` is a function of state, input and time. It takes no renderer, no physics and no scene, and
it produces `FireEvent`s rather than damage. Resolving an event against the world is a separate call.

This is the shape the networking model needs, built in from the start rather than retrofitted: a client runs
the simulation the instant the trigger goes down so the weapon feels immediate, and sends the events up; the
host runs identical code and is the only thing that turns an event into damage. Spread comes from a hash of
the shot number rather than a random generator, so both machines deviate the same round the same way without
either sending the direction, and there is no generator state to keep in step.
