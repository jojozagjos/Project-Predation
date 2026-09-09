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

## ADR-015: The model editor runs inside the game, not beside it

**Status**: accepted, 2026-09-09

Models and animations are authored in a panel inside the running game rather than in a separate
program. It draws through the same renderer, the same shader and the same lighting the world uses,
so what is built is what appears in the player's hands: there is no export step, no second
definition of what a model is, and nothing that can look right in a tool and wrong in the game.

The cost is that the editor ships in the game executable. That is acceptable while it is small, and
the alternative, a second application duplicating the render path, is exactly the thing that makes
tool output stop matching the game.

Models are JSON: a list of parts, a list of named sockets, and animation clips. Sockets are the
contract between a model and whatever holds it, so moving a grip in the editor moves the hand that
holds it without a line of code changing. Imported geometry is stored inside the model file rather
than referenced by path, so a model is one self-contained thing even when it came from a download.

## ADR-016: UDP with reliability built on top, not TCP

**Status**: accepted, 2026-09-09

The transport is UDP, with an in-house reliable channel over it. TCP was rejected because it
bundles together the one property the game wants and one it cannot afford: guaranteed delivery, and
strict ordering of everything.

A lost snapshot must not delay the next one. TCP would hold every later snapshot behind the missing
one until a resend arrived, by which time the world has moved on twice and the resent snapshot is
worthless. The correct response to a lost snapshot is to ignore it, which TCP cannot express.

So both channels share one socket. The unreliable one is fire and forget. The reliable one keeps
each message until the far end acknowledges it, buffers anything that arrives early, and delivers
in order. Acknowledgements ride on traffic already going the other way, so they cost nothing.

Reliability is deliberately small: sequence numbers, a resend timer, and an in-order delivery queue.
No congestion control, no fragmentation. The largest message in the game is an 85-byte snapshot, and
building the parts of TCP the game does not need would be work spent recreating the problem.

## ADR-017: The host simulates every player; clients predict only themselves

**Status**: accepted, 2026-09-09

Clients send input, never results. The host runs the same movement code against its own physics
world for every player, and what comes out of that is what happened. A client that says it is
somewhere is making a claim, and claims are not written into the world.

A client still moves the instant a key goes down, because a round trip of dead controls would make
the game feel broken. It runs the movement itself, remembers each tick it guessed at, and compares
against the host's answer when it arrives. Agreement is the normal case and costs nothing. On
disagreement it takes the host state and replays every input the host had not yet seen, which is
why `PlayerController::Step` had to be a pure function of state, input and world from Milestone 3
onward: replaying it must give the same answer as running it the first time.

The remaining difference is carried as a drawing offset that decays over a few frames, so a
correction of a few centimetres is invisible rather than a twitch. Past two metres it snaps instead:
the client has been through a wall or has been grabbed, and smoothing that would look worse.

Remote players are interpolated between the two snapshots that bracket a moment 66 ms in the past,
never extrapolated. A guess about a body that has just stopped or turned is worse than being
slightly late.

Only position, orientation, stance, lean and stride phase cross the wire. The walk cycle, the arms,
the head and the weapon hold are reproduced locally by the same procedural body the local player
uses. A gait is expensive to send and cheap to reproduce.

## ADR-018: Rolling onto your back while prone is gone

**Status**: accepted, 2026-09-09, superseded by removal the same day

Supine prone was built and worked: the roll followed how far round you were looking, passed through
the side, and the limbs went with it. It was first switched off behind a flag, then removed
altogether at the request of whoever has to play it. It is a whole movement of its own and the game
does not need it yet.

The code is in the history rather than behind a flag. A flag that is never true is a second version
of every function it touches, and the pelvis roll reached into the arms, the legs, the weapon hold
and the torso twist limit: keeping it meant keeping five conditionals nobody could exercise.

What replaces it: a prone body turned further than a neck allows shuffles round on its front. Which
way it goes is decided once and held, because deciding it every tick makes the body jitter at
exactly half a turn, where which way is shorter flips between one frame and the next.

## ADR-019: Climbing is a fixed path, not a force

**Status**: accepted, 2026-09-09

Mantling moves the character along a computed path with the simulation switched off for its
duration, rather than applying an upward impulse and hoping. Two reasons.

It has to be repeatable. A client replays its own inputs whenever the host disagrees, so a climb
must produce the same result every time it is run. A scripted path does; a push against a collider
resolved by a solver does not, quite.

And it has to be committing. The path takes longer for a higher ledge, cannot be steered, and cannot
be cancelled. That is the cost of the shortcut, and without it climbing would be strictly better
than walking round and nobody would ever walk round.

Finding the ledge is four raycasts: down from in front to find the top, forward to confirm there is
something in the way rather than open ground, down again further on to confirm there is enough top
to stand on, and up to confirm there is headroom. All against static geometry, so both machines find
the same ledge.

## ADR-020: The host hands round a roster so it can be replaced

**Status**: accepted, 2026-09-09

When the host leaves, the game does not end. The lowest surviving player number takes over and the
rest connect to it.

The whole difficulty is that by the time anyone notices, the machine they would have asked is the
one that left. So the host publishes the roster whenever it changes: who is playing, what they are
called, and the address each was seen from. Everybody therefore already holds the same list when
they need it, and everybody reaches the same answer without having to agree on one.

The world survives the handover because every machine already has a complete copy of it. That is
what applying the same events to the same starting state buys: the successor does not have to be
sent anything, its own copy simply becomes the authoritative one.

Two limits worth stating. The addresses are the ones the old host saw, so on one network they are
the machines' own addresses and work; through a router they are the hole that router punched, which
stays open only as long as it stays open. And a player who never finished joining holds no roster,
so it is not allowed to elect itself: without that rule every client that failed to connect declared
itself the new host.
