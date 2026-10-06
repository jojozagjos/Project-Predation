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

## ADR-021: The crouch folds forward so the hips have somewhere to be

**Status**: accepted, 2026-09-09

A crouch is a leg problem before it is a pose problem. The camera is dropped to crouch eye height,
the body is anchored so the head lands on the camera, and whatever height is left over between the
hips and the eye decides how much of it the legs have to absorb. With the torso near-upright there
was almost nothing left over: the hips sat 0.56 m up, the leg folded to 57 per cent of its length,
and the thigh finished 70 degrees off vertical with the knee half a metre in front of the body.

No knee pole fixes that. With two bones of equal length, the hip and the foot fix where the knee
can be, up to a sign; the pole picks which of two points, not where they are. So the hips had to
come up, and the only height available to raise them with is the torso. A real crouch folds forward
for exactly this reason. The lean is now 46 degrees and the thigh and shin each sit at about 52.

Folding forward pushes the pelvis out behind the camera. That is also true of a real squat, and it
is wrong here, because a game camera is locked over the collision capsule rather than over the feet.
The whole crouched figure is therefore slid forward to compensate, which leaves the head a little in
front of the eye. Nothing sees that: the head is hidden from its owner and the camera is invisible to
everyone else. What does matter is that the figure stays inside its capsule, so nobody is shooting at
a body that is not where the hit test says it is, and that is asserted rather than assumed.

The stride goes with it. Stride length and step height were the standing ones whatever the stance,
so a crouched player at walking pace was asked for a 0.6 m step lifted 0.14 m clear. There is no leg
free to do that when the knees are already folded, and the thigh swept 70 degrees a stride reaching
for it. Both scale with the stance now. Because the walk phase is integrated from distance travelled
over stride length, shortening the stride raises the cadence to match instead of sliding the feet.

The crouch itself had to come up as well, and this is the part that took two tries to find. Every
pose test ran on the defaults compiled into PlayerConfig, and the game ran on Assets/Data/player.json,
which asked for a crouch eye height of 1.02 m. That is a full squat: the hips land at 0.38 m, the leg
folds to a third of its length, and the thigh passes the horizontal partway through every stride,
knee above hip. Shortening the stride enough to hide that turns the walk into a shuffle, which is
what the first attempt did. The eye is now at 1.30 m in both places, and a test loads the shipped
file and checks the pose it produces, because the whole cost of this was that nothing did.

Raising the capsule to match cost some of the range of things you can crouch under: the band is now
1.44 m to 1.80 m rather than 1.15 m to 1.80 m. That is a level design constraint and it is the one
to revisit if crouching needs to get under something lower, but it cannot be bought back by lowering
the eye alone, because it is the eye that decides where the hips are.

## ADR-022: What the hands are holding is traced against the world, not guessed

**Status**: accepted, 2026-09-09

Where a weapon or a carried item sits is worked out as an offset from the eye. That is the right
frame for it, because it has to stay on screen and the screen is at the eye. It knows nothing about
the world, and it showed: walking into a wall put most of a metre of barrel through it, and lying
down put the hold under the floor, because the prone carry hangs below an eye that is itself a few
centimetres up.

The first attempt at the wall was a soft pull-back driven by a single trace along the view. It half
worked, and failed in the case people actually hit: walk into a wall, then turn. The trace runs off
along the face and finds nothing much while the barrel is still crossing it, measured at 0.17 m
through in the test that now covers it.

Both are the same question asked of the world instead of a rule. One trace from the eye to the point
being held, one straight down beneath it. The first stops the muzzle at whatever is really in front
of it whichever way the player is looking; the second puts the hold on top of whatever is underneath,
traced rather than compared against the player's own feet, so lying on a crate keeps the weapon on
the crate. The soft pull-back stays, because bringing a long weapon in against a wall is what anyone
does with one in a corridor; it is now a matter of how it looks rather than the thing preventing a
solid object passing through another.

## ADR-023: Firing is an event, so it travels as one

**Status**: accepted, 2026-09-09

Everything else about another player is a state: where they are, which way they are looking, what
stance they are in, what is in their hands. Snapshots carry states well, because a snapshot that
goes missing is superseded by the next one.

Firing is not a state. It happens on one frame, and that frame is almost never a frame a snapshot
goes out on, so a snapshot-carried "is firing" flag misses the moment more often than it catches it.
That is why other players' weapons fired silently: no flash, no recoil, no animation, and rounds
that appeared to come from nowhere.

The recoil is driven from the shot message instead, which is sent exactly when a round leaves the
barrel and is already reliable because the round itself has to arrive. The host has to apply it to
the shooter's avatar itself, because nobody sends that message back to the machine that made it.

Rounds are drawn as something that travels: a short lit section flying from the muzzle at 260 m/s
and a mark where it lands. Lighting the whole line at once draws a diagram of a shot rather than a
shot, and left up long enough to be seen it reads as a laser. The full traced line moves behind a
debug toggle, alongside the line the round was really traced along, because rounds are traced from
the eye so the crosshair tells the truth and drawn from the muzzle so they look right, and checking
that difference has not become a lie needs both drawn at once.

## ADR-024: Shrinking a character capsule is not a question

**Status**: accepted, 2026-09-09

Changing stance resizes the collision capsule, and the resize can be refused: that refusal is what
stops a player standing up inside a vent. It was implemented as one call with a zero penetration
allowance, used for growing and shrinking alike.

Shrinking cannot create an overlap, so the test had nothing to find, but it found something anyway.
A character standing on the ground is touching the ground, and whether that contact reports a
penetration of exactly zero or of a millionth of a metre depends on the capsule's dimensions.
Crouching therefore worked at a capsule height of 1.42 m, was silently refused at 1.44 and 1.50, and
worked again at 1.55, with nothing to tell those apart but rounding. It had never shown up because
the shipped height happened to fall on a value that worked.

The check now runs only when the new shape is larger in some dimension than the current one. The
regression test sweeps twenty-one crouch heights rather than checking one, because the failure was
not a threshold and any single height would have missed it.

## ADR-025: The host names a dropped item, not the machine drawing it

**Status**: accepted, 2026-09-09

A pickup's index is the name every machine uses for it afterwards. It is taken by index and removed
by index everywhere at once, so the number has to mean the same thing on all of them.

Both sides chose their own, reusing the first free record so numbers stayed small enough to fit in a
byte on the wire. That is a sensible rule and it is not a shared one: two machines with different
holes in their lists reach different answers, and from the first disagreement onwards taking one item
removed a different one somewhere else. What that looks like from inside the game is an item that
cannot be picked up on one screen and a second copy of it on another.

The host now names the index in the spawn event and everyone else uses it, filling any gap with dead
records so the numbering stays aligned. The choice is a separate function so it can be tested
without a renderer, which everything else about spawning a pickup needs.

Loose items are also swept rather than stepped, because a keycard is fifteen millimetres thick and a
dropped one covers several centimetres in a tick; and clients ease towards the host's answer rather
than being teleported to it, because the host speaks thirty times a second and the screen draws at
least twice that. The client's own solver is held still while that happens, so the two are not
pulling against each other, which is separately how a thin item could be pushed through a floor.

## ADR-026: glTF is read here rather than by a library, and every primitive becomes a part

**Status**: accepted, 2026-09-09

Models arrive as .glb. The part of glTF a static prop needs is small and completely specified: a
JSON chunk, a binary chunk, and accessors saying how to read one out of the other. The JSON parser
is already a dependency, so the importer is about four hundred lines and no new third-party code.
What is deliberately not read is skinning, morph targets, cameras, lights and textures, none of
which this renderer has anything to do with. A textured download arrives as flat-shaded parts in
the colours of its materials, which is the look the game has anyway.

Every primitive becomes one part, baked into place by its node's transform, rather than the file
being merged into a single mesh. That matters more than fidelity for a weapon: a magazine has to be
its own part or no reload can move it. The carbine that arrived came in as four parts; the pistol as
one, which means it will need splitting before its slide can cycle.

Three things happen at import that are not in the file. The model is scaled to a stated size,
because downloads arrive in wildly different units and one a hundred times too large is
indistinguishable from one that failed to load. It can be turned, because glTF has no idea which way
a weapon points and the game wants the barrel down +Z; both models that arrived ran along +X. And
sockets are seeded from the geometry, because a model with no sockets is held by its origin, which
for anything downloaded is the middle of the receiver: the hand ends up inside the weapon and the
barrel points out of the wrist. Those seeds are guesses meant to be moved, but a guess taken from
the shape is close enough to see what is wrong with it.

## ADR-027: The grip is placed by looking at where the hand lands

**Status**: accepted, 2026-09-09

The trigger hand was placed at the weapon's origin rather than at its grip socket. That worked while
every weapon was built procedurally with its origin at the grip, and stopped working the moment a
model came from outside. Both hands now come from sockets.

Which raises the question of how a socket gets placed correctly, and the answer is not by looking at
the model: the only question that matters is where the hand ends up, and the only way to answer it
is to hold the thing. So there is a weapon bench. It points a weapon at a model without a restart or
an edit to weapons.json, drives everything the weapon does, draws the sockets on the weapon as it is
held with a line to the hand meant to be at each, and puts the distance between them on screen in
centimetres. Placing a grip is then a matter of moving a socket in the editor and watching a number
come down.

The model cache had to learn to forget, or the editor and the hands holding the model disagree until
the game is restarted, which is exactly the loop the bench exists to close.

## ADR-028: A weapon is carried by a named point on it, not by its origin

**Status**: accepted, 2026-09-10

The carry tuning placed the model's origin at an offset from the eye. That is meaningful only while
every weapon is built here with its origin on the grip. An imported model's origin is wherever the
person who made it left it, which for both guns that arrived is the middle of the receiver: the
whole carbine sat a hand-span too far forward, the support arm was left pointing at a handguard 21
centimetres beyond its reach, and the stock ended up in the camera.

What the tuning places is now a named point on the weapon. Carried, that is the trigger grip, so
the numbers read as where the firing hand is. Sighted, it is the sight socket, so aiming lines the
sight up on the view axis in all three axes rather than in height alone; the old version put the
sight at the right height and left it wherever it happened to be along and across, which is why the
sights drifted off centre as soon as the player aimed anywhere but level. Everything that clamps the
hold - the floor that keeps it out of the camera, the clamp that keeps it inside the arm's reach -
measures that point too, for the same reason.

Two smaller things follow from it. The back of the weapon is measured as well, because a floor on
the grip is not a floor on the stock, and the difference is a whole buttstock; it is enforced only
while aiming, since a carried weapon hangs low and to the right where the near plane never reaches
it. And a socket is where the palm closes, while the arm chain ends at the wrist, so the wrist is
now placed a hand's length short of the socket. Driving the wrist onto the socket put the joint
inside the weapon and charged the arm for a hand it does not have.

The carry itself is further out and less far down than a real hand. At a 90 degree horizontal field
of view the frame stops about 29 degrees below the axis, and a hand where a hand actually goes is
below that: the weapon drops off the bottom of the screen entirely. Holding the drop to about half
the reach puts it just inside the bottom edge. Short weapons are pushed out, up and in towards the
middle from there, blended by the weapon's own length, because a pistol held at a carbine's grip
points at the floor beside your hip.

## ADR-029: A socket carries a turn, and that is how a model is righted

**Status**: accepted, 2026-09-10

A model arrives however its author left it, and the two conventions in the wild differ by a quarter
turn. The importer can turn it on the way in, but by the time anyone can see that it is wrong the
model has been saved, its sockets have been placed and its clips have been authored. Turning it
after that means turning the geometry, which takes the sockets, the clips and everything else with
it: the first attempt at that left the shipped carbine facing backwards with its muzzle where its
stock had been.

`ModelSocket` already carried a rotation and nothing read it. The `grip` socket's rotation is now how
the weapon is turned in the hand, and the `sight` socket's is how it is turned once the sights are
up, falling back to the grip's. The model spins about the hold, so the hand does not move. Nothing
in the file changes except three numbers on one socket, which is what makes it undoable.

The same reasoning gave the editor a first-person panel. Everything else in it looks at a model from
outside, and outside is not where the player is: a hold that reads perfectly in the viewport can put
the receiver across half the screen from behind the eye, and finding that out meant leaving the
editor, starting a game and picking the thing up. The body is already in the editor and already
holding the model, so the panel is that body's own eye at the game's field of view, drawn into an
offscreen target every frame.

## ADR-030: A weapon's movements belong to its model, except the recoil

**Status**: accepted, 2026-09-10

There were two versions of a reload and two of an equip: one written in C++ against the whole weapon
as a single lump, and one authored on the model as a clip. The built-in one stepped aside when a
clip of the same name existed, which made what an author saw depend on what they had named things,
and it could never be right anyway: it does not know what parts a weapon has, and a reload is a
magazine leaving a well and a bolt going home. Those are the model's parts. The built-in versions
are gone, and `reload`, `equip` and `unequip` are clips or they are nothing.

Fire is the exception, and it is a real distinction rather than an inconsistency. The recoil is the
whole weapon moving in a hold; every weapon does it, it is the same movement for all of them, and it
has to agree with the hold that the carry code computes. The bolt cycling is this weapon's alone and
nothing in the game could guess at it. So a `fire` clip layers on top of the recoil instead of
replacing it.

The support hand is the other thing that cannot be a clip. A clip moves parts of a weapon, and a
hand is not one of them, so the hand that fetches a magazine off the belt during a reload stays
where it is: driven from how far through the reload the simulation says the player is.

## ADR-031: The developer tools are a build option

**Status**: accepted, 2026-09-10

`PRED_DEV_TOOLS` is on for anything built here and off for anything packaged. A packaged build has
no model editor in its menu and no editor commands in its console, and the packaging script leaves
out the raw model downloads the importer works on, which are tens of megabytes and no use without
it. The editor's code is still linked; what is gone is every way to reach it, which is what "not
shipped" has to mean while the editor and the game share a scene, a body and a renderer.

## ADR-032: A clamp is solved, not stepped

**Status**: accepted, 2026-09-10

Two of the clamps that keep a weapon inside the arm's reach walked towards their answer in two
centimetre steps. A loop like that does not converge on a value, it converges on a two-centimetre
lattice, and near the limit it takes a step on one frame and not on the next. What that looks like
from inside is the hands shaking, and it looks worst while aiming and turning, which is exactly when
the hold sits closest to the limit.

Both are now solved directly: the distance to move back along a line until a point enters a sphere
is the smaller root of a quadratic. `Tests/BodyPoseTests.cpp` measures the change in the hold's
movement from tick to tick while aiming through a sweep; it was 20 mm, which is the step, and is
now 0.1 mm.

The general rule this stands for: an iterative approximation inside a per-frame update has to
converge to something finer than the eye can see, or it is a source of jitter rather than a way of
avoiding maths.

## ADR-033: Editing a socket is not editing the model

**Status**: accepted, 2026-09-10

The editor had one "something changed" flag, and the preview rebuild it drove tore down and rebuilt
every mesh and every entity of the weapon in the hands. Moving a grip about therefore copied a
quarter of a megabyte per part per frame of the drag, which is why the frame rate fell into single
figures after a while of placing sockets.

A socket changes nothing that is drawn. It changes where a hand goes. There are now two flags, and a
socket or a clip edit only rereads the named points; the meshes are rebuilt when the geometry
changes, which is on an import, a part edit, an undo or a model-wide transform.

The same distinction is why the shipped weapon models are no longer asserted on in the test suite.
They are worked on, they spend afternoons half-turned, and a suite that goes red because somebody is
editing an asset is a suite everyone learns to ignore. The rules are checked against models the
tests write themselves; the shipped files are reported on with WARN, and the editor says the same
thing on screen while you are looking at it.

## ADR-034: The editor anchors the hold at the other end

**Status**: accepted, 2026-09-10

In the game the hold is fixed and the weapon hangs off its grip socket: the weapon's position is the
carry point minus the grip, so the trigger hand is at the carry point by construction. That is
right, and it is what ADR-028 is about, and it makes the editor unusable for the one job it exists
for. Dragging the grip socket moves the gun and never moves the hand, so placing a grip by watching
where the hand lands is impossible: the hand does not go anywhere.

The bench pins the grip the weapon is *carried* by while leaving the grip the hand *reaches* for
live. The gun then stays where it is and the hand walks along it, which is the question being asked.
It is the same relationship anchored at the other end, and the toggle says which end.

Where the hands are is separate from where on the weapon they close, and only the second half
belongs to the model. The first half is the same for every weapon a person carries, so it is the
player's tuning: nine numbers under `hold` in `player.json`, edited in the bench and written back
from there, because something placed by eye has to be saveable from where it was placed.

## ADR-035: The editor's first-person panel has to be able to lie only while you are dragging

**Status**: accepted, 2026-09-10

Pinning the carried grip (ADR-034) is what makes placing a grip legible: the gun holds still and the
hand walks along it. It is also, by construction, not where the game will put the weapon, and the
first-person panel beside it was therefore showing a hold nobody would ever see. Someone lined a
weapon up in the editor, started a game, and found it somewhere else entirely.

The pin is now re-taken the moment a drag ends. While a handle or a field is being held the gun
stays still and the hand moves; the instant it is let go the weapon settles into the game's own
placement and the panel is telling the truth again. The panel also renders at the game window's
shape rather than at a fixed 16:9, because how much of a weapon is on screen depends entirely on how
wide the screen is.

The general rule: a preview may differ from the thing it previews only while an edit is in progress,
and never at rest.

## ADR-036: Two carry distances that measure different points are not comparable

**Status**: accepted, 2026-09-10

`weaponReadyForward` places the grip and `weaponAimForward` places the sight, and a sight is forward
of a grip. Since ADR-028 made the carry place a named point rather than the model's origin, those
two numbers stopped being comparable, and 0.42 to the sight is nearer the eye than 0.38 to the grip
by however far the sight sits ahead of it. Raising the sights therefore pulled the whole weapon back
about five centimetres, on open ground, with no wall anywhere. It was reported three times as the
gun being pushed back while aiming, and twice I looked for it in the wall handling, because that is
where a weapon coming back towards the eye normally comes from.

The sighted distance is now floored at the ready distance plus the gap between the two sockets, so a
weapon may go further out when the sights come up and may not come back. `Tests/BodyPoseTests.cpp`
measures the origin, the back and the muzzle at both aims on open ground.

The lesson is about the units of a tuning value rather than about weapons: a number that means "how
far out X is" cannot be compared with one that means "how far out Y is", and giving both the same
suffix is what makes the mistake invisible.

## ADR-037: A wall does not touch an aimed weapon

**Status**: accepted, 2026-09-10

Three separate mechanisms moved a weapon when the player got near something: the carry offset was
shortened, the muzzle was traced out of whatever it had entered, and the sights were broken down
towards the carry. All three were sound reasoning about a rifle in a corridor and all three read, in
the sights, as the gun being shoved about the moment you brush a doorframe. Sighted, the weapon lies
along the view axis, so every one of them runs it at the eye.

None of them applies while aiming now. Aiming into a wall puts the barrel in the wall, which is what
a barrel in a wall looks like. Nothing about where a round goes changes, because shooting has always
traced from the eye.

The muzzle correction also keeps only the part of itself that runs along the carry axis. It used to
be applied as it came out of the trace, which is a vector towards whatever surface happened to be
nearest, so brushing a wall on the left slid the weapon right and down as well as back: a gun being
knocked out of the hold rather than drawn in.

## ADR-038: Where a weapon sits and where the hand sits are two sockets

**Status**: accepted, 2026-09-10

The carry placed the grip socket, so the trigger hand was at the carry point by construction. That
made the two impossible to set independently: moving the grip to put the hand somewhere moved the
whole gun instead, and a hold that was right could never be kept while the gun was nudged on the
screen. It also produced the editor's ugliest feature, a pin that held the weapon still during a
drag so the hand would appear to move, which then had to be un-pinned on release so the preview
would stop lying about where the game puts things.

There is a `carry` socket now. It says where the weapon sits, so moving it moves the gun; `grip`
says where the trigger hand closes, so moving that moves the hand along the gun. A model without one
falls back to its grip and behaves exactly as before, and the editor offers to add one where the
grip is, which changes nothing until it is moved.

The pin is gone with it. So is the six-button nudge that existed only to work around the coupling.

The general shape of the mistake: one value serving as the answer to two questions is not a
simplification, it is a constraint that nobody wrote down, and it shows up as a tool that cannot
express what someone is plainly trying to say.

## ADR-039: A hand on a weapon rolls with the weapon

**Status**: accepted, 2026-09-10

Every bone in the rig takes its roll from the plane its own joint bends in, which is the right answer
for a limb and cannot be resolved any other way: a straight limb has no bend to take one from. It is
the wrong answer for a hand that is gripping something. The arm's plane turns as the player turns, so
the hand rolled about its own forearm while the gun in it did not, and the fingers wound round the
grip. Measured through a full turn, the trigger hand's orientation relative to the weapon moved 49
degrees.

A hand closed on a weapon is part of the weapon, so its roll comes from the weapon's own frame and
its fingers point at the socket rather than on down the forearm. The same 49 degrees is now 9, and
what is left is the arm honestly reaching differently as the body turns.

The hand that fetches a magazine during a reload is rolled the same way, because a magazine well is
part of the weapon too.

## ADR-040: Smooth a hand in the frame the thing it is reaching for lives in

**Status**: accepted, 2026-09-10

The hand that fetches a magazine during a reload eased towards its target in world space. Both places
that target can be are attached to the player: the magazine well is on the weapon and the weapon
follows the view, and the belt is on the body. Easing in the world therefore charged the smoothing
for every degree the camera turned, so the hand trailed the well it was reaching into and never lined
up while the player was moving. At a normal mouse turn of 360 degrees a second it lagged by 12
centimetres, and worse the faster you turn.

It is smoothed in the carry frame now, where a turn moves the target hardly at all and what is left
to ease is the hand's own journey from the gun to the belt and back, which is the thing that wants
easing. The same measurement reads 3 centimetres at any turn rate.

This is the second time the same mistake has been found in a different place: the first was a hand
placed rigidly and snapping, this one a hand smoothed loosely and lagging. The rule that covers both
is that the frame a movement is smoothed in has to be the frame the target is still in.

## ADR-041: A client predicts where it will be, never whether it is alive

**Status**: accepted, 2026-09-10

A client's own controller is a prediction of what the host will do with its input. It was also
applying its own fall damage, and both ends were counting the same respawn clock. That agrees only
while both ends agree the player died, and a mantle interrupted by a fall is exactly where they stop
agreeing: the client killed itself on a landing the host had run a little differently, told nobody,
and then revived itself too. The result was a player alive and playing on their own screen and lying
on the floor for good on every other.

Health, death and coming back are the host's, like everything else about the world. A client's
controller no longer applies damage at all, and its respawn clock does not run; both arrive as world
events. Nothing about movement prediction changes, because movement is the part a client is supposed
to guess at.

The ammunition in a picked-up weapon was the same shape of mistake in miniature: the host sent the
rounds with every spawn and the client threw them away when it took one, so a rifle dropped with
three rounds came back full.

## ADR-042: A correction must not watch its own output

**Status**: accepted, 2026-09-13

Two corrections keep a held weapon out of a wall: the muzzle comes down, and the whole weapon comes
back. Both were measured on the weapon as it stood, which is the weapon after both had already run.
So each was reacting to its own work. Turned out of the wall, the barrel was no longer in the wall,
so the turn unwound, so the barrel went back in; and separately a dropped muzzle is near and low, so
it needed drawing in hardly at all, so the weapon sat further out, so it needed dropping further. A
centimetre of ground could tip the pair between those states, and what a player saw was the weapon
flicking as they walked up to a wall.

Both now measure the weapon with every correction taken back off it, which asks a question about the
room the player is standing in. Their own movement is then the only thing that changes the answer.

The same shape of mistake has appeared three times now in this codebase and is worth naming: a
per-frame correction that reads the state it has already corrected is a feedback loop, and a
feedback loop with a threshold in it oscillates. Measure the uncorrected state.

## ADR-043: There is no honest way to aim at a wall a hand's length away

**Status**: accepted, 2026-09-13

A carbine is six hundred millimetres long, a player can stand three hundred and twenty from a wall
because that is how wide they are, and a sighted weapon has to be far enough out that the camera is
not inside the receiver. The three cannot be had at once, and for a long time what gave was the
barrel: forty centimetres of it inside the wall. Dropping the muzzle is what gets a barrel out of a
wall, and it is the one correction the sights cannot survive, because the sights are the pose.

So the sights do not come up that close, which is also what happens to a person who tries it. This
reverses ADR-037, which was right about the thing it was reacting to: breaking the aim on how boxed
in the player felt shoved the weapon about every time they brushed a doorframe. What is refused now
is measured against where the muzzle would be with the sights up, so it refuses only where the
sights would be a lie and works exactly as before everywhere else. The simulation refuses too, so
nobody walks at aiming pace and shoots at aiming accuracy while looking at a weapon held at the hip.

## ADR-044: An arriving body places its limbs; a walking one eases them

**Status**: accepted, 2026-09-13

Feet and hands are smoothed towards where the pose wants them, which hides the step when a foot
trace crosses from one surface to another. That is right for somebody walking about and wrong for
somebody who has just arrived somewhere: a respawned body spent half a second hauling its limbs back
from where its corpse fell, through itself, at whatever angles the solver found on the way.

A body that is put somewhere rather than having walked there places its limbs outright for one frame
and eases from then on. Everything the old body was part way through is cleared with it.

The same rule applies to what a foot remembers. A planted foot keeps the height of the surface it
landed on so its target does not snap up and down the edge of a ledge; off the ledge that stops
being an answer to anything, and holding it anyway left the legs reaching up over the head for a
surface the body had long since fallen past.

## ADR-045: Limits on what a stranger can make this machine allocate

**Status**: accepted, 2026-09-13

Hosting now opens a port that people outside the house can reach, which makes every buffer sized
from a packet an attacker's to size. Two were unbounded. A connect request is one datagram and it
bought a peer with two vectors in it, so a stream of datagrams with forged source addresses bought
as many as the sender cared to send. And a peer could open a gap in the reliable stream and post
packets behind it for ever, because nothing makes a sender fill a gap: sixty thousand sequence
numbers of held payload is eighty megabytes for one connection.

Sixteen peers and sixty-four held packets. Neither costs anything in a real game, which is four
players on a connection that works, and both turn "use this machine up" into "this datagram was
dropped".

The port mapper is held to the same rule from the other direction: a search reply is a datagram from
whatever felt like answering, and a device description is a document fetched over the network, so
neither may name a host the game then goes and talks to. A reply is believed only from the address
it names, that address has to be on this network, and a control URL has to stay on the device whose
description it came from. The mapping itself has an hour's lease, renewed while the game is up, so
a crash cannot leave a hole in the router open for ever.

## ADR-046: The mixer is a function, and the sound card is the outer layer

**Status**: accepted, 2026-09-13

Sound is the one subsystem where a fault is hard to see and easy to hear, and "play it and listen"
is not a test anybody can run twice the same way. So the mixer takes a list of voices and a
listener and returns samples, and everything that decides how loud a thing is and which ear it is
in is arithmetic that a test can assert on: attenuation with distance, panning with the listener's
own facing, a voice ending when it runs out, the ceiling that stops eight gunshots at once tearing,
and the gain smoothing that stops a moving source clicking.

The device wraps that rather than containing it. With no sound card the engine still takes sounds,
holds voices and answers questions; it simply never mixes. A headless run therefore makes no noise
and takes no special path to do it, and the game does not refuse to start on a machine with the
audio switched off.

One thing this arrangement will not catch, and it is worth being plain about: whether a gunshot
sounds like a gunshot. Nothing here can. The recipes are a starting point to be tuned by ear.

## ADR-047: Sounds are recipes, not recordings

**Status**: accepted, 2026-09-13

Every sound is generated at startup from a line of JSON: how long, how fast it decays, how much of
it is hiss and how much a note, what note, how far that note slides, how much top is taken off, and
how hard the click on the front is. Twelve sounds are a few hundred kilobytes of samples built in a
few milliseconds.

The same reason the models are boxes. There is no sound designer on this project, and a placeholder
that can be tuned in a text file while the game runs is worth more right now than a library of wav
files that can only be replaced. Every field is something a person can hear the effect of, so
tuning is a thing the person making the game can actually do.

It is deterministic, which matters more than it looks: the same recipe gives the same samples on
every machine, so two people in a game never disagree about what a gunshot sounds like, and a test
can assert on a waveform.

## ADR-048: Two players dial each other, and swap the number by hand

**Status**: superseded by ADR-058, 2026-09-14. Originally accepted, 2026-09-13

Hosting across the internet needs somebody's router to forward a port, and on a network somebody
else runs there is nobody to ask: no router answers a forwarding request, and the address the menu
can hand out only works inside the building. The way every peer-to-peer game solves this is ICE.
Both machines ask a public server what their connection looks like from outside, they tell each
other, and then both start sending at the same moment; each router sees a packet going out first and
opens a hole for the reply, so a connection neither side was allowed to accept is made by both ends
dialling at once. libjuice does it in about four hundred kilobytes and has no dependencies.

The part ICE cannot do for itself is the telling. The two machines have to exchange a block of text
before there is any connection to exchange it over, and a game with a server of its own would pass
that through the server. This one has no server and is not going to grow one for this, so the
players pass it themselves over whatever they are already talking on. It is one line of base64, it
is two pastes, and it costs nothing to run. A code that does not survive a chat window is the most
likely thing to be wrong, so the encoding is tested against wrapping, whitespace and a sentence in
front of it.

Underneath, none of the game changed. The sequence numbers, the acknowledgements and the ordering
have nothing to do with how the bytes travel, so rather than writing them again the transport takes
a carrier: something with a Send, a Receive and one far end. A punched connection is exactly that,
and a pair of them wired together in a test is a pipe, which is how it is checked without a network.

It is not certain to work. A network that hands out a different hole for every destination defeats
hole punching, and the only answer to one of those is a relay somebody pays to run. What this does
is turn "impossible without a router nobody can configure" into "works on most connections".

## ADR-049: The shipping build has its own build folder

**Status**: accepted, 2026-09-13

ADR-031 made the developer tools a build option. The packaging script then turned that option off
by reconfiguring the *same* build folder the everyday build uses. `PRED_DEV_TOOLS` is a cached
CMake variable, so it stayed off afterwards: package once and your own release build quietly lost
its model editor until you cleared the cache. Nothing said so. The title screen simply had one
fewer button.

There is now a `windows-shipping` preset with its own binary directory, and
the developer presets pin `PRED_DEV_TOOLS=ON` in their own cache variables so a preset configure
always restores it. Packaging builds the shipping preset and then checks the cache actually says
`OFF` before it lays anything out, because a stale cache is silent and an editor that leaked into
somebody else's copy is not visible from the outside.

The build also says which one it is: once in the log at startup, and on the title screen next to
the byline in the developer build. Two builds that look identical and behave differently are worth
one line of text each.

## ADR-050: One vcpkg tree for every preset

**Status**: accepted, 2026-09-13

vcpkg's manifest mode installs the dependencies into the build folder, which means a tree per
preset. On this project that is 3.1 GB, and with debug, release and shipping folders it was 9.3 GB
of the same bytes three times over: two thirds of everything the repository occupied.

`VCPKG_INSTALLED_DIR` in the base presets points them all at `build/vcpkg_installed`. Nothing is
lost by sharing, because a triplet's installed tree already holds the debug and the release
libraries side by side, and the presets here differ only in build type and in options of our own.

The one thing to watch is `find_program`: it caches an absolute path and does not check afterwards
that the path still exists, so the shader compiler had to be un-cached once by hand when the tree
moved. `clean.cmd` and `clean.sh` now remove any per-preset tree they find, so a folder configured
before this change stops costing anything the first time either is run.

## ADR-051: The Linux port is withdrawn until the game is finished

**Status**: accepted, 2026-09-14, withdrawing the groundwork from 2026-09-13

The Linux preparation is removed: the `.sh` scripts, the `linux-*` presets, the container that
would have compiled them, and `Docs/LINUX.md`. It was never a working port — nothing had ever been
compiled by a Linux compiler — so what was actually there was a second platform's worth of files
and presets to keep correct, in exchange for nothing that runs. Finishing the game on one platform
comes first. It is all in the history, and reaching a second platform is easier from a finished
game than from a half-built one.

What stays is the handful of POSIX branches inside the engine: the `#else` arms in `SystemInfo`,
`Window::NativeDisplay()`, the display handle the renderer passes to bgfx, and the Threads link in
`Engine/CMakeLists.txt`. They compile to nothing on Windows, they are the parts that were reasoned
out carefully rather than typed quickly, and deleting inert correct code to look tidier is how you
end up writing it twice.

## ADR-052: Footsteps are recordings; everything else is still a recipe

**Status**: accepted, 2026-09-14

ADR-047 said sounds are recipes rather than recordings, and the reason held: there is no sound
designer here, and a placeholder that can be tuned from a text file while the game runs is worth
more than a library of files that can only be replaced. Footsteps are the exception, because a
footstep is a physical event with a texture, and texture is the one thing a handful of tunable
numbers cannot fake. A synthesised step is recognisably a burst of filtered noise no matter how
carefully the numbers are set.

So `Assets/Data/footsteps.json` names clips per surface and the engine reads wav. Everything else
is still synthesised, and the synthesised footstep is still there and still used when the clips are
missing: a sound pack that cannot be shipped must not be able to take the game's audio down with it.

Three things fall out of reading real files rather than generating them.

Trailing silence is trimmed on load. The pack's clips are around half a second each and hold about
a sixth of a second of sound; the rest is padding, and padding holds a voice open for its whole
length. At a sprint the next step would begin while two previous files were still being silent.

Clips are picked at random with an immediate repeat refused, rather than alternating. Two clips
alternating strictly is a pattern the ear finds within about six steps.

Surfaces are levelled by a gain in the file, and the numbers are measured rather than guessed: each
clip's RMS was taken after trimming and the gains bring each surface to roughly concrete's level.
Gravel is recorded nearly three times louder than metal in this pack; without levelling, walking
from one to the other is a volume change rather than a material change.

Concrete is the default. Not by taste, because nobody here can claim to have listened carefully:
its two variants are the shortest in the pack at 255 and 260 ms, they are within 5% of each other in
level so alternating them does not read as a limp, and at a spectral centroid near 300 Hz they sit
below the range you need clear to hear something moving in the dark. Metal's two variants are 414
and 604 ms and 50% apart in level, which is the opposite of all three.

## ADR-053: A host holds one punched connection per guest

**Status**: superseded by ADR-058, 2026-09-14. Originally accepted, 2026-09-14, extending ADR-048

ADR-048 had two players dial each other and swap the number by hand. What it did not say, because
it did not occur to anyone at the time, is that a hole punched through two routers joins exactly two
machines. One carrier meant one far end, so a game played over the internet was two players and no
more, whatever `kMaxPlayers` said. On one network four players already worked, because a UDP socket
hears from everybody; that difference is what made it easy to miss.

The host now holds one link per guest. `DatagramCarrier` gained a link index, and each link gets a
stand-in address of its own — 0.0.0.1, 0.0.0.2 and so on — so every punched connection becomes an
ordinary peer and goes through the same peer table, the same reliability, the same acknowledgements
and the same timeouts as a real address. Nothing above the transport knows the difference. The
alternative was a second set of machinery that did the same work for punched connections, which is
how two implementations of acknowledgement end up disagreeing about what arrived.

Receiving is round-robin across the links rather than draining each in turn, so one player sending
hard, or one link with a backlog after a stall, cannot hold the others out of the frame.

The signalling follows from it: one code swap per guest, not one per game. The first is negotiated
from the title screen before there is a game to be in, and the rest from the pause menu while the
host is already playing, because the second and third players do not arrive at the same moment as
the first and going back to the menu to admit one would end the game for everybody in it.

`kMaxPlayers` stays 4 and stays the only place the number is written. Nothing in the transport
cares: it holds sixteen peers and the carrier holds as many links as it is given.

## ADR-054: Sound that is still arriving is a stream, not a sound

**Status**: accepted, 2026-09-14

Proximity voice needs the mixer to play audio that does not exist yet when it starts playing. Every
other sound in this game is a complete buffer before anybody hears it; a voice coming down a wire
arrives while it is being played, in fragments, out of order, with holes.

A stream is therefore its own thing beside a sound, and the difference is entirely in what running
out means. A sound that reaches its end is finished and its voice ends. A stream that runs dry plays
silence and keeps its place, because a gap in the network is a pause in the sentence and not the end
of it. A stream is only over when it has been closed and drained, which is when the speaker has
actually stopped.

Three things follow, and each was a decision rather than an accident.

The cursor counts samples since the stream opened rather than samples into a buffer, because the
buffer keeps having its front thrown away: what a voice has already played is dropped as it goes, or
an hour of conversation would be an hour of audio held in memory.

A queue that grows past half a second has its oldest thrown away. Letting it grow is what makes a
listener permanently late: stall for a second and every word after it arrives a second behind, with
no way to catch up. Dropping audio is audible, and being late for the rest of the game is worse.

Playing a stream that is still empty is allowed. Refusing it would mean the voice opens on the first
syllable and therefore loses it.

Nothing here knows what a microphone is, and the mixer is still a pure function of its voices and
its listener, so all of it is tested without a sound card.

## ADR-055: A slope is not a staircase

**Status**: accepted, 2026-09-14

The view smooths stair steps: walking up a step teleports the capsule upward inside one tick, and
feeding that straight to the camera reads as a jolt, so the view absorbs it and lets it decay. It
decided how much to absorb by comparing where the physics put the body against where the body's own
velocity would have put it.

On a slope those two differ by the entire climb. Walking up a ramp your velocity is very nearly
horizontal and the ground lifts you as you go, so the difference was about two centimetres a tick at
walking pace on twenty-five degrees. Every tick on every ramp in the game was therefore recorded as
a step, the camera was pulled down by it and allowed to recover, and the whole body juddered.

The rise that walking along the ground you are already on would give is now subtracted first, from
the ground normal and the distance travelled. On a staircase the ground under the foot is flat, that
term is zero, and a real step is still the whole jump and still smoothed.

It survived this long because every pose test drew exactly once per simulation step at an
interpolation alpha of one, and that is the single value at which the interpolated and
un-interpolated positions agree. The game draws as fast as it can. The test that found it draws
three times per step, the way a machine running at 180 Hz does, and that is now how the slope tests
run.

## ADR-056: Proximity voice goes through the mixer like any other sound

**Status**: accepted, 2026-09-14

A voice is a thing that happens somewhere. So it is a positioned voice in the mixer, with the same
attenuation, the same panning and the same distance cut a footstep gets, rather than a separate path
with its own idea of where sound comes from. Knowing roughly where somebody is by hearing them is
most of the point.

Opus at 24 kbit/s, because voice codecs are not a thing to write yourself. A twenty millisecond
frame at 48 kHz is 3840 bytes raw, three times what fits in a datagram; it compresses to about
eighty. Frames go on the unreliable channel: a frame is useful for a twentieth of a second, a resend
would arrive after the word it belonged to, and the codec fills a gap better than a late packet
would. The decoder is told when a frame was lost rather than handed silence, so a dropped packet is
a smudge rather than a hole.

One decoder per speaker, because a codec carries the state of the conversation it is decoding and
two people through one would be nonsense.

The host decides who hears what. A client sends its own frames to the host, which stamps who they
came from — a client filling that in could speak as somebody else — and forwards them only to
players within `kVoiceRange`. Forwarding everything and letting each listener attenuate it would
work and would also put the whole conversation on every machine, which somebody could read.

The microphone is opened when the key goes down and closed when it comes up. That costs the first
few tens of milliseconds of the first word. It buys the operating system's microphone indicator
meaning what it says: a game that holds the microphone open all match and promises it is not
listening is asking to be taken on trust, and this way there is nothing to take on trust.

## ADR-057: On a slope, the eye comes down

**Status**: accepted, 2026-09-14

Walking up a ramp looked like shuffling: almost no stride, the legs barely moving. Walking down, the
legs over-extended instead. The cause is not the animation and not the cadence — the stride phase
advances at exactly the same rate on the flat, up fifteen, down fifteen, up twenty-five and down
twenty-five, 3.3999 metres a second of stride in every case.

It is the proportions. The hip sits at 0.530 of standing height and the leg plus the ankle comes to
0.542, so a body standing level has about two per cent of its leg in hand. On the flat that is
enough, because the feet are never far from under the hips. Walking up a ramp the trailing foot is
behind the body *and* below it and the two add together: the leg is asked for 0.998 of its own
length, which is dead straight, and a foot that cannot be reached is pulled in towards the hip
instead. That is the shuffle.

Two fixes were tried and measured before this one. Shifting the whole stance up the slope: the leg
stays at 0.998 through every value from 0 to 0.8 metres per unit of slope. Shortening the stride so
the foot is placed where the leg can get to: also 0.998 at every value, and it made the uphill
stride shorter still, which was the complaint. Neither works, and the reason is the same for both —
the shortfall is vertical and neither of them is.

The eye drops a little on a slope instead, up to twelve centimetres, eased like any other change of
stance. That is a person bending their knees on a hill, and it is the only place the slack can come
from: the body is anchored to the eye, so lowering the pelvis on its own nets out to nothing once
the head is put back on the camera.

Measured after: the worst leg extension going up twenty-five degrees is 0.963 against 0.970 on the
flat, and the stride went from 0.52 m to 0.61 m. Nothing is at full stretch any more.

## ADR-058: A relay, not hole punching

**Status**: superseded by ADR-067, 2026-09-21. Originally accepted, 2026-09-14, superseding ADR-048 and ADR-053

Hole punching was a mistake, and the way it failed says why. Two machines behind home routers cannot
reach each other, so both dial at once and hope the routers open. That needs each end to describe
itself to the other first, which needed a code swapped by hand; it needs one end to be "controlling"
and the other "controlled", which ICE decides from the order things happen in, which in our case was
a race between two people pasting into a box — and what that produced, in the log, was a flood of
"ICE role conflict (both controlled)" alternating with "(both controlling)". And on a symmetric NAT
it cannot work at all, at which point the answer is a relay anyway.

So: a relay. Everybody connects outwards to one machine both can reach and it forwards between them.
Outbound always works — it is why the web works without anybody configuring a router — so there are
no candidates to gather, no roles to agree, and nothing that can work on one network and fail on
another.

The costs are real and small. One extra hop of latency, typically ten to forty milliseconds. And
whoever runs the relay pays the bandwidth, which is about two hundred kilobits a second for a full
game: a four player snapshot is 89 bytes at 30 Hz, and a voice is 82 bytes per twenty milliseconds.
For co-op against an AI creature that is fine. For competitive shooting between players it would not
be.

What it buys beyond working at all is the lobby. One code, and anybody who types it joins — before
or after the game has started. Hole punching could not do that even in principle: every pair of
machines had to swap a fresh pair of codes, so a four player game was six exchanges and the host had
to be sitting at a menu for each one.

It also removed the only copyleft dependency in the project. libjuice is MPL-2.0; nothing that is
left is.

The transport did not change. A relayed link is a `DatagramCarrier` exactly as a punched one was, so
the handshake, the reliability, the prediction and the roster are the same code running over a
different pipe, and none of it knows.

## ADR-059: A room is dark because it has a roof

Before this, the dark test room was dark because a function called `InsideDarkVolume` said so. The
map declared a box, the game noticed the camera inside it, and the environment was blended down. It
was honest about being a stand-in and it read exactly like what it was: walk through the doorway and
the whole world dimmed, like a dimmer switch rather than like going indoors.

Two things stop light reaching that room, so there are two depth maps and they are the same code.

The **sun** is stopped by an ordinary shadow map: the scene is rendered from the sun's direction into
an orthographic depth target, and shading asks whether anything stands between a surface and the sun.

The **sky** is the other half, and it is the half that actually makes an interior dark. Shadowing the
sun alone leaves a room filled with the same flat hemispheric ambient as the field outside it, which
looks like a room somebody forgot to light rather than a dark one. Sky occlusion is a shadow test
with the light directly overhead, so it is the same machinery pointed straight down: a roof blocks
the sky exactly the way it blocks the sun. What is left underneath is a small fraction, standing in
for light that bounced its way in, because zero is not a dark room but a void.

Both maps are fitted around the player rather than the level: a level is larger than a map can be at
any useful resolution, and the only shadows anybody can see are the ones they are standing among.

Three things about the implementation are worth writing down because each cost time.

The maps store distance from the **back** of the map rather than from the light. An empty texel then
holds zero, and zero means "as far away as this can see", which is what "nothing is here" should
mean. Storing distance-from-the-light needs an empty texel to hold a very large number, which needs a
clear palette entry, which is one more thing that has to be working for the lighting to be right.

The maps are bound **per draw**. bgfx keeps uniform values between draws but not texture bindings: a
submit consumes them. Bound once before the loop, the first mesh reads the maps and every mesh after
it samples an empty slot — and because the first mesh is the ground, the symptom was a correctly lit
ground and a world where every other object was wrong. That looks like a broken map rather than a
lost binding, and it sent me through the projection, the culling, the depth test and the encoding
before the binding.

The nine PCF taps are weighted as a tent rather than averaged flat. Nine equal votes give ten shades
and the set of voting texels changes in a jump at each texel boundary, so shadow edges climb in
stairs. Weighting each tap by how much of it the sampling point covers makes the answer move
continuously for the same nine samples.

The consequence for level building is the point of all of it: put a roof on something and it is dark
underneath, anywhere, with nothing to declare and nothing to keep in step with the geometry.

## ADR-060: Surfaces reflect where they face

The ambient specular term was one number: the same hemispheric ambient the diffuse uses, tinted by
Fresnel and faded out with roughness. That is enough to stop metal rendering black, and it is not a
reflection. It does not know which way the surface faces the world, so a steel plate looks the same
whether it is angled at the sky or at the floor, and walking around it changes nothing on it.

It now looks along the reflected view direction and asks the same sky-and-ground hemisphere what is
over there. A floor picks up the sky, the underside of a rail picks up the ground, and both change as
the player moves, which is most of what separates a polished surface from one painted a lighter
colour. A rough surface reflects a cone rather than a direction, so the reflection vector is pulled
back towards the normal in proportion to roughness.

How much of that reflection leaves the surface comes from Karis' analytic fit to the split-sum
environment BRDF rather than from a linear fade. It carries the two things the fade got wrong: that
grazing angles reflect far more than head-on ones whatever the roughness, and that a rough surface
keeps some of that rather than none.

The reflection is occluded by the same sky term as the rest of the ambient, because a room the sky
cannot see into has nothing to reflect. Without that, every polished surface indoors would be a hole
through the roof.

This is still a two-colour environment and not a probe: it cannot reflect the object next to it, only
the sky above and the ground below. Real reflections need either probes or a screen-space pass, and
both want a deferred or at least a depth-prepassed renderer, which this is not yet.

## ADR-061: The sky is a triangle, and it goes in its own view

The background was the clear colour. It is now a procedural sky: two gradients meeting at the
horizon, a sun glow, and the same tone curve the world goes through.

It is drawn as one full-screen triangle at the far plane rather than as a box or a sphere around the
camera. A box needs geometry, a size chosen against the far plane, and care that the player never
reaches the edge of it. A triangle needs none of that, because the sky is not a thing in the world --
it is what is left where nothing in the world was drawn. One triangle rather than two, because a quad
has a seam down its diagonal and shades every pixel along it twice.

Its colours come from the same `Environment` the lighting reads, so the sky a player sees and the sky
the surfaces reflect cannot drift apart: the zenith is the ambient sky opened up, the horizon sits
between that and the fog colour the distance fades into.

Two things had to be got right and both were wrong first.

It needs its own view id, ahead of the main one. Submitted first into the main view it came out *over*
the world, because bgfx sorts the draws inside a view to save state changes and "submitted first" is
therefore not "drawn first". Views run in id order and that order is a promise, so the sky has view 3
and the world has view 4. The sky view clears the colour; the main view clears only the depth, or it
would wipe the sky before the world was drawn onto it.

And it has to go through the tone curve. Written straight out, its colours landed far darker than the
same numbers do on a surface -- the horizon read as a black band where it should have been haze --
because everything else on screen has been through exposure, ACES and gamma and it had not.

Also here: a dither of under half a level of output, in both shaders. A sky is the largest, smoothest
gradient on screen and the first place eight bits per channel shows as bands; so is a dark curved
surface lit only by a torch, which is where it was reported.

## ADR-062: The shadow map's texel snapping, and why it was never working

Shadows were reported as flickering five separate times. Each time something plausible was found and
fixed -- a fractional tap spacing, a constant bias that could not serve both square-on and glancing
surfaces, two cascades combined by taking the darker answer -- and each time the flicker came back
somewhere else. That pattern is the tell: the thing being fixed was not the cause.

The cause was in `ShadowMap::Fit`, and it had been there since the first shadow commit:

    eye  = centre - forward * (depth * 0.5)
    view = lookAtRH(eye, eye + forward, up)
    viewCentre = view * centre          // (0, 0, -depth/2), always
    snapped    = round(viewCentre.xy / texel) * texel

`centre` lands on the view-space origin by construction -- that is what the view was built to do --
so the rounding had nothing to round, the translation was the identity every frame, and the map slid
smoothly along with the player while appearing to be snapped. The grid it samples the world on
therefore moved a fraction of a texel every frame, so the filtered value for a fixed point in the
world changed every frame: every shadow edge crawled and every partially occluded patch shimmered.

The fix is to quantise against a frame that does not itself depend on the centre: a rotation-only
light basis, the centre taken into it, rounded there, and brought back out to place the eye.

Two things follow from having got this wrong for so long.

The maths is now in a free function, `FitShadowMap`, that does not touch the graphics device, and it
has a test. The bug is invisible in a still picture and unmistakable in motion, which is the worst
combination there is: no screenshot could show it and the only way to see it was to play. The test
creeps a centre past a fixed landmark in tenths of a texel and asserts the landmark keeps landing on
the same texel. It fails eight of its nine assertions against the old code.

And the filter can be narrow again. Much of the blur that had accumulated was compensation for crawl
that could not otherwise be hidden -- the player's own shadow had become a cloud. The sun is back to
nine taps and about a two-centimetre penumbra; the sky keeps twenty-five, because there the width is
the answer rather than a way of hiding the lack of one: it is asking how much of a quarter-metre
neighbourhood can see sky.

## ADR-063: Mirrors are one planar reflection, in one plane

ADR-060 gave surfaces a reflection of the sky-and-ground hemisphere, which is enough for metal not
to render black and is not a reflection: it does not know what is beside a surface, so a steel plate
looks the same angled at the sky as at the floor, and nothing in it moves when you do. The report
was that reflections "still aren't there", and that was right.

What is here now is a planar reflection. The scene is drawn a second time from a camera reflected
across one flat plane, into a texture, and a surface lying in that plane samples it at its own place
on the screen. That last part is the whole trick: the reflected image was rendered with the same
projection from the mirrored camera, so for any point in the mirror's plane the two views agree
pixel for pixel. It is exact for a flat mirror and meaningless for anything else.

Screen-space reflection was the alternative and is cheaper, because it reuses the frame that is
already drawn. It also only knows about what is on screen, so you vanish out of a mirror the moment
you step out of frame and every reflection has a ragged edge where the source data runs out. For a
game whose whole tension is what you can and cannot see behind you, a mirror that stops working when
it matters is worse than no mirror.

The limit taken in exchange is one plane. Several mirrors lying in the same plane share a pass for
nothing, which is why the three test panels are lined up; mirrors in different planes need a pass
each and this renderer has room for one. That is a deliberate cap rather than an oversight -- a
second pass over the whole scene is the most expensive thing in this renderer.

Measured on the test map, standing four metres from the panels with them filling much of the screen:
2.51 ms a frame without, 3.77 ms with. Half again as much, for one wall. It is off past thirty
metres, off when the eye is behind the plane, and switchable from the graphics page, and the target
is at half the window's width and height because a mirror is looked at through a surface and at an
angle, where a quarter of the pixels is very hard to see.

Three things had to be got right and each of them fails in a way that looks like something else.

The reflection turns every triangle inside out, so the winding to discard is the opposite one for
that pass. Miss it and the mirror shows the insides of everything.

The mirror is blended in after the tone curve, not with the rest of the lighting. What is in that
texture has already been exposed, tonemapped and gamma encoded, because the reflection pass runs the
same shader; mixed in before the curve it goes through all of that twice, which lifts the blacks and
flattens the highlights and leaves a mirror looking like milky plastic. It did.

And a mirror is not the camera. `hiddenFromCamera` -- the player's own head in first person -- means
"not from the camera's own point of view", and a mirror is precisely the other point of view. Drawn
with the camera's set, anybody looking in a mirror is decapitated, which is the same bug the head
has already had once in its shadow. There are now three renderable sets rather than two, and a test
that says what each of them leaves out.

## ADR-064: A creature is thought on the host and shown everywhere else

The creature's mind runs on the host and nowhere else. Every other machine is sent what its body is
doing -- where it is, which way it faces, how fast it is going, how far into a strike's wind-up it
is, its health and whether it is alive -- thirty times a second, and draws a copy of it.

The alternative that sounds cheaper is to run the same brain on every machine from the same seed and
send nothing. It does not work, and not for a subtle reason: a brain is only deterministic given
identical inputs, and its inputs are what each machine can see and hear, which differ by a round trip
of latency and by every player's prediction. Two copies would disagree within a second about whether
somebody had been seen, and nothing would ever bring them back together. Sending the mind's state
instead of the body's -- tracks, scores, the timeline -- was the other option, and is several times
the size to send things no player can see.

Four details, each of which is there because leaving it out breaks something:

- **The seed goes in every packet**, not once at spawn. Something sent once can be lost, and a player
  who joins late was never there to receive it. It is 32 bits a creature, and it is what lets a client
  build the same animal -- which matters little while the body is boxes and entirely once it is
  generated from the seed.
- **Every message carries a sequence number**, and a client ignores one older than what it has.
  Unreliable packets can overtake each other, and an old one applied after a new one pulls the
  creature back to where it was a moment ago.
- **An empty message is still sent.** It is how a client learns that the last creature has gone;
  sending nothing when there is nothing would leave the client drawing the last one it heard about.
- **The copy is drawn a little ahead.** Each update is already a few hundredths of a second old when
  it arrives, so the copy is eased towards where the creature was plus a tenth of a second of its
  speed at most. At a run it stays within about 0.2 m of the host's, measured -- close enough that
  a round aimed at the copy finds the host's creature, whose body is a metre and a half long.

A client's copy has a physics body where the host's creature is, so its own prediction of a shot
finds it and leaves no bullet hole, which is what the host will decide. The host's answer is still
the one that counts: a client's round is re-run against the host's world, which contains the real
creature.

What a client cannot do is look inside the creature's head. Its copy has a brain object that never
runs, and the inspector says so rather than displaying a mind that is not thinking. Streaming the
inspector's snapshots from the host is in the design (AI.md) and not built.

## ADR-065: Stalking reads gaze, playing dead is a real fall, and death stops the brain

Three decisions from Milestone 9's first steps, each made after watching the wrong version happen.

**The opening is where they are looking, not whether they can see it.** A stealthy creature waits for
a moment to strike: somebody alone, looking away, or its own patience running out. The first version
counted "they cannot see me" as "they are not looking", and the creature, crouched behind a wall with
the player staring at that wall, took the wall as its opening and stepped out into their view. Being
out of sight is what cover is for; the opening is a gaze turned elsewhere. And it only knows where
somebody is looking while it can see them -- after that it believes the last look for four seconds, and
then does not know, which is half an opening. Cover that hides it also blinds it, so it peeks: every
few seconds it leans out to a spot with a view, looks, and slips back. A peek that finds them turned
away is how the stalk ends.

**Cover is a query, not a list.** Points on the navigation mesh round the target and round the creature
itself, each scored as a product of named reasons like the behaviours are: hidden from every player it
knows about (a spot that is not scores a hundredth, so no combination of the others lifts it over one
that is), about eleven metres off, dark, behind them, near, and not reached by walking past their nose
-- the version without that last one picked excellent cover on the far side of the player and walked
slowly at them to reach it. A dark corner, a gap behind a crate, and in time a vent, are all used
without being named.

**Playing dead is the same fall as dying.** From outside the two must be identical or the trick does not
work: the same roll onto its side, the eyes going out, the body falling to whichever side has room, the
physics box lying down with it so it can be shot where it lies. On the wire it is one bit, separate from
alive, so a client draws it and cannot tell. Underneath, the brain runs, and the act has rules: only
straight after a bad wound with somebody close, only for a creature more cunning than timid, at most
twice, never again once it has been shot while down. It springs at anybody who comes within reach, and
that spring is seen through rather than reweighed -- the version that reweighed it stood up in front of
somebody, found its fear, and ran.

**Death stops the brain, and clears what it meant to do.** Nothing after it perceives, decides or moves.
The game reads the brain's intent every tick, and a creature killed on the tick a strike was due left
the strike standing, to be applied every tick from a corpse.

Also settled here: a game starts without its creature. It arrives about forty seconds in, varied by
seed, somewhere twenty metres or more from everybody and out of every player's line of sight, so
nobody ever watches it appear.

## ADR-066: A creature learns what a shut locker means; searching finishes its round

Two things from finishing Milestone 9.

**What it knows about lockers, and what it has to learn.** A locker door is shut when somebody is inside
and open when nobody is, so a creature that knew that from the start would open every shut locker it
saw and hiding would be pointless. A creature that never learnt it would make hiding a guaranteed
escape. So it starts not knowing: a shut door is a hundredth of a reason. The first time it opens a
shut locker and finds somebody, what a shut door means jumps -- and from then on, for the rest of the
match, it checks them. Hiding works the first time and gets riskier the more a group leans on it,
which is the game we want. It also suspects lockers for the reasons anybody would: a locker door heard
and not seen, somebody vanishing right beside one. It never knows who is inside until it opens the door;
the brain is handed which locker each player is in, and is only allowed to read it at that moment, the
same honesty rule as reading a gaze only while it can see a face.

**Searching is a round, not a mood.** The first version made searching an option weighed like any other,
scored on how unsure it was of where somebody had gone -- and since that uncertainty only grows, the
option fell off the bottom of the list mid-search and the creature wandered away between two lockers.
Now a search plans a round (suspected lockers first, then places spread round where they were heading)
and finishes it, however its confidence changes on the way; what ends it is running out of places or
the search going stale by its persistence, or a sound or a glimpse that makes something else score
higher. And arriving where somebody was and finding nobody is itself news: it lowers how sure the
creature is, which is what hands over from going to where they were to looking where they went.

Also settled: a creature watching somebody out of curiosity keeps facing them when it backs off. Turning
away lost sight of them, which dropped the watching, which made it hunt, which turned it back -- the same
flip-flop that stalking had between cover and gaze, and the same cure: know what you are doing
through a glance away.

## ADR-067: A lobby server introduces players, and they play directly

**Status**: accepted, 2026-09-21. Supersedes ADR-058.

The relay worked everywhere because every byte of every game went through it, and that is exactly
what made it the wrong thing to depend on: whoever runs it pays for the traffic, and a host with a
monthly allowance switches the machine off when it runs out. This project has already lost a server
for a month that way, on another game.

So the server only makes introductions. A host asks it for a code; a guest asks to be introduced to
that code; the server tells each where the other might be, and both then send to every address of the
other at once. Each router sees its own machine's packet go out first, takes the other's for the
reply, and lets it in. That is hole punching, and the game then runs PC to PC.

It is hole punching again, which ADR-058 retired, but neither of the things that went wrong then can
happen now. The players swapped descriptions by hand, and now the server carries them. ICE decided
which end was in charge from the order things happened, so the pasting raced and both ends claimed the
same role; now there are no roles to agree -- the host answers probes and the guest connects to
whichever of its addresses answered first. No ICE and no library: a token, a probe and a reply, over
the game's own socket.

The server is a Cloudflare Worker (`Tools/LobbyWorker`), not a machine. That was chosen over running
the same thing as a UDP program on a free virtual machine, which was built first and worked: a Worker
needs no card, no machine to keep patched and nothing opened on any firewall, cannot be reclaimed for
sitting idle, is deployed from GitHub with a button, and Cloudflare does not charge for bandwidth at
all. Its free plan's limit is requests per day, which resets daily rather than shutting anything off
for a month; an evening of play is about fifteen hundred.

A Worker only speaks HTTP, so it cannot see what a game's UDP socket looks like from outside. Public
STUN servers answer exactly that question (Cloudflare's and Google's, both free); the game asks two of
them through its own socket, and if they see different outside ports the router is one that makes a
new hole for every destination, which punching cannot pass -- the host is warned in its lobby. The
Worker adds one guess of its own: the address the web request came from with the game's port, which
most home routers keep. The host's router is also asked (UPnP) to let people straight in, as one more
address to offer.

Everything that has to reach another player goes through the game's own socket, because a hole a
router opens is for one socket only -- the STUN answer too, which is only true of the socket that
asked. The transport gained a side door for that (`Open`, `SendUnframed`, `TakeUnframed`): anything
arriving that is not the game's is kept, bounded, for the lobby client. The guest's transport is
opened before there is a game on it, used for STUN, the introduction and the probes, and then handed
to the session to connect over, holes and all.

The lobby itself is a screen, not a server feature: the host's session runs from the moment it hosts,
guests connect and wait, and the roster the host already sends carries one more bit -- started. Guests
go into the world when it is set, so everybody goes in together, and anybody later goes straight in.

The Worker keeps its lobbies in memory in one Durable Object. If Cloudflare restarts it, each host is
told on its next check-in that its code is unknown and asks again for the same one, which it gets
back. The lobby logic is one plain JavaScript file run three ways -- by the Worker, by a local Node
server for testing on one PC, and by its tests, which GitHub runs.

What it cannot do is connect two players whose routers are both strict. A relay for just those pairs
is the obvious next step if it turns out to matter.

## ADR-068: A game on the same network is asked for, not only listened for

**Status**: accepted, 2026-09-21

Two copies of the game on one PC found each other at once, and two PCs in the same room often did not.
Both causes were on the browsing side, and neither showed up in testing on one machine.

Windows sends a datagram to 255.255.255.255 out of one network adapter only, whichever it considers the
default. A PC with a VPN, a virtual switch for WSL or Docker, or wired and wifi both up announced itself
into the wrong one. Every adapter has its own broadcast address, which does go out of that adapter, so
the beacon now sends to all of them as well.

And Windows Firewall, on a network it treats as public, drops announcements nobody asked for — but lets
in a reply to a broadcast this machine sent, for a few seconds. So a browser now asks "is anybody
hosting?" once a second, and every host answers it directly with its beacon. The host needs its
firewall open to be hosting at all, so nothing new is asked of anybody.

Networks built to keep devices apart (schools, offices, hotels, client isolation on mesh wifi) still
stop it, and nothing inside a game can fix those. The empty list now says so, and codes go out through
the internet and back, which those networks allow.

## ADR-069: A reload is animated by hand, hands and all

**Status**: accepted, 2026-09-21

A reload used to be half authored and half written in: a model's clip could move the weapon's parts,
but the support hand followed rules in `PlayerBody` -- to the magazine well at one fraction of the
reload, the belt at another, back at a third. None of that could be tuned without code, and a reload
that is not a simple swap, such as flipping a pair of taped magazines, could not be made at all.

Now a clip can carry tracks for the hands. A hand's key is its offset from the socket it rests on, in
the weapon's frame, and its wrist turn; while the clip plays, that hand goes exactly there, through the
same arm solver the ordinary hold uses. The written-in reload remains only for models whose clip has
no hand in it.

Parts can be held. Each key says what carries the part from that moment -- the weapon, the left hand or
the right -- and a part in a hand is placed in that hand's frame, so it goes wherever the hand goes. A
part changes holder all at once, at its key: the keys either side are in different frames and a blend
between them means nothing. The editor's *Held by* recomputes the key so the part stays where it is on
screen, which makes "the hand takes the magazine" one change rather than two numbers to line up.

The clip is the reload's length. A weapon whose model has a `reload` (or `reload_empty`) clip reloads
in exactly that clip's duration. Otherwise the two could disagree -- the gun ready to fire while the
magazine is still in the hand, or the hand stood waiting at a finished reload -- and the only fix would
be to keep two numbers in two files the same by hand. A reload from an empty magazine is its own clip
and its own length, chosen by the simulation when the reload starts, and it crosses the wire as one
bit so everybody else sees the right one (protocol version 8).

Templates make a reload that already moves -- including a double-magazine flip whose shape is the same
after the roll, so the clip can end with every part at rest -- because shaping something is far easier
than starting from an empty timeline. The carbine ships with its templates as its reloads.

## ADR-070: What a creature can do comes from its body

**Status**: accepted, 2026-09-21

Speed, sight, hearing and reach used to be drawn from the seed as numbers beside the temperament. Now
the seed makes a body and those follow from it: legs set the speed, eyes the sight, frills the hearing,
neck and head the reach, bulk the health. A creature that is fast because its legs are long reads as
one thing; a creature that is fast because a number said so, while it waddles on stubby legs, reads as
a bug. It also makes every seed consistent across machines for free, because the body is rebuilt from
the seed everywhere.

The body's rest pose is worked out once, in one place, and everything that needs to know where a part
is -- the drawing, the box rounds hit, where it sees from -- asks it, so they cannot disagree.

Health is large on purpose. A predator that drops to a burst is a target, so a medium body takes two or
three carbine magazines and the biggest far more. Because the brain's pain and harm were written against
160 health, they are now measured against the body's own: the same round hurts a big body less. A
cvar scales it all for tuning.

Two things moved with the body. The brain's assumptions about height -- eyes at a metre, a body
middle at 0.7 m -- were right for one body and wrong for a 0.5 m six-legged one, which then could not
peek over the cover it chose; they come from the body now. And sight passes through creatures: its eyes
are inside its own box, and with rays that count a start inside something as a hit, a second creature's
box around its head blinded it outright.

## ADR-071: Creatures are skinned as one body, and a crawler is one of four kinds

**Status**: accepted, 2026-09-22

Bodies built from separate ellipsoids and capsules looked assembled: every joint showed two ends, the
body was a row of beads, and big glowing spheres for eyes read as a toy. Horror comes from something
that looks like it could be alive. So a creature is now drawn as:

- one lofted skin for the torso, with ribs and a spine pushed out of the surface;
- limbs of tapered segments that meet inside a shared joint ball, so they bend as one limb;
- a skull with a hinged jaw and teeth;
- eyes as sunken hollows with a dim point of light, placed on the skull's own surface so they cannot
  float off it however many there are.

All of it is still built from the seed at spawn. Nothing is streamed or authored, so every machine
builds the same animal.

The crawler, a gaunt human-like thing on all fours, was added as a fourth kind of body rather than
replacing the others. For a moment it was made most of them, and that was the wrong call: a monster
you have seen before is a monster you know. Each kind turns up about as often as the others, and a
set of per-seed details (snout, gape, teeth, ribs, fingers, claws, brow, skull) makes two of a kind
differ. Adding the crawler changed every seed's body once; the draws were reordered freely because no
save data or network message depends on them.

## ADR-072: A creature is one skinned mesh sculpted from a distance field

**Status**: accepted, 2026-09-22

Blending separate primitives (ADR-071) still read as parts: every shape was its own mesh, and the eye
finds the seams. A creature is now one mesh on a skeleton.

- **The shape** is a signed distance field. Bones, muscle, ribs, vertebrae, shoulder blades, the skull
  and the joints are rounded cones and ellipsoids joined with a smooth union; the sockets, nostrils, mouth
  and temples are carved out with a smooth subtraction. That is what gives joints, a neck growing out of
  the shoulders and ribs standing out of the chest, rather than tubes pushed into balls.
- **The mesh** comes from sampling the field on a grid and running surface nets over it. Surface nets
  gives smoother results than marching cubes for the same grid, and has no special cases. The grid is
  sampled exactly only near the surface and blended elsewhere, and the work is spread over every core.
  meshoptimizer's attribute-aware simplifier then cuts the triangle count by about two thirds, keeping
  normals and paint. That is the one new dependency.
- **The paint** is a vertex colour. The engine's single vertex format gained an RGBA8 colour, white by
  default: the mesh shader multiplies it into the albedo, and its alpha scales roughness. One mesh can
  then be skin, bone, gums, sockets and claws, with a crease-shadow term baked in. A texture atlas would
  have needed unwrapping a procedural mesh for no gain at this resolution.
- **Skinning** is on the processor into a dynamic vertex buffer, up to four bones a vertex, weighted by
  distance to each bone's shapes. Eight creatures of 10,000 vertices is well under two milliseconds, and
  there is no second shader or vertex format to maintain. GPU skinning remains the path if creature
  counts grow.
- **Animation** is procedural (`CreatureRig`): world-planted feet that step when the body has moved on,
  ground by ray, spine bend, head look, a spring tail and poses for each action. There are no authored
  clips because every seed is a different body.
- **Death** is a Jolt ragdoll built from the same bones. It needed swing-twist joints and two physics
  layers: ragdoll pieces, which touch the level and loose items but not people; and hit zones, which
  touch nothing and are found only by rays. Every machine simulates its own fall, since nobody acts on
  where a corpse's hand is.

Skins are built from the seed, so every machine builds the same one, and they are cached by seed.

## ADR-073: Attacks, grabs and the nest; jumps baked into the navigation mesh

**Status**: accepted, 2026-09-22

One slow blow with a long cooldown made the creature easy to dance round. It now has four blows: a swipe
landing a quarter of a second after it starts, a bite, a lunge from a few metres, and a grab. The next can
follow almost at once. A grab is the creature's answer to a team: it takes the one on their own away from
the others, to kill them or to wrap them in a cocoon at its nest. The team answers by shooting it off them,
cutting them free, or the victim struggling. A held player is pinned by the host, which says so in the
snapshot, and their own machine stops predicting until they are let go (protocol 9).

Reaching somebody up on something is a navigation question, not an animation one. So jumps are found
when the mesh is built, across every open edge down to floor within four and a half metres with the way
clear, and baked in as Detour off-mesh links flagged by height. A creature's body decides which flags it
may route through. That answers "can it get up there" the same way everywhere, including in the tests.

Doors are kinematic and the mesh runs through them, so the brain checks the next leg of its route against
shut doors and opens or breaks them. Letting the mesh be cut by doors would have meant rebuilding it
whenever one moved.

The creature lab is part of the test map's world rather than a second map, because the whole game assumes
one world built behind the menu. Moving everybody there is a respawn at a different spawn point, which
every part of the network already handles.

## ADR-074: CIRRA, and creatures that are not all hunters

**Status**: accepted, 2026-09-23

The world, the organisation and the organisms are now described in
[Project_Predation_Lore_Reference.md](Project_Predation_Lore_Reference.md), written by the project's owner.
It is the source of truth for anything the game says about them, and nothing in the code writes lore of its
own. Three decisions followed from it, each asked and answered rather than assumed:

- **The organisation is CIRRA**, the Critical Incident Response & Research Agency, replacing ACRD. That is
  every name a player reads and the settings folder: `%APPDATA%\CIRRA\ProjectPredation`. The first run under
  the new name copies the old folder across, so nobody loses settings to a rename; the old one is left
  where it is.
- **A creature has a temperament** -- predator, territorial, timid or curious -- because the lore is clear
  that these are animals and not monsters: a frightened one, a wary one and a watching one are as much
  what they are as a hunting one. Most still hunt. Any of them is dangerous to somebody who hurts it.
- **Only some take people.** Grabbing, carrying off, nests and cocoons belong to predators and territorial
  ones with a strong nesting trait; the rest fight or keep away.

The temperament is drawn after every other trait, so each seed keeps the body and senses it had.
Hunting tests pin their creatures to predators, since a timid one would pass a test of hunting by keeping
out of the way; the temperaments have tests of their own.

## ADR-075: Lamps that belong to the place, lit per surface

**Status**: accepted, 2026-09-23

The levels had no lights of their own. Everything was the sky, a little ambient, and whatever torches and
muzzle flashes were about, four of them for the whole frame. A building needs a lamp in most rooms.

Each surface now chooses its own lights: of every lamp in the level and every light of the moment, the
ones that reach its bounding sphere, strongest at its nearest point first, up to eight. The first slot is
still kept for the one light with a shadow map -- the local torch whenever it is lit -- because only that
slot reads one. This is the forward renderer's standard answer and costs nothing on the GPU that four
slots did not; the price is on the CPU, a few thousand distance checks a frame. The one thing it needs
from a level is that a surface is not enormous: a floor the size of the building is lit by the handful of
lamps nearest its middle and dark at its ends. So MapBuilder draws any box wider than eight metres as
tiles, collided as one piece, and the lab's floor is laid as tiles.

Clustered shading -- a grid of the view, each cell holding its own light list, read per pixel -- was the
alternative, and would take hundreds of lights without tiling anything. It is a larger change to the
renderer and to every shader that lights, and the levels do not yet have enough lamps to need it. It is
the next step if a generated facility does.

Lamps (Game/World/LevelLights) are a kind -- ceiling strip, caged wall lamp, battery emergency lamp,
floodlight -- and a mood -- steady, flickering, failing, dead, or pulsing. Moods are worked out from each
lamp's seed and the time, so every machine shows the same sort of flicker with nothing sent. Lamps are on
circuits that can lose their power; emergency lamps ignore that and are what is left in a building with
the power out. What a creature can see counts the lamps with a clear line to the player, and the nearest
failing lamp buzzes and stutters with its light. `r.lamp_scale` scales them all; `light_report` lists
those near you.

## ADR-076: A nest is a heart on a wall and growth that is not solid

**Status**: accepted, 2026-09-24

A nest was a sculpted mound on the floor with a static box round it, so that people walked round it.
The creature that built it was standing on that spot while it built, and the box was made round it:
every nesting creature trapped itself in its own nest. Rebuilding the navigation mesh round the box
also took a worker thread and a swap each time.

A nest is now a heart hung on the nearest broad wall and patches of growth cast out from it onto
whatever surfaces surround it, appearing over five minutes from nearest to furthest. None of it
collides with anything; the heart has a hitbox on the layer rounds find and nothing else does. The
navigation mesh never changes for a nest.

What is sent is only what cannot be worked out: where it was built, its seed and its age (NestBuilt,
with twelve bits of age added), and each wound to its heart (NestWounded, the fraction left, nought
when it bursts). Each machine finds the same wall and the same surfaces with the same rays against the
same static level, so the growth itself is never sent, and a newcomer is told every nest with its age
and wounds. Protocol version 11.

Sculpting a heart and its growth takes around 40 ms, so it is done on a worker when the nest is built
and put in the scene when it is ready.

## ADR-077: Crawlspaces in the navigation mesh, and tactics as place queries

**Status**: accepted, 2026-09-24

A creature too big for the crawlspace hunted somebody lying in it by climbing onto its roof: the roof
was the standing floor nearest them, the route to it counted as reaching them, and it stayed there.

The navigation mesh is now built for a crawling body (a metre of headroom) with floor lower than a
standing body marked as crawlspace (a Recast area of its own, a Detour flag, `NavMesh::kCrawl`). Every
query takes the same `allowed` flags a route already did for jumps, so a body that does not fit never
sees crawlspace floor at all and one that does is routed along it. The alternative, a second mesh per
body size, costs a second build and a second copy of every query for one flag's worth of difference.
A route that ends at a different height from where it was asked to go no longer counts as arriving.

What a creature does about somebody it cannot follow, and the rest of its new tactics -- cover against
something rather than merely out of sight, ambushes beside doors and crawlspace mouths, going round to
gunfire rather than straight at it -- are queries over places scored like its options, in
CreatureTactics.cpp. Two new behaviours carry them, Ambush and Flank; the protocol's four bits of
behaviour still hold them.

## ADR-078: Climbing over the floor's navigation, drawn in the surface's frame

**Status**: accepted, 2026-09-24

Creatures that climb go up walls and across ceilings. The choices were a navigation mesh for every
surface -- walls and ceilings voxelised as floors in their own frames, joined at their edges -- or the
floor's mesh with the creature drawn somewhere else. The second is what is built: a creature on the
ceiling is over a point on the floor, its route is the floor's route, and its body is drawn on the
ceiling above the point it is at. Ceilings in these levels are flat and cover the floor they are over,
which is when that is exactly right; where they stop it lets go. Walls are only climbed up, from the
floor to a ceiling, never along.

The rig takes a surface rotation, and draws the whole body in that frame; its feet find the surface with
a probe along the body's own down. Only five things in it were measured against the world's up -- how
far a foot is from where it wants to be, how high a step lifts, how high the body rides over its feet,
and the tail on the floor -- and they are measured against the surface now.

The brain plans from the floor point under it, sees from its eyes, and knows it is up there. Its body,
hitboxes and eyes follow what is drawn. Clients are sent what it is clinging to and which way the wall
faces; protocol version 12.

## ADR-079: Mimicry sends the frames that were heard, with each player's leave

**Status**: accepted, 2026-09-24

Creatures that mimic say back phrases players said. The host keeps each player's recent phrases as the
Opus frames that arrived and, to mimic one, sends those frames again as the creature's voice. Nothing is
decoded, altered or re-encoded; nothing is stored anywhere but the host's memory, for the match.

Consent is per player and per frame: every voice frame a client sends carries its player's own
"may be mimicked" bit, and the host keeps phrases only from frames that have it and forgets a player
entirely on the first frame that does not. The host's own player is under the same setting. It is on by
default, which differs from the design plan's "opt in"; the setting is in the voice settings with what
it does written on it, and ai.mimic turns the behaviour off for a whole game.

Voice frames gain a creature bit (with an eight-bit creature number in place of the three-bit player)
and the consent bit; protocol version 13.

## ADR-080: Items are used by fire, as data, with the host deciding what a use did

**Status**: accepted, 2026-09-24

The items other than the weapons could be picked up, carried and dropped, and did nothing. Each now has
a use, written in items.json as a kind, a length, an amount, sounds, and a motion -- keys of where the
hand has the item how far through -- rather than as code per item. The kinds are few and each is a real
mechanic: heal, recharge (a torch cell that now runs down), unlock, flare (struck, then thrown, burning
as a light in the world) and inspect.

The motion moves the one-handed carry the body already had, in the view's frame, so it looks the same
for any item and on anybody's body: the host tells everybody when somebody starts, finishes or stops,
and each machine plays the same keys on that player's hand. The alternative, an authored model with
parts and clips per item as the weapons have, is where items go when they have real meshes; the keys
here would then drive the whole item as a weapon clip's root track does.

A client's use is a request, like everything else. The host checks the client carries one, applies the
effect (health through the controller it simulates for them, a door through the world it owns, a flare
it puts in the world and announces), and takes a used-up item off its tally of what the client carries.
The torch cell is the one thing a client keeps to itself: it is only ever its own torch.

Data files the game writes are now written as a person would write them (JsonText): numbers rounded to
four places, arrays of numbers on one line. items.json saved by the editor used to come back with
-0.10599999874830246 and every number of a colour on a line of its own.

Protocol version 14: an ItemUse message, three world events, and five bits of event kind.

## ADR-081: Data files written tidily, typos named, sounds split by category, creature tuning as data

**Status**: accepted, 2026-09-24

Four changes to how Assets/Data works, all so the files can be edited by hand with confidence:

- **Written as a person would write them.** Everything the game saves -- items, models, the player
  tuning, input bindings, settings -- goes through JsonText: numbers rounded to four places with the
  trailing zeros off, arrays of numbers on one line. A file the game has saved is no harder to read
  than one somebody typed.
- **Typos are named.** The item, weapon and sound loaders compare every key against the keys they
  read and log the rest -- "items.json: 'medkit' has a key nothing reads, 'max_stak' -- a typo?" -- and
  a test holds the shipped files to having none. A misspelled key used to be silently ignored, and the
  setting it was meant to change kept its default.
- **The sound library is one file per category**, Assets/Data/Sounds/<Category>.json, matching the
  folders in Assets/Audio, each sound under its short name; the file's name is the category. One
  36 KB file of seventy-odd entries was hard to find anything in.
- **What every creature's mind shares is data**: Assets/Data/creatures.json -- sight range and field,
  how fast somebody is made out, how far sound carries through walls, how much better a new plan has
  to be, how long stalkers and ambushers wait, how often a voice is used. Read at start and again
  whenever the file changes. What makes one creature different from another stays in its seed. The
  shipped values are the ones the code had as constants, and a test says so.

## ADR-082: A director over the creatures, and learning by reinforcement

- **Two minds, as the best-known example of this kind of game has it:** the creatures, which know only
  what they perceive, and a director, which knows where everybody is and uses it only to pace -- a
  nudge towards somewhere near the players when it has been quiet, and a request to give them room when
  the pressure has gone on. Neither ever hands a creature a position. A creature busy with somebody
  refuses the request, so nothing walks away from a sound it has not looked into.
- **Machine learning, deliberately small.** A multi-armed bandit over five tactics, shared by the brood,
  rewarded by blows and grabs and punished by wounds and deaths, reset each match. No trained model:
  one would need data we do not have and would behave in ways nobody could tune or explain. A bandit
  learns within a match from a handful of encounters, is a few lines, shows its reasoning in the log,
  and only tilts choices, so no tactic is ever written off or forced.
- **No killing while held.** A grab ends at a cocoon or a throw, never a death in its arms.

## ADR-083: Creature bodies reworked, and variety from a stream of its own

- **Limbs are anatomy, not tubes.** Each segment is a bone with muscle over it; a joint is knuckles and
  a point on the outside of the bend; the far segment thins to tendons; the root is a shoulder or
  haunch grown out of the torso. The one sculpture system is kept -- it was the right base.
- **Variety is drawn from a second random stream** (build, head, markings, growths, and a runt or
  brute now and then), so adding it did not reshuffle what the first stream draws. The build bends the
  body's measurements before the legs are fitted to them, so every build still stands.
- **Jaws are fitted, not drawn.** The lower jaw's length comes from where the upper face ends; the
  jaw is narrower than the upper row of teeth and the lower teeth sit inside and between them, with
  lengths capped to the room the mouth has.

## ADR-084: A post-processing pass, and a mixer that hears walls and rooms

- **The picture is finished in a pass of its own.** The scene is drawn in linear light into a
  half-float target; bloom, exposure, the filmic curve, grading, vignette, grain, a lens fringe and
  the fear effect happen after. Offscreen views (icons, the editor) still draw finished pictures, so
  nothing that samples them changed. Multisampling moves from the screen to the target.
- **Fear is shown, not told.** No meter: the edges of the picture close in, beat and drain of colour,
  and a drone swells and your heart beats, as something close is after you, has you, or you are badly
  hurt or hiding with something near. It lets go slowly.
- **Sounds hear the level.** Each is muffled by what lies between it and the ears (two rays, a low-pass
  and a drop in level), and the room the listener stands in is measured and fed to a reverb. Both are
  cheap enough to do for every sound, which is the only way they stay consistent.

## ADR-085: Generated facilities, planned from a seed and built as boxes

- **A plan, then a building.** `FacilityLayout` plans a facility from a seed with no engine at all:
  a grid of 2.5 m cells, two or three floors of 3.6 m, rooms joined by corridors that loop (a tree of
  the nearest, then a second way out of nearly half the rooms), stairwells that climb through both
  floors, locked doors only on dead ends with the keycard somewhere reachable without it, lamps with
  moods, lockers, supplies, clutter, a way in and a dark room out of the way for a nest.
  `FacilityMap::Draw` turns the plan into boxes, flights of stairs, lamps and a list of doors, lockers,
  crates and items, still without an engine, so tests can check that nothing solid is inside anything
  else, that every door fits its doorway, and that every room can be walked to. `Build` puts it in the
  world. Only the seed is sent: every machine builds the same one.
- **Vents are a network.** Crawlspace ducts (1.3 m, too low to stand in, which the navigation marks as
  crawl) run room to room, room to corridor, and into ducts already dug. Two duct cells that touch are
  always joined; where one meets a room or a corridor it opens low at the foot of the wall.
- **Walls without overlaps.** Walls stand on the edges between cells, 0.2 m thick, merged into runs;
  runs along x own every corner they reach and runs along z stop at their faces, so no solid is inside
  another, which is what the level's own geometry check looks for.
- **In the one world.** Between the test map and the lab, where it makes the single navigation mesh no
  bigger. `facility [seed|new]` goes there (rebuilding it first); the host tells everybody the seed,
  and a newcomer hears it before anything numbered in it. Door, locker and crate numbers go over the
  network in a byte now, not six bits (protocol 17).
- **Lamps stop at the floor.** Nothing casts a lamp's shadow, so the facility's lamps reach 5.4 m rather
  than their kind's 9 m: far enough to light their room, not far enough to light the one below.

## ADR-086: Light through doorways, bounded lamps, and doors creatures go round

- **Every lamp is bounded to its room.** A lamp lights only inside a box a little bigger than its room
  (or its straight run of corridor); the shader drops anything outside it. That stopped light going
  through walls, and it also stopped light going through doorways. So a doorway gets a weaker copy of
  the lamp beside it (`LevelLights::AddSpill`): the same flicker and mood, 45% as bright, bounded to the
  next room, with no fitting of its own.
- **Doors fill their frames.** Panels are 2 cm narrower and lower than their holes, not 10 cm. Doors
  whose swings would cross hinge the other way.
- **An open door is in the way.** The navigation mesh has every doorway open and knows nothing of the
  panels, so creatures walked through open doors. Each door now reports where its panel stands. A
  creature heading across an open panel aims for a point past its free edge, and is kept its own
  half-width off it, on the side it came from.

## ADR-087: Creatures on more than one floor

- **Height counts between storeys.** A creature's sense of "how far" ignores height, which is right
  for a block or a crouch and wrong for the floor above. More than 2.8 m apart vertically now counts as
  far (the flat distance plus twice the height). A creature no longer stands under where it wants to be.
- **Noises are heard on the floor they were made on,** dropped from where they were made (a gun at eye
  height) to the floor under it.
- **Nowhere to stand on the stairs.** Random places to wander to, wait at or listen from are not
  picked on a sloping polygon if anywhere else will do. A place to go round to a noise must be on its
  floor and near it by walking, not only as the crow flies.
- Tested by sending creatures up and down every stairwell of three generated facilities, after a
  noise and after somebody they can see, including one that goes about on ceilings.

## ADR-088: Lockers you look out of

- **Slits, and a fixed view.** A locker door has a band of louvred slits at eye height. Hidden, the view
  looks straight out through them and turns only 28° either way, 22° down and 12° up: what a body shut
  in a locker could see, as in Alien: Isolation. The door still collides as a solid panel.
- **Prompts are for glancing at.** A locker's prompt comes up for somebody looking into its doorway, not
  at its foot. Every interaction prompt is 30% larger on a darker, edged backing.

## ADR-089: Nests that creep, beat outwards and die from the heart

- **Grown over surfaces, not lines of sight.** After what the heart can see close round it, a nest
  creeps outwards patch by patch across the surfaces: into a corner and up the next wall, over an edge
  and round onto its far side. At most 17 m along the way it grows and about 400 patches, over seven
  minutes (`ai.nest_growth_seconds`). A patch that turns onto a new surface starts a little way up it,
  and may run into corners but not off edges.
- **The beat goes all the way out.** Each heartbeat travels out through the whole nest from the heart,
  a swell and a faint glow, weaker the further it goes.
- **It dies from the heart outwards, and something stays.** The heart bursts and slumps first; the
  death spreads out at 0.75 m/s; each part darkens, slackens and rots down to a third of its size over
  `ai.nest_rot_seconds`. The husk is never removed.

## ADR-090: Lamps are chosen per piece by nearness, and throw their light down

- **Up to twelve lamps per drawn piece, tied by nearness to its middle.** Each drawn piece of level is
  lit by the few lamps that reach it. With eight slots and every lamp touching a long wall scoring the
  same, neighbouring pieces chose different lamps and a hard line ran down the wall between them. Ties
  now go to the lamp nearest the piece's middle, boxes are drawn in 4 m tiles, and there are 12 slots.
- **Doorway light only through open doors.** A doorway's share of a lamp sits just through the doorway
  with a small source, and is let through as far as the door hung there is open.
- **Corridor lamps light the corridors that turn off their run,** so a corner or junction does not end
  the light in a line.
- **Ceiling lamps are downlights** (full within 45° of straight down, gone by 100°): the top of a wall
  beside one is no longer the brightest thing in the room.

## ADR-091: The host eats; everybody else is told (protocol 19)

- Feeding is decided only on the host. Every bite mark and every piece torn off a body is sent as a
  `CorpseBitten` event -- where, which part, whether it came away, how much is left -- and applied the
  same everywhere. Eating separately on every machine from the creature's animation made each screen
  show a different body.

## ADR-092: What creatures hear is a guess, and what they do up close

- **Heard positions are approximate:** within a tenth of the distance in the open, a quarter through a
  wall or shut door (at most 5 m). Taken exactly, footsteps let a creature follow somebody through
  doors as if it could see them.
- **Nobody walks up to one unanswered.** A watcher backs off at a proper pace; pressed within 3 m it
  turns on them if bold or cornered, and otherwise runs. Anything with eyes sees somebody within 2.2 m
  in front of it, however dark. A test walks, runs and creeps up on thirty creatures of every
  temperament.
- **Hiding is against something.** Cover and hiding places in the open count for almost nothing; a
  creature watching from cover faces the doorway or corner the player would come through.
- **Wall climbing stays inside the building:** no sliding into a wall standing across the way, no
  climbing beside stairs, down again from a wall that goes nowhere, and never more than 12 s on one.

## ADR-093: Nests grow as roots between lumps; voices are levelled

- **Roots, not circles.** A nest creeps over surfaces patch by patch; each patch has a root that grows
  to it from the patch before (bent into corners, never across the air), and an irregular lump where it
  arrives. Each beat lifts it in a ring moving out at about 4 m/s; nothing flashes.
- **Voice chat is levelled per speaker** to a strong speaking volume (up to 12×), eased and soft-limited,
  and full volume to 8 m. The microphone test plays the same.

## ADR-094: No blind steps, eyes on this side of walls, sky slack capped on walls

- **A creature never takes a step the navigation cannot.** When the query fails (a mesh still being
  rebuilt for a level just put in, a creature off the mesh) it stays put or eases back onto floor it can
  reach without passing through anything. Taking the step straight walked creatures through walls,
  shuffled them in jerks at doorways and put them outside the building.
- **It senses from its eyes on this side of a wall.** A long creature's eyes are well out in front of its
  body; facing a wall close up they were through it.
- **Open door panels steer, never push.** A creature goes round a panel that has finished opening and
  ignores one still swinging; it is never shoved by a door it has just opened.
- **Sky slack on walls is capped at 0.2 m.** It grew with the shadow distance and came to more than a
  roof's thickness, lighting a band along the top of every wall -- worse on the higher settings.
- **The facility's lamp boxes have no margin** (their edges lie in the middle of walls), so neighbouring
  boxes no longer overlap into a double-lit strip, and every lamp adds a little bounce light from all
  directions so ceilings are not pitch black over a downlight.
- **The director hints 10-25 m from a player,** and a new creature restarts the quiet it waits for.
- **Dying, a body goes along the blow with a little lift,** and a ragdoll joint pushed into a ceiling
  never takes that ceiling's top for its floor.

## ADR-095: The dead drive a CIRRA support drone

- **Dying puts you in a drone, a few seconds later** (`game.drone_seconds`, 4 s), set down at the
  insertion point -- side by side when several are down. It is a knee-high tracked machine with a camera
  on a mast (`Game/Player/SupportDrone`): a real body in the physics world, not a free camera. It sees
  only what its camera sees, and a dead player's voice comes from it.
- **Driven like a person walks:** the stick is read in the camera's frame; the tracks turn it to face
  that way and then drive, and pulled back it reverses. The tracks set its velocity along its own
  forward and its spin about its own up each tick; falling, bumps and tipping stay the physics'. Low
  friction on its tracks, high on its hull, so it rolls driven and does not skate when knocked over.
- **It can be knocked over and gets itself back up:** on its side or back and still for 3 s, or when
  its driver presses jump, it kicks off the floor and is rolled upright by a controller over about a
  second (a single spin kick overshot onto its other side). The picture tips with it.
- **It cannot hurt anything, and can be hurt.** 100 health; creatures within 1.6 m swipe it out of
  their way (34, with a shove and a spin, 1.8 s apart); rounds knock it along and hurt it. At nothing it
  shuts down for 10 s -- no picture, only static -- and comes back with 60. Its motor is heard a few
  metres off, anonymously, which is what draws creatures to it.
- **Its owner simulates it.** Its state (where, which way up, where the camera points, health,
  reboot, lamp) rides with the owner's input to the host and with their snapshot to everybody else --
  one bit when there is none. The host decides every blow and sends `DroneHit`; the owner applies it.
  Protocol 20.
- **Somebody else's drone is shown, not simulated:** its body is on the hitbox layer and put where it
  is told, so rounds find it and nothing collides with it. It was first a kinematic body moved once per
  rendered frame; a kinematic move sets a velocity for the time given, the physics then steps a longer
  fixed tick, and the overshoot grew every step until it overflowed and closed the game a couple of
  seconds after a drone arrived. Kinematic moves belong in the fixed update, with the fixed step.
- **Death is for the rest of the deployment in the facility** (`game.permadeath`), and only a pause in
  the testing area. `PlayerDied` says which. With everybody down for good the deployment is over:
  no drone is sent, and after `game.wipe_seconds` everybody comes back at the insertion point -- the
  stand-in for the craft flying itself home until the ship exists.
- Dead, the hotbar, crosshair and condition bars go; the drone's lamp takes the torch's key and slot.
- `net_host` now marks the game started, so a player joining a console-hosted game is not left in a
  lobby waiting for a start that has already happened.

## ADR-096: The mission site: several buildings on open ground, closed in by rock

- **A site is planned from a seed** (`Game/World/SitePlan`): two or three buildings, each a
  `FacilityLayout` of its own size and height, on 190 m of open ground; a pad near one edge where
  everybody arrives; rock all the way round in two ragged rows, the back one taller, so the edge is a thing
  you can see and not an invisible wall. Only the seed is sent. `SiteMap` builds it; the game's
  `facility <seed>` command and map now mean the site.
- **Buildings are placed anywhere and entered from outside.** A layout has an origin and options (size,
  floors, ways out). A way out is a doorway in the building's outer wall on the ground floor, straight into
  a room or corridor that reaches the wall or along a corridor dug in to the nearest one; the first faces
  the landing. Every building now has an outer wall all the way round (outside the grid is a space of its
  own), so from outside it is a closed block. The default 24-by-24 facility still plans exactly as before.
- **Outside is night.** Each site picks a sky -- a low moon, overcast and moonless, or blowing haze -- and
  the scene takes its sun, ambient and fog while the picture is taken from there, and gives them back on
  leaving. Floodlights hang over every door and stand on poles round the pad and along the way from it to
  each building, some flickering, failing or dead. A pole holds its lamp out on an arm: a lamp inside the
  top of its own pole is inside it as far as its shadow goes, and lit nothing.
- **Cover and landmarks:** freight containers (some stacked), boulders, fuel tanks by the buildings, and a
  pipe on supports from one building to the next, high enough to walk under -- something to follow in the
  dark.
- **Navigation is of where the players are**, the site or the testing area, never both: they are far
  apart and nothing walks between them. It is rebuilt when everybody goes somewhere else (creatures wait
  for it, as they already did). The testing area's mesh builds in half the time it did.
- Open ground and rock are drawn in 24 m pieces rather than 4 m ones: few lamps reach them, and a ground
  cut into 4 m squares was thousands of things to draw.

## ADR-097: Lamps by clusters, levels drawn in batches, lamp shadows filtered smooth

- **The main view lights by clusters.** Each frame the view is cut into a grid (16 across, 9 up, 24
  depth slices, exponential out to 200 m); every lamp that reaches something on screen goes into the cells
  its reach (its sphere, cut to its room's box when it has one) overlaps; three small textures carry the
  lamps, each cell's list and the lists themselves. A pixel is lit by exactly the lamps of its cell. It
  replaces choosing the twelve lamps nearest each drawn piece, which is what put hard lines between two
  pieces that chose differently and forced the level into four-metre pieces so that the choice was local.
  The torch stays in the list with its shadow; mirrors and item icons still use the list.
  `r.clustered_lights 0` goes back to the list, for comparison.
- **Levels are drawn in batches.** With the lighting no longer tied to how the level is cut up, the map
  builder merges every static piece of one material in one 16 m cell (24 m outside) into one mesh; each box
  still has its own body. The site went from about 6,000 drawn things and 12,000 draw calls a frame to
  under 1,000 of each; on the development machine from 60 frames a second to about 350 outside, and from
  100 to 200 or more inside.
- **Lamp shadows are filtered smooth**: sixteen readings over the four-by-four texels round a point,
  weighted by where it falls between them, instead of nine readings a whole texel apart. The nine made
  a staircase of every shadow edge -- most visibly the edge of light through a doorway.
- `perf_report [frames]` logs the averaged frame, GPU time, draw calls and timed sections, for measuring
  changes like these.

## ADR-098: Buildings built as one structure; bigger sites; death is for the mission; the drone to play

- **A building is one structure, not floors stacked up.** Its outside is a shell a face at a time, from
  under the ground floor to a parapet over the roof, thicker than any wall inside and clad, covering every
  slab's edge; the ways out are cut through it, with a sill across each doorstep. The faces along x own the
  corners and the faces along z stop at them, and the walls along z stop just inside every wall along x they
  meet, so no two faces ever lie in one plane (what showed as flicker on the corners and the doorstep).
  Lintels over doorways are flush with the wall either side.
- **Stairwells are one cell wide**, the flight as wide as the arch into it, and their walls go on up the
  whole storey to a hair past where the next floor's walls begin, so the slab between is never seen edge on
  from the stairs and no line of light shows where the two meet.
- **Two tests hold the structure to that:** rays from the middle of every stairwell, every four millimetres
  of the way up to the next floor, must meet the wall's face and nothing behind it; rays from outside, every
  centimetre of every face from the ground to the roof, must meet one flat face. Put back the old 3 cm gap
  and the first finds 128 cracks.
- **Plans make more sense.** A doorway is never put within two cells of another on the same wall; a room's
  own doors are reused before a new one is cut. A duct must save a real walk -- at least six cells shorter
  than going round by the doors -- or it is not dug, and no two duct mouths, or a mouth and a doorway, are
  within three cells of each other.
- **Bigger sites, wider halls.** Cells are 3 m, not 2.4 m. A site is 300 m across with three to five
  buildings; the main one 24 to 30 cells a side and three or four floors, the rest 12 to 20 cells and one to
  three floors. Straight corridors four cells or longer are sometimes widened to two cells, into solid rock
  only. Getting the data out of one is meant to take a while.
- **Navigation is built in tiles** when a map is too big for one piece (its compact heightfield indexes
  cells in 24 bits, and a site at 0.12 m cells overflowed it and came out empty). Tiles of 512 cells are
  built on up to eight threads and joined; jumps between floors are found after. A map that fits in one
  piece is built exactly as before. The site's mesh builds in under two seconds.
- **Death is for the rest of the mission.** Nobody respawns: a player who dies watches, or goes on in the
  drone. When everybody is down, the mission is over and everybody goes back to the ship (the testing area
  stands in for it until there is one) and comes back to life there.
- **The drone hops** (jump, about 0.3 m, not more than about once a second), onto a step or over a cable.
- **Creatures come back.** A creature killed is replaced, after about 75 seconds (`ai.return_seconds`,
  with some chance either way), at a point at least 30 m from everybody and out of their sight, preferring
  45 m, so there are always as many as there have been.
- **Development:** `drone` drops a drone where you stand and plays it (again to go back to yourself);
  `r.fullbright` (a setting in development builds) lights everything flat for looking at shapes;
  `site_room` and `site_stairs` go to a room or a stairwell of a site's building.
- **Fixes.** The frame timings in the F3 overlay are gathered by name in a fixed order, so the list no
  longer jumps about. The lamp clusters test each cell against a lamp's sphere exactly and hold four times
  as many entries, which is what overflowed into black boxes. Eight lamps' shadows are redrawn a frame, not
  four, so a door opening in front of a lamp no longer leaves its shadow flickering behind.

## ADR-099: Structures the level check leaves alone; the site clear of the testing area; a dead player's keys; the drone's hop

- **Pieces built to join are one structure, and the level check says nothing of them.** A building's walls
  reach a hair into its floors and ceilings so no hairline shows where they meet, round a stairwell right
  through them, and the site's rock is blocks run together and sunk into its ground, as its buildings are.
  The check made on loading reported every one of those, over a thousand warnings. A body can now be put in
  an overlap group (PhysicsWorld::SetOverlapGroup; MapBuilder::SetStructure for what a map adds), and two
  bodies of one group are not reported. A building's floors, ceilings, walls and shell are one group; at a
  site, with its ground, rock and pad; a lamp pole with its arm. Everything else -- a crate, a shelf, another
  building -- is still reported if it is inside any of them. A test builds every building of two sites as the
  game does and requires the check to come back empty.
- **The site is 30 m further east** (its corner at x = 100): its ground reaches 45 m past the open ground and
  its rock about 30 m, and at x = 70 the rock stood 4 m into the testing area's field and the ground ran
  under the lab's edge.
- **Dead, the keys that are still yours work.** Every game key was behind "alive", so a player in the drone
  could not hop, switch its lamp or pause, and a dead player could not change whose view they watch. Now,
  dead: jump is the drone's hop, the torch key its lamp, interact the next view (without a drone), and the
  development respawn key brings you back; Escape pauses alive or dead.
- **The drone hops onto things.** Higher (about 0.6 m), still driven through the air for three quarters of a
  second after, and with no grip at all while it is off the ground: driven at a ledge and gripping its face,
  the face held it up -- the hop was braked to a few centimetres and it hung on the wall. A test drives it at
  a 0.4 m ledge: it stops there, and hops up onto it.
- **At a site, creatures mostly arrive indoors**: three in four (`ai.arrive_indoors`) come out somewhere inside
  a building, within 80 m, still out of everybody's sight and at least 30 m from anybody.

## ADR-100: The first mission -- the data, the breaker, the drive and the shuttle

Decided with the user: download the data at a terminal, carry the drive back to the shuttle and extract
with it; sometimes the power is out, sometimes there is no map; the launch leaves anybody not aboard.

- **Planned from the site, like everything else** (Game/Mission/Mission.h): every machine plans the same
  mission from the site's seed. The terminal stands on a bench in a room reachable without the keycard
  and not the nest -- the further from the shuttle, the likelier -- with a download of 35 to 60 seconds.
  Two in five missions have its building's power out; three in ten come without map data.
- **Every building has a breaker panel** on a wall: in its plant room if it has one, or a ground-floor
  room, always reachable without the keycard; planned last, so no building plans any differently for
  it. Each building's lamps are now on circuits of their own (a hundred apart), so one losing its power
  leaves the rest lit. With the power out the terminal's screen is dark and it only clicks; its building
  is lit by its emergency lamps alone until somebody finds the panel -- its lamp red -- and resets it,
  which is heard well beyond the building.
- **The download goes on only while somebody alive is at the terminal** (within 4.5 m, on its floor),
  and the terminal is heard working every seven seconds. When it is done the drive is on the bench in
  front of it: an ordinary item, carried, dropped by whoever dies with it, on no equipment bench.
- **The shuttle stands on the pad** (Game/World/Shuttle.h): legs, a cabin with benches, a ramp down
  towards the site, a lamp in the ceiling and a launch console at the front. Everybody arrives in its
  cabin. Anybody aboard can launch; it leaves twenty seconds later -- pressing again holds it -- with
  whoever is in the cabin, and the drive if somebody aboard has it or it is lying in the cabin. The
  result is shown to everybody (data recovered or not, you aboard or left behind, how many made it) and
  then everybody is back aboard the ship: the testing area until there is one. The drive is gone from
  whoever had it. Going back to the site after that puts it back as it was.
- **The objective is on the screen** while at the site: what to do now, and with map data a bearing to
  the terminal ("140 m north-east, one floor up") -- a stand-in for the site map, which is next.
- **The host runs it all** and sends how it stands (a Mission world event: stage, power, progress,
  launch countdown, result) on every change and twice a second while something counts; a player who
  joins is told. Protocol 22. Using the terminal, a panel or the console is an interaction like any
  other, checked by the host.
- The level check on loading also leaves alone what is built to join the site's structure -- the
  shuttle, standing on the pad, and the pipework run into the buildings -- and the planner no longer puts
  anything against a wall beside a way out of a building.
- Placeholder sounds for all of it, in World.json: the terminal's beep, dead click, working chatter and
  finishing tones, the breaker, the launch alarm and the shuttle leaving.
- Development: `mission` says where the terminal is and how it stands; `mission_goto terminal|breaker|
  shuttle`, and `mission_skip` to finish a download at once.

## ADR-101: The site map

- **M opens the site map** (the `map` action, rebindable; `site_map` in the console), over the game rather
  than pausing it, alive or dead. North is up. With the briefing's map data it shows the open ground, the
  shuttle, every building a floor at a time -- the floor you are on in the building you are in, the
  ground floor of the rest -- as its rooms, corridors and stairwells, where the data is (and on which
  floor, when that is not the one shown), everybody still up, and you, pointing the way you look.
- **Without map data it shows nothing** but that there is none: the site has to be searched.
- Not shown, on purpose: the breaker (nobody knows the power is out until they try the terminal),
  doors, and where anything else is. It is the Company's plan of the site, not a scan of it.
- The objective still gives the bearing to the terminal with map data, and names the key.

## ADR-102: What a site is called, what the intercom says, and deploying from the ship

- **Sites have names** (Game/Mission/SiteNames.h), in the style agreed for title cards: the planet as a
  catalogue lists it ("KEPLER-741 V") and the site as the Company designates it ("POLAR RESEARCH FACILITY
  06, NORTH CRYOSPHERE"), made from the site's seed so every machine calls it the same. The words are in
  Assets/Data/sites.json, which holds only the agreed examples: the lists are the user's to fill.
- **The intercom** says lines from Assets/Data/intercom.json at the mission's moments -- arriving, with or
  without a map, finding the power out, the download starting and done, the launch, and the result, and
  being left behind. Each line is a recording in a sound folder (Assets/Audio/Intercom/...) and its
  subtitle; one of a moment's lines is picked the same on every machine. None are written: they are to be
  written and recorded. Both files are read again when they change.
- **Arriving at a site** puts its name up: the planet, then the site, fading in and out.
- **The deployment console** stands in the testing area (standing in for the ship). The host uses it to open
  the briefing for the next site: its name, the objective, whether there is a site map on file, and Deploy,
  Another site or Not yet. Anybody else is told the host chooses. `briefing [seed]` opens it from the console.

## ADR-103: Cinematics -- data on a timeline, played in the world, between every part of a deployment

Asked for as a system, not as one-off sequences: see Game/Cinematic/Cinematic.h and Assets/Cinematics.

- **A cinematic is data** (Assets/Cinematics/<name>.json): cameras, shots that cut or blend, actors moved by
  keys or along paths, their models' clips (ramps, doors, clamps), sounds, markers for the game to act on,
  title cards typed out and captions, particles, shake, fade, letterbox, and how far the fog is pushed back and
  the dark lifted. Keys have eased curves (linear, step, in, out, in-out, or a Bezier of their own).
- **Written against anchors, not coordinates**: the game supplies where things are for this site -- the pad,
  the crawler's start and parking place, the building and its way in, a clear viewpoint of it, the shuttle's
  rest -- and paths (the crawler's route there and back, found round everything on the site). One file fits
  every generated site, and what is edited by hand in it is never overwritten by what a mission generates: the
  generated part is only the anchors. A camera can ride an actor or keep one in view.
- **Played in the world** (CinematicPlayer): its actors are the real things -- the site's shuttle and the
  mission's crawler are bound by name and moved, and put back where they rest when it ends -- or editor models it
  brings in. Everything it shows is worked out from its time alone, so scrubbing shows what playing would; what
  happens (sounds, markers, particles) happens as time passes it. The game takes the picture, lens and far plane,
  holds the players and the creatures, hides the HUD and everybody's bodies, and at the end hands the picture back
  to the player's own eyes (or fades up from black when it ended in black). The host starts one for everybody.
  Nobody playing can skip or pause one; the controls are a development build's.
- **The vehicles are real** (Game/World/Vehicles.h): the shuttle and a snow crawler are editor models
  (Assets/Models/Vehicles, written from code the first time and edited from then on), solid where they rest, with
  sockets for where people stand, the console, the cabin, and lamps that move with them (headlights, a landing
  light). The crawler is parked at the terminal's building with its ramp down to the door: the team arrives in
  it, and leaves in it -- the launch console and "aboard" are the crawler's now.
- **The deployment, shown**: deploying plays surface_insertion (the shuttle out of the dark and down onto the
  pad, the crawler out along its route, the building revealed with its name typed out, the crawler turning at
  the door and its ramp coming down, and the picture handed back inside it); launching plays surface_extraction
  (the ramp up, away from the building, back to the pad, the shuttle up) and its last marker takes everybody back
  aboard the ship for the debrief; everybody down plays surface_wipe (the empty crawler leaving on its own, the
  shuttle going, "MISSION FAILED / RETURNING ON AUTOPILOT").
- Debugging: cine_debug (time, shot, blend, camera, actors, what is coming and what has happened, anchors, and
  every camera's and actor's path drawn), cine_list/play/restart/skip/seek/pause/resume/speed/stop/reload.
- Also: nests no longer grow round the end of a wall (a root drawn straight to the far side went through it) nor
  more than a hand's breadth into a wall they run into; and the game waits for a navigation rebuild before
  shutting down, which crashed quitting within a moment of arriving somewhere.

Still to come: the cinematic editor's timeline, the ship and its travel and arrival, the station's docking,
and short first-person moments for pulling a drive or throwing a breaker.

## ADR-104: The cinematic editor -- a timeline over the world as it is

How to use it: docs/EDITOR.md, "Cinematic editor". Game/Cinematic/CinematicEditor.h.

- **Inside the running game, not a scene of its own.** A cinematic is written against a site's anchors, so it
  can only be seen as it will be at a site: the editor opens wherever the game is (`cine_edit <name>`), plays
  the cinematic there with the real vehicles, fog and lamps, and edits the very copy that is playing. Every edit
  is shown at once, because the player works everything out from the time alone.
- **The picture in a preview, framed as the game frames it.** The finished picture is drawn, whole and scaled
  down, into the part of the screen the timeline and inspector leave (a rectangle the final post-processing pass
  draws into, PostProcess::Settings::out*), at the screen's own shape; the bars and title card are drawn to
  match. Nothing is cropped or stretched, so a shot framed in the editor is the shot in the game. H hides the
  editor for the full-size picture.
- **Keys measured from anchors, placed by looking.** "Set from the view" and K with the free camera take the free
  camera's place and turn *relative to the camera's anchor at that moment* (the pad, the crawler, the reveal
  point), which is what makes a key placed at one site right at every other. It is exactly the inverse of how the
  sampler places the camera (checked: view from a key, set it from the view, and it is unchanged).
- **One history, of whole files.** Undo keeps the cinematic's text as each change left it, a change being
  finished when nothing is being dragged or typed into; so a drag or a typed number is one undo, and undo can
  never disagree with what is saved. Adding, removing or changing an actor starts it playing again from the
  same moment, since its model has to be put in the world.
- **Saving keeps the file's layout.** The JSON is written in the order it is laid out (ordered, not sorted) with
  every number to four places and whole numbers whole, so a hand-written file saved from the editor differs
  only where it was edited, and hand edits and editor edits can go on side by side.
- **Nothing for players.** It is compiled only into a development build, like every control that plays, pauses,
  scrubs or skips a cinematic. While it is open the players and the world are held, and the markers that act on
  the game (hold, release, go_to_ship) do nothing; the intercom's lines are still heard.
- **Scripted input for testing panels**: ui_move, ui_down, ui_up, ui_click, ui_wheel, ui_key and ui_text drive
  the interface from --exec with nobody at the machine (the real mouse is ignored from the first of them). The
  editor was tested with them: selecting, dragging, undo, the menus, the free camera, copy and paste, the curve
  handles, duplicating, playing, and saving as another name.

Still to come: the ship and its travel and arrival, the station's docking, and short first-person moments for
pulling a drive or throwing a breaker.

## ADR-105: The ship -- where everybody is between deployments

Decided with the user (2026-09-28): a mid-size carrier with a hangar; a briefing room, a gear room, crew quarters
and a mess, and a cockpit with windows; travel is a long burn with a time skip (no jump); the shuttle leaves from
the hangar, dropping out through doors in its floor. Game/World/ShipMap.h.

- **Another place in the one world** (ShipSpec::kOrigin, 1.5 km from everything else), like the testing area, the
  lab and the site: going aboard is being put there. It is where a game starts and where every deployment ends
  (the extraction's and the wipe's go_to_ship). The testing area is still there (`testmap`), for development.
- **Two decks and a hangar as tall as both**, walked between by stairs: gear room (lockers, ammunition, every item on
  the bench as the testing area has them), quarters (bunks, lockers, a table), mess (galley, tables), a gallery
  looking down into the hangar, the briefing room (the deployment console before a screen), the cockpit.
  Nothing comes aboard: creatures do not arrive while everybody is on the ship.
- **The hull is round the rooms**, so the ship seen from outside is the one everybody is standing in: the cockpit's
  windows are windows in it, and the bay under the parked shuttle opens through its belly (the bay doors are a
  vehicle model with clips, like the ramps). Its engines have a glow a cinematic turns up (light `ship_engines`),
  and cinematics can bind its shuttle (`ship_shuttle`) and bay doors (`ship_bay_doors`) and measure from `ship`,
  `hangar`, `cockpit`, `briefing` and `engines`.
- **Space is drawn in the sky**, not built: wherever the eye is near the ship the sky is black with stars and a
  faint band of the galaxy, and -- over a site -- its planet, lit by the sun with ice and cloud on it and its air
  glowing at the edge. Being in the sky it is infinitely far away: nothing clips it, from the cockpit or from a
  shot outside.
- A picture handed back after a cinematic to eyes that are somewhere else entirely -- taken aboard as it ended --
  is now cut to rather than swept across the world to.

Next: the transit burn and arrival, boarding the shuttle in the hangar and dropping out, the return into the hangar
and the debrief.

## ADR-106: Fixes from playing the ship build

- **Thin things laid on a surface cast no shadows** (MapBuilder::SetShadows): a screen a few centimetres off a wall,
  paint on a floor. In a lamp's 16-bit shadow map they are the surface under them, and flickered between lit and
  shadowed as the lamps given shadow maps changed with the view -- what looked like z-fighting.
- **Hiding in a locker looks out of its door** whichever way it is turned. A thing turned by yaw faces (-sin, -cos),
  a look of yaw faces (sin, -cos): the locker gave its turn as the look, which is right only facing north or south.
- **Creatures only where creatures are**: a site, or the creature lab. Never aboard the ship or in the testing area --
  including the top-up that replaces the dead, which had ignored the first rule.
- **A new game is a new game**: it starts aboard, with nothing done, however the last one ended. Only going to a
  place from the menu (`facility`, `lab`...) starts somewhere else.
- **Nothing opens over a cinematic** (pause, map, briefing, inventory), and one starting closes whatever was open.
- **Fallen out of the world** (more than 40 m below any floor): put back where this place is arrived at.
- **Aboard, nobody tires and no torch runs down.**
- The briefing room's screen hung over the doorway to the cockpit: now a screen either side of it, the console before
  the port one. Cinematic shake is a quarter of what it was. The warning roar before an attack is no longer heard.
  A terminal found without power makes restoring it the objective. Nothing is put in front of any building's doors.
  Ramps are four centimetres thick, so their foot is almost flush.
- **The crawler parks well out from the door** (16 m, closer only where another building is in the way), and does not
  turn on the spot: it pulls past, then backs straight in, its ramp to the door (PathFollow::reverse), and every
  corner of its ways is rounded off (SitePlan::Smoothed).

## ADR-107: Aboard the ship, a deployment from start to finish

- **Going out**: the host chooses a site at the briefing console and deploys; the ship's burn is shown from outside
  only (ship_transit) -- it lights its engines and is gone into the distance, the transit card on black, then it comes
  in from the horizon straight at the camera and stops over the planet (the "arrive" marker puts the planet in the
  sky). Everybody then walks to the hangar and boards the shuttle; its controls, at the front of its cabin, launch it
  only once everybody who is up is aboard -- nobody is left on the ship while the rest are down. ship_launch: the ramp
  up, the bay doors open under it, the clamps let go and it drops out of the belly and burns away to the planet; then
  the site's insertion.
- **Coming back** (the user's choice): the extraction ends with everybody aboard; the ones who made it into the
  crawler come back in the shuttle, seen climbing into the bay and docking (ship_docking), and are standing in its
  cabin when it hands back; the dead and the left behind are waiting in the briefing room. A total wipe skips the
  crawler: straight back aboard, and the shuttle is seen coming home empty on autopilot (ship_return_empty). The
  mission's result is shown once the docking is over.
- **The ship's outside is a model** (Assets/Models/Vehicles/carrier.json, written from Vehicles::CarrierModel the
  first time), edited in the model editor like any other. It is drawn round the rooms, and a second copy stands on a
  "stage" far out in space for cinematics to fly -- the rooms cannot fly with it, so flying the one round them would
  leave them floating. Its parts are fitted between one another: two faces in one plane flickered (tested).
- The shuttle is parked nose forward, so it drops out and goes on the way it faces, towards the planet ahead.
- A marker that sends everybody somewhere is carried out after the cinematic's frame, not inside it: the next
  cinematic starting from inside the one playing would pull it out from under itself.
- Dev: ship_goto shuttle|hangar|briefing|gear|cockpit, ship_orbit <seed>.

## ADR-108: The map and the objective tracker are things you carry

Decided with the user: no map shows the whole site, or where the creature or the data is.

- **The map** (item `site_map`, device "map") shows only what is near, swept like a motion tracker: a line goes
  round, casting rays at chest height, and the walls it finds light up and fade after it has passed -- walls on your
  floor only, nothing moving, no objective. 20 m across on a site that came with map data, 12 m without. It works
  anywhere, the ship included, because it reads the level rather than a plan. M takes it out of the bag, or puts it
  back; there is no map without one.
- **The objective tracker** (`objective_tracker`, device "tracker") points at what is to be done next and says how
  far, beeping faster and higher the closer it is (Items/tracker_beep): the terminal; its building's breaker once
  the terminal is found dead; the crawler once the drive is out; aboard, the shuttle when it is waiting, the
  briefing console otherwise.
- The objective no longer gives bearings: finding the way is the tracker's. The whole-site map is gone.
- Both are on every equipment bench, the ship's gear room among them. Items say what they show while held with
  `device` in items.json.

## ADR-109: The nest -- smooth, never finished growing, and gone once dead

- **Smooth**: its skin had shards and slivers that caught the light -- detail finer than the grid it is sampled on
  (knots at 7 per metre, hairline veins), a sheet thinner than a cell at its edges, normals taken over a third of a
  cell, and the result thinned to an eighth of its triangles. Now nothing finer than the grid holds, the sheet is
  never thinner than a cell, vertices are eased towards their neighbours, normals are taken over a cell, and it is
  thinned to a quarter.
- **No largest size**: past its first spread (17 m, as before) it goes on growing, slower, for as long as it lives. As
  it nears the edge of what is planned, a ring further out is planned and its skin rebuilt on a worker; every machine
  extends it at the same distances, so all agree. (Bounded only by what can be drawn: 3000 patches.)
- **Dying**: the heart goes first -- grey and dry, the same grey as the skin, in two and a half seconds -- then the
  death goes out through the rest from it; everything then rots away to nothing, the heart shrinking on the wall and
  the skin sinking into the surfaces behind the death, and when the last of it has gone the nest is gone.

## ADR-110: The map and the tracker are handheld screens (replaces ADR-108's sweep)

Asked for by the user: a normal map rather than a radar, and both devices handheld things with screens that you look
at in your hand -- and that other players can see too.

- **Screens in the world, not on the HUD.** Each player holding a device has a 256x256 texture of their own
  (`device_screen_<id>`), painted on the CPU 15 times a second and put on the device in their hand. It is the same for
  your own and for somebody else's, so looking over a friend's shoulder shows what their screen shows.
- **The map** is a plain plan, north up, centred on whoever holds it: the building cells of the floor they are on
  (the ground floor of the buildings they are not in) with their walls, the crawler and the landing pad, everybody
  else as a dot, and an arrow for the holder. 44 m across with map data, 22 m without; aboard, the rooms of the deck
  they are on, 40 m across. Nowhere else, "NO MAP DATA". Still no creature and no objective on it.
- **The tracker** is a fan like the old motion trackers': the objective is a blip that flashes with each beep, or an
  arrow at the fan's edge when it is behind or to one side, and the distance above. The beep is in your head for your
  own and from where they stand for anybody else's.
- **How it is drawn.** A device's case and screen share the one texture: the case is mapped to a corner painted its
  colour, with alpha nought. `Material::emissiveTextured` makes the glow follow the texture times its alpha, so the
  picture glows and the case does not.
- **Held up to be read.** Items can move the hand itself with `hold_hand` (metres right, up and forward of where
  things are usually carried), set in the bench editor next to Offset and Turn. The devices use it to be held up in
  front, screen turned to the eye.

## ADR-111: Multiplayer aboard the ship

Reported by the user: in a lobby, the other player could not see the ship -- there was nothing.

- **The cause: the wire could not say where the ship is.** Positions were sent as -512 to 512 m, and the ship is a
  kilometre and a half out (its cinematics' stage further still). Every position aboard reached the other machines
  clamped to the edge of that range, in empty space. Now -4096 to 4096 m in 23 bits: the same millimetre, across the
  whole world. Protocol version 25, so an older build is refused rather than misread. The bandwidth bounds in the net
  tests rose by about a tenth to match.
- **Somebody joining is made where they belong.** The host decided where to put newcomers when hosting started --
  before it had gone aboard -- so a newcomer was made on top of the host and shoved them. The game now tells the host
  (`NetHost::SetSpawnFor`): aboard in their own place, in the crawler at the site, or the spawn.
- **Where everybody is, and how the ship stands, is told** (`WorldEventKind::ShipState`): the map, the site the ship is
  over and whether the shuttle is ready -- sent to a newcomer and whenever any of it changes. The rest followed the
  cinematics' markers, which a newcomer has missed and a skipped cinematic never reaches.
- **A cinematic stopped on the host stops everywhere** (a skip went on playing for everybody else).
- Arrivals side by side are 0.7 m apart, not 0.55: a body is 0.64 m across, so neighbours arrived inside one another.
- Checked with a host and a guest on one machine, scripted, each taking pictures: joining aboard; the burn; boarding
  and the drop; the insertion; the extraction with one left behind (the one aboard back in the shuttle, the other in
  the briefing room); and everybody down (the empty shuttle home, both in the briefing room). `sleep <seconds>` in
  --exec waits by the clock, so two machines with different frame rates can be scripted to meet; `deploy`,
  `mission_finish`, `shuttle_launch` and `ship_goto <place> all` drive a deployment without a person at either.

## ADR-112: The model editor knows what it is editing

Reported by the user: the model editor treated everything like a gun, and the ship's model should be editable.

- **A model says what it is**: `"kind"` in its file -- `weapon`, `vehicle` or `prop` -- chosen in the File panel ("What it
  is") and written on save. A model without one goes by its folder (Vehicles, Props, otherwise a weapon).
- **Only a weapon is offered a weapon's tools**: the Hold it and First person panels, someone holding it, the grip,
  carry and magazine socket help, the reload, equip and fire clips and reload templates, the hand tracks and Mirror
  hands, and the barrel-down-+Z import hint. A vehicle or a prop gets parts, sockets by name (a vehicle's explained:
  arrival, cabin_min / cabin_max, console, lamps), clips by name, and the person standing beside it for scale.
- **The tools work at the model's size.** Handles, rings, grab distances, socket markers, the axes and the grid scale
  with the model (one for anything hand-sized, so a rifle is edited exactly as before); the camera's pace is set from
  its size when it is opened (and is in the View panel); snapping and the import size default by kind. The carrier,
  sixty metres long, is as workable as a rifle.

## ADR-113: The journey takes time, the crawler is gone, and a kit for everybody

From the user's playtest:

- **The journey to a site is played, not skipped** (replaces ADR-107's transit): `ship_depart` (the ship pulls away from
  the camera, faster and faster, its engine glow fading, until it is gone), then the burn itself -- everybody has the
  run of the ship for `game.ship_travel_seconds` (90), the dust going past the windows and the planet coming up ahead
  over the second half, "Arriving in m:ss" on the HUD -- then `ship_arrive` (in from the distance, slowing all the way to
  a stop, and the planet below). The host's clock; `ShipState` carries it (protocol 26). No deploying while under way.
- **Smooth**: the ship's and shuttles' flights were keys eased one at a time, each segment starting from a standstill --
  a stop-start at every key, which was the stutter. Now smooth curves through them (monotone cubic, sampled every tenth
  of a second), or motion from a formula.
- **One ship in the picture**: the stage is 2.6 km ahead of the real ship, so a shot of one had the other in its
  background. Whichever the camera is nearer is shown, and the other hidden.
- **Space is black** (it was the day sky's zenith dimmed, which read as grey), the sun's glow tighter in it.
- **The launch's last shot is from the side**: the shuttle heads straight for the planet's middle, and seen from behind
  it flew into it.
- **No crawler** (asked for: it drove through buildings, and on this map the shuttle is enough). The shuttle lands on
  the pad, which is now towards the middle of the site so every building is a walk; the team arrives in its cabin; the
  launch is worked from a console at the front of it; the drive comes back to it; the extraction is its ramp closing
  and it lifting off. Its ramp is now as wide as its hull, so closed it covers the whole back. The crawler's model,
  routes and parking are gone (its engine sound is kept, for another vehicle later).
- **Not where**: the objective no longer says which floor the terminal is on, and the insertion looks over the whole
  site rather than at the terminal's building. The terminal is in any building, each as likely (it was nearly always
  the big one); a creature arriving indoors can arrive in any building, not only near the players.
- **The map turns with you**: the way you face is up, north marked round the edge. No map key (M): items are held.
- **A kit for everybody**: the gear room has two benches, a whole kit at either end of each -- four, one for everybody
  who can be aboard -- restocked each deployment, when the ship is rebuilt for the new site.
- **Voice and the microphone test no longer crackle**: a stream played each sample the moment it arrived, and caught up
  with what was coming every few milliseconds; each catch-up was a gap. Now it keeps 60 ms in hand before playing and
  after running dry, fades out through a gap rather than stopping dead, and the output is kept about 30 ms ahead of the
  device, so a slow moment on a slower machine does not starve it.
- The tracker is quiet in cinematics. Frames over 50 ms are logged ("Long frame"), so a stutter can be found.

## ADR-114: The shuttle flies

Reported: the redocking landed and then closed the doors, the shuttle did not point where it went, and it stopped and
started from place to place without seeming to have any speed.

- **Flown, not keyed.** Every shuttle flight (launch, docking, the empty return, down onto a site and off it) is
  generated (the flight tool in the scratch scripts, kept in the commit message's spirit here): a centripetal
  Catmull-Rom path through waypoints -- which never loops or overshoots between unevenly spaced points -- the distance
  along it a cubic whose ends move at the speeds asked for, so one leg hands its speed to the next; the nose along the
  way it is going, pitch limited, turned with weight (eased twice) and banked into turns; or held at a set attitude to
  rise, hover and land.
- **Docking in the right order**: in under the ship slowing, curving up without stopping through the open bay doors, a
  hover while the doors close beneath it, down onto them, and only then the ramp. **The insertion** comes in over the
  site already facing the way it will stand, so it does not spin in the air.
- **The engines' glow** was a disc a centimetre off each nozzle's end, which fought it for the depth buffer at any
  distance, and glowed blue even cold. Now a slug set into the nozzle and well proud of it, dark until a burn. On the
  stage the cinematic camera's near plane is two metres, not a quarter, which gives distant plates far more depth.

## ADR-115: A boot screen, and the title over a planet

Asked for by the user, after Alien: Isolation's title: a planet in the background, and a boot screen that loads and
looks good and says who made the game.

- **Boot screen** (once, at start; any key skips it after a moment; `boot` shows it again): black, a ring of segments
  turning with a sweep, PROJECT PREDATION and MADE BY JOSEPH SLADE coming up out of the dark in turn, a thin line
  filling across the bottom with what is loading, then fading into the title. Skipped in scripted runs.
- **Title**: the sky alone -- the camera out in open space, nothing within its far plane -- with a planet filling the
  left of the picture and the sun just behind its far edge, so what shows is a thin amber crescent and its air glowing
  (a planet's air can now glow warm: `Environment::planetAirWarm`). The name, spaced out, and "Press any key" over its
  dark side; the menu there after a key. The footer says made by Joseph Slade.

## ADR-116: Orders come in, and are briefed on the screen

Decided with the user: missions come at random times, with a video briefing whose voice-over is procedural -- put
together from recordings so it always matches the site -- and never text-to-speech. Orders are orders: none is refused.

- **Orders**: the host's clock -- 30 s into a new game (`game.first_order_seconds`), and 1 to 3 minutes after getting
  back aboard (`game.order_seconds_min`/`max`) -- only while there is nothing else going on aboard. They come in with a
  chime (World/transmission) and the intercom's "orders" moment; the briefing room's two screens ask for them to be
  played; anybody plays them at the console; once briefed, anybody deploys from it. The state rides with the ship's
  (`ShipState`, protocol 27), so everybody's screens show the same, and a briefing joined part way through is joined
  where it is.
- **The briefing** (Game/Mission/Briefing, Assets/Data/briefing.json): slides -- header, destination, site,
  conditions, map, objective, end -- each with a line said over it: phrases, and the site's own names, numbers and
  weather said word by word from recordings (Briefing/Phrases/..., Briefing/Words/<word>, Briefing/Numbers/<n>). A
  slide lasts as long as what is said in it; a recording not yet made is silent but subtitled and timed as its words
  would be. The left screen runs the slides (a turning planet, the site typed out, gauges for the weather, the
  objective's steps); the right shows the site: its plan when a map came with the order, static when not.
- The old pop-up for choosing a site is gone. `briefing [seed|play]` has orders come in now (and plays them).
- The screens' drawing is shared (Game/World/ScreenCanvas.h): the devices' and the briefing room's.

## ADR-117: Boot cards, and a title of our own

Asked for by the user: the boot as black-and-white cards one after another, crediting jojozagjos, and an original title
where the planet's surface can be seen, without the stars stuttering. (Replaces ADR-115.)

- **Boot**: black and white, each card up out of the black and back into it -- LOADING (walking dots, a hairline
  along the bottom), "made by jojozagjos", BEST PLAYED WITH HEADPHONES, PROJECT PREDATION -- then the black lifts off
  the title. A key moves to the next card; `boot` shows them again; scripted runs skip them.
- **Title**: the scene itself, not a painted backdrop: the carrier in orbit (the stage's, so nothing of anybody's is
  near), the camera drifting slowly round it, the ice planet below with its day side and night coming across it, the
  ship right of the middle and the menu down the left. Near plane two metres out, far three kilometres, for a ship a
  hundred metres off.
- **Stars** were points smaller than a pixel sampled at the pixel's middle: there one frame and gone the next as the
  view moved. Each is now a soft point spread over at least the pixel it falls in (`fwidth`), which holds still.
- **Planets** have ground and cloud at two scales, so seen close they are a surface, not a blur.
- The credit everywhere is jojozagjos.

## ADR-118: Prompts that face the right way, snow, device faces, and space seen from space

From the user's playtest:

- **No prompt at the terminal**: an interactable's focus offset is in its entity's own frame -- FocusPoint turns it --
  but the terminal, the breakers, the deployment console and the lockers turned it again where they set it. Facing any
  way but one, their focus was behind or beside them: behind their own front, the trace to it was blocked, and there
  was no prompt. Now given in the entity's frame. A test stands in front of every terminal and panel of several sites
  and checks each is offered; `interact_report` lists what is near the view and why each is or is not offered.
- **The site in space**: the site is a kilometre and a half from the ship and a cinematic's camera sees for twenty,
  so leaving the ship showed it hanging there. Out in space nothing further than 1.2 km from the eye is drawn but the
  ship's two outsides (`MeshRenderer::farVisible`, `SceneRenderer::SetDrawRegion`).
- **Snow** falls at the site (Game/World/Snowfall): a box of flakes round the eye, each coming round again as it
  leaves it, swaying and drifting with the site's wind, never indoors or in the shuttle's cabin.
- **The map and the tracker** show a screen when nobody holds them -- on the bench, in the inventory -- drawn once at
  start (`SetDeviceFace`): a plan, and a fan. They were plain pale boxes.

## ADR-119: Creatures that get somewhere and let you get away; a drone that climbs; placeholder voices

From the user: never meeting the creature unless the data was in its building, and dying very quickly when you did; the
drone stuck on stairs; placeholder audio for the briefing and the intercom.

- **Stuck**: a creature trying to go somewhere and getting nowhere only ever gave up its door-dodging. Now, three
  seconds of that and it is put back on the walkable surface with a fresh route; six and it gives the destination up
  (`CreatureBrain::OnStuck`) -- roaming, looking into something, searching or stalking, it goes about something else.
  One stood in a doorway for the rest of a game before, and was never met.
- **Pace**: after a creature's blow lands, nobody is struck again for 2.5 s (the blow is held), and the one struck gets
  their breath back to run. Two blows from one creature within twelve seconds and it pulls back for eight to fourteen
  (`AskToWithdraw(..., insist)`, scored above anything else while it lasts). It was four blows in three seconds.
- **The drone** rides up a step by itself -- low in front, clear above, up to about a stair's rise -- held from pitching
  over as it does. A ledge knee high is still hopped.
- **Placeholder voices**: a silent placeholder.wav, about as long as what it stands for, in every folder the briefing
  (its phrases, the site's words, the numbers) and the intercom (one line per moment) will play, with a README in each of
  Assets/Audio/Briefing and Assets/Audio/Intercom listing what to record where.

## ADR-120: Particles, and better snow

Asked for by the user: better snow, sparks and particle effects.

- **Particles** (Game/World/Particles): a pool of six hundred small boxes, each thrown with a speed and a spread, pulled
  down, slowed, landing and bouncing on the ground found under its burst (one trace a burst, not one a particle a
  frame), changing colour and cooling its glow over its life; sparks drawn stretched along the way they go. Looks:
  sparks and chips where a round hits something hard (darker bits off something alive), a burning flare spitting, an
  engine's exhaust, snow sprayed up. The cinematics' own puffs are gone for it.
- **Snow**: flakes of different sizes, flat, turning over as they fall; the wind gusting; and spindrift -- a share of it
  streaming low along the ground, faster than what falls.
- Unused code removed: HeldDevice, ObjectiveTarget, Particles::Live.

## ADR-121: The loadout locker; interactions sent in five bits; the console out of the screens' way

Asked for by the user: a loadout screen instead of every item sitting out on the benches, with it made clear where kit
comes from; and the deployment console moved from in front of the briefing screens.

- **The loadout locker** stands straight ahead of the gear room's door: a tall locker with a screen reading LOADOUT, and
  a hazard line on the floor before it. Using it opens a screen on your own machine, empty each time, listing everything issued (with
  what it is for, and a weapon's magazine and spares), a count of each up to the most one person may have, and the six
  slots filling as you choose: there is not room for everything, so something is left.
  Drawing a kit hands back everything you had from the locker and gives you the new one, weapons loaded; what was found
  (the keycard, the drive) is kept and takes its slot.
- **What is issued** is data: `loadout` and `blurb` per item in items.json. The benches aboard are
  empty; bench_count still lays out the test map's and the creature lab's.
- **Multiplayer**: the locker is nobody else's business, so a client draws its kit itself and tells the host
  (MessageType::Loadout); the host checks they are at the locker, holds the kit to the limits, and sets its count of
  what they carry to it, so drops and uses are checked against what was really drawn. Protocol 28.
- **Fixed**: an interaction was sent to the host in three bits, which holds eight kinds; the ninth and later arrived as
  others. A client using the deployment console asked the host to open a door, and one at the shuttle's controls asked
  to pick something up. Now five bits, with a compile-time check.
- **The deployment console** stands in the briefing room's forward port corner, facing the room, rather than in front
  of the port screen.

## ADR-122: Deaths off-site, drone placement, the nest's look, and a batch of fixes

From the user's list.

- **Dying** on a site is as before: for good, and a drone. Anywhere else (aboard, the creature lab) you are back on your
  feet at the spawn in three seconds, with no drone and no "everybody down"; the lab no longer sends everybody aboard.
- **The drone** arrives near where you fell, outside: the nearest open ground two and a half metres or more from the
  body that can be driven to from it, rather than at the insertion point.
- **The drone on stairs**: it took itself to be airborne whenever the middle of it was over a stair's edge (one ray,
  straight down), and never drove again; now any corner will do, or resting upright. And looking for room above a step
  a whole tread ahead saw the next riser, so it never climbed a flight at all.
- **Creatures arriving** only where there is a way to the players: the walkable surface has unreachable pockets.
- **The world's console commands from a client** (spawn_creature, nest_here, nest_grow, nest_hurt, grab_me,
  creature_clear, creature_climb): sent to the host and run there for them, where they stand (MessageType::Command,
  developer builds only, only those commands). Protocol 29.
- **The nest**: the flesh is shaded smooth from its own surface and its colour broadened, where the thinned mesh had
  shaded and painted it in shards; the egg sacs are smooth shapes of their own -- wet, dark amber, something curled
  showing through -- gripped by a lip of flesh; strands are whole tubes. The sacs, the strands and the flesh on walls
  and ceilings are held still when the nest beats (a negative height in the vertex data); the floor's flesh pulses.
  The eggs do not hatch (the user's call: decoration).
- **Also**: stamina (12 s of sprint, back in 8); the head shown until a cinematic has handed the camera back; the torch
  held by the player in third person, not by the camera behind them; the hangar bay's fore and aft linings removed,
  which fought the hull's belly; the title camera on an evened-out clock; a Default button on each setting and each key.

## ADR-123: Surface texture sets, the lobby's rules, one creature, and a batch of fixes

From the user's list.

- **Surface texture sets (PBR)**: a material can carry a normal map and a roughness map beside its base colour and a
  `surfaceScale`, the metres one copy covers. With a scale the shader lays all three on from every side at once
  (triplanar, normals blended "whiteout"), by world position, so the level's boxes need no coordinates of their own.
  Sets are folders, `Assets/Textures/<set>/<set>_BaseColor|_Normal|_Roughness.(png|jpg)`, OpenGL-style normals, read by
  `Surfaces::Apply` with their mips made on load and sampled anisotropically. Missing maps fall back to white and a flat
  normal (texture index one). The snow ground is the first: `snow_02`, a copy every 2.5 m. 2K for anything that
  tiles across a level, 1K for small props.
- **The lobby's rules**: the host sets them in the lobby, everybody else reads them there; sent to each player on
  joining and to everybody on a change (WorldEventKind::Rules). Only friendly fire for now (the user's call: trip
  length, battery, voice copying and creature count are not lobby settings). Protocol 30.
- **One creature a site**, and no more arriving once it is dead (`ai.return` 0).
- **The creature torn between players**: having left one player for another, it holds on to the new one for four
  seconds unless somebody else is much better (0.35 in score), where it had gone back and forth every few frames.
- **The nest** grown by the host's `nest_grow` is sent to everybody (WorldEventKind::NestAged). A nest only rots away
  once the nest itself is dead, not when its creature is (the user's call).
- **Cinematics**: no prompts, no firing and no using items while one holds the players; everybody is heard by everybody
  while one plays, wherever they stand; the shake is a jolt of the camera's place rather than a turn of it, which had
  swung the sky; a player's weapon is hidden with their body.
- **In transit**: no progress bar on the screens and no "arriving in" countdown; the screens' clock is the session's
  shared tick, so every player's reads the same.
- **Also**: no prompt over a menu; items dropped are kept this side of walls (a ray from the eye); the name of the item
  shown for a moment when the hotbar moves to it; a new lobby code shown when a client takes over as host; the held item
  set along the view, not the forearm, which had turned it while the body caught up; less sway aiming down sights;
  lights flicker in short stutters every few seconds, not constantly; lamps keep their shadows to 70 m past their reach.

## ADR-124: The open exploration rework begins: a universe from a seed, and campaigns saved by the host

The user's design for reworking the game round open exploration is [REWORK_DESIGN.md](REWORK_DESIGN.md), with their
first two answers: the crew is an independent, CIRRA-contracted exploration and recovery crew with a ship of its own,
and CIRRA's missions stay as optional contracts. This is its first step, the part everything after it stands on.

- **The universe** (Game/Campaign/Universe): star systems in the cells of a thin galactic disc, each made the first time
  it is asked for from the universe's seed and where it is (SplitMix64, the same on every machine), and kept only while
  looked at. A system is a star and its planets outwards, about half again as far out each, with moons; how warm each
  is comes from its star's light and its distance (and its air), and that picks its biome. Everything a planet can be
  is data (Assets/Data/universe.json): biomes, atmospheres, terrains, weathers, civilizations, specials, stars and the
  kinds of landing region -- a planet is one of each, picked by weight from what suits it, so variety is combinations
  and a new kind of world is an entry. Each solid body has landing regions with their own seeds, designations in the
  agreed style ("SURVEY SITE 05, GLACIAL SHELF") and places on the planet; some are charted, the rest are to be
  found. Home always has a settled world with a shipyard on it, where a campaign starts. Systems are named from the
  catalogue list ("KEPLER-217"), planets by numeral, moons by letter ("KEPLER-217 VI b").
- **A campaign** (Game/Campaign/Campaign): only what is not made from the seed -- credits, components, upgrades, the
  ship's colours, where the ship is and where it is heading, what is known of each body, regions found, what has been
  changed in each region, the log, story flags, cargo, and the campaign's clock (which the planets turn by). Its JSON
  form is the save. Keys a newer game wrote are kept and written back; it carries a version for bringing old saves up.
- **Saving** (Game/Campaign/CampaignStore): a folder a campaign under the player's own data (Campaigns/<name>), with
  a hand save and an autosave; loading takes the newer one that reads. Each is written whole to a spare name and put
  in place, the one before kept as a backup, on a worker thread, so a crash mid-save loses nothing and the game does
  not stall on the disk. Autosaved on getting back aboard and on leaving; saved by hand from the pause menu (the host).
- **Hosting** chooses a campaign: the host page lists them (newest first, with time played and when saved; deleting
  asks twice) or begins a new one with a name and an optional seed. Single player is hosting with nobody else.
- **Multiplayer**: the host's copy is the campaign. It is sent whole (MessageType::Document: JSON in parts of a
  thousand bytes, put back together in order) to anybody arriving and to everybody a moment after it changes, and
  every ten seconds for the clock. Clients ask things of it with MessageType::Request (an action, two numbers, a
  short text), for the host to check and do; nothing handles one yet. A client that takes over as host keeps the
  campaign as it last had it, saved as a campaign of its own. Protocol 31.
- Not yet: the ship, the map, travel, landing regions and the rest are still the old game; they come next, each on
  this.

## ADR-125: The system map, and the ship flown between the planets

- **The map** opens at the briefing room's console (the navigation console, in a campaign) on each player's own screen:
  the system drawn not to scale but true to direction (distance from the star by its square root, bodies larger than
  life, moons well clear of their planets), with the star, every body's orbit, rings, the ship and its course, lit by
  the star and turning on their axes. Drag to turn, wheel to zoom, click to pick out, double-click to go to it. The
  picked-out body's panel shows only what is known: anything can be seen to be a gas giant; records say what it is and
  who is there; the ship's scan gives its air, warmth and weather; a close scan or a visit its terrain and the rest.
- **Planets look the same everywhere**: one surface (Shaders/planet/planet_surface.sh -- ground mixed from two colours,
  seas to the planet's share, ice from the poles, cloud, gas giants' bands and storms) drawn by the map's
  PlanetRenderer and by the sky for the planet out of the windows. The sky turns a planet's poles to its edge as seen,
  so one below the ship is not all ice cap.
- **Travel** (Game/Campaign/Travel): the ship accelerates half the way and slows the rest, steering for where the
  planet is now; so a trip twice as far takes about half as long again, a new course part way is just steered for,
  and calling the course off brings it to rest between the planets. Drive tiers push harder (2.5 times each). No fuel.
  The host flies it; everybody else is told ten times a second (MessageType::Travel, with the campaign's clock and what
  each player is pointing at) and flies it the same way between. Setting a course from orbit plays the departure;
  arriving plays the arrival, and in orbit the ship scans the body and finds 1 + sensor tier uncharted landing
  regions; one is chosen to go down to (the map can change it) and the shuttle is ready. While under way the sensors
  scan whatever passes close (wider with a better tier). Finds go in the log.
- **Pointing things out**: what each player has under their pointer on the map is shown to everybody else, a ring in
  their colour and their name, and marked in the list.
- Title cards and conditions in a campaign are the body's and the region's, not the old seeded names.
- A service location (the shipyard) is not gone down to in the shuttle; docking comes with the shipyard.

## ADR-126: No host migration; briefings only when there is one

- **The host leaving ends the game** for everybody else, and the title says so. The campaign is saved on the host's
  machine and nowhere else, so handing the game to somebody else made a second copy of the campaign that drifted from
  the first (the user's call). Gone with it: the successor election, the addresses on the roster (PeerEntry keeps
  names only) and net.migration_seconds. Protocol 32.
- **Briefings play only when there is one**: orders no longer come in on their own (the timer and its three settings
  are gone). The briefing machinery -- the screens, the procedural voice-over from the recordings -- stays for CIRRA's
  contracts, and the orders command still issues one for testing. In a campaign the briefing room's console is the
  navigation console, and its screens show where the ship is going, where it is, or that nothing is chosen.

## ADR-127: The crew's own ship -- small, one deck, its outside made from its look

The old ship was a mid-size CIRRA carrier of two decks and a hangar as tall as both. In the rework the crew operate a
ship of their own that starts small and grows (REWORK_DESIGN.md), and keeps a shuttle (the user's call). Rebuilt:

- **One deck, about 41 m, along the spine** (bow at -z): the cockpit (the helm under the windscreen, two seats, windows
  ahead and to either side); the ops room (the navigation table -- the console the system map opens at -- the two
  screens on its forward wall either side of the door, a galley counter, a bench and table); the crew section (a
  corridor between the bunks to port and the gear room to starboard: the loadout locker straight ahead through its
  door, a bench, lockers and ammunition); the shuttle bay (twice as tall, the shuttle on the bay doors in its floor,
  room down its sides to walk round to the ramp); and the engine room aft (the reactor and the machinery). Ceilings
  2.7 m, the bay 5.6 m. Spawning is in the ops room, facing the screens.
- **Its outside is made from a look** (ShipHullLook: primary, secondary and accent colours, drive tier, sensor tier) by
  ShipMap::HullModel, and made again when the look changes: the campaign's colours, its drive (a main drive and small
  ones; from the second tier nacelles on pylons off the bay's sides, larger with each) and its sensors (a mast; a dish
  from the first tier; arrays on booms from the third). Bevelled sections drawn along the spine -- the cockpit
  narrower and lower ahead of the body, the bay wider and taller, open under its doors -- with an accent belt and
  stripes, plates, ribs, lit portholes, navigation lights. The old carrier model (and Assets/Models/Vehicles/
  carrier.json) is gone.
- **Growth by sections along the spine** (a cargo hold, a lab, more quarters between the crew section and the bay) is
  what the size upgrades are to add, with the shipyard; the layout is built for it.
- Cinematics: the launch, docking and empty-return shots from inside the old hangar are moved into the new bay (its
  aft corner, looking forward over the ramp). The title's shot is reframed for the smaller ship.
- The map device aboard shows the one deck ("THE SHIP"); the briefing screens hang where the ship says.

## ADR-128: The intercom for the new loop, and the docs brought up to date

- **Four new intercom moments** for travel: course_set, course_changed, course_stopped, and region_found (the ship's
  scan has found somewhere new to land). Each machine says them from what it sees change in the campaign -- under way
  or not, the destination, the regions found -- so everybody hears them without anything more sent. Folders with
  placeholders to record over; the subtitles are the user's to write (the suggestions in RECORDING_AND_TEXTURES.md
  are only there to show what each moment is for).
- **The voice doc rewritten for the ship being the crew's own**: nothing gives orders or sends people to a briefing
  room; the orders recording waits for CIRRA's contracts; the briefing's pieces (all still placeholders) wait for the
  contract script, with the words the universe's names now need listed. The textures doc lists a ground set per kind of
  world and the ship's sets (neutral, to be tinted by the campaign's colours).
- **Docs updated**: README (what the game is now, and how far it has got), ROADMAP (the rework's steps: built and next),
  DESIGN_PLAN (marked as superseded in its structure by REWORK_DESIGN), HOSTING and BUILDING (hosting a campaign),
  NETWORKING (protocol 32, the campaign's messages, no migration), ARCHITECTURE (the game's modules), and the
  TUTORIAL_CHECKLIST (the map, travel and saving are there to be taught).

## ADR-129: The navigation map at three scales, other systems, starting docked, and space that makes sense

- **One map, three scales** (Game/PredationGameMapView.cpp): the galaxy (systems as stars, as far as anybody scrolls),
  a system (its star, planets, moons and stations), and a body (its globe, with the landing areas found on it). Zooming
  in past the nearest goes into whatever is under the mouse or in the middle; out past the furthest goes back up a
  scale; a trail along the top goes back up too. The galaxy's systems near the camera are worked out again only when it
  has moved some way, and far ones are drawn as dots, because ImGui has only so many vertices a frame.
- **Controls**: left-click picks, double-click (or Enter) opens, right-drag slides the map along under the mouse (along
  the flat of the system or the galaxy's disc), left- or middle-drag turns it, the wheel zooms towards the mouse,
  WASD moves, Q and E turn, F focuses what is picked, H goes back to the ship, Backspace goes up a scale, Esc closes.
- **Panels**: a list down the left (nearby systems and a search by name and the ones been to; a system's bodies, moons
  and stations under their planets; a body's areas with day or night and local time), details down the right with
  what can be done (open, set course, set course and dock, go down here), how the ship stands along the top.
- **Landing areas chosen on the globe**: each found area is a marker at its latitude and longitude; day and night come
  round as the body turns; "go down here" sets the course and the area in one, or just the area when already there.
- **Other systems** (Travel::SetSystemCourse): a crossing takes 75 s and more with distance, less with a better drive;
  set out from wherever the ship is, changeable part way, arriving at rest at the edge of the new system on the side it
  came from. Any drive can cross; whether the first drive should be limited is still open. The travel message carries
  the system (protocol 33).
- **A campaign starts docked at the station** over the settled home world. A station is a body (not landable) round its
  planet; docking at one is arriving there. The ship shows a station beside it when docked (ShipMap::StationModel: hub,
  ring, spokes, solar wings, an arm clamped to the ship's roof), off to starboard so the cinematics, which look past
  the ship from port, frame the ship against it, and still there through the cinematic of leaving it.
- **Courses go round the star** (Travel::StarClearance, AimPoint): a straight way that would pass too close is
  steered round by a point beside the star until it is clear; the map draws the way the ship will actually go
  (Travel::Preview, the flight run ahead).
- **Space out of the windows**: every other body where it really is and as big as it really is from there -- a disc
  when near enough to have a size, otherwise a steady point of light, brighter than the stars, as bright as its size,
  its light and its distance make it, and lit to its phase; up to twelve. In orbit the ship goes round the body below
  every sixteen minutes, so the sun rises over its edge and sets behind it, and in its shadow the sun's light on the
  ship goes. At a station, the world below is the station's planet, not the station drawn as a planet beside it (which
  was the "two planets next to each other"). Between the stars, the star left behind is a dimming sun astern and the
  one ahead a brightening point, the nearer lighting the ship.
- **Moons are never more than half their planet's size**, so a planet and its moon are not a matched pair.
- **The ship's screens** say where the ship is going and when it arrives, how far through a crossing it is, and
  docked; the objective says where the navigation table is.

## ADR-130: Starting landed at a hub, not a station; planets with relief; a sun that makes sense; the map steadier

- **No station** (it superseded ADR-129's): the settled home world has a **hub** -- a charted landing area of the kind
  "hub" (universe.json, `"ship": true`) -- where the ship itself sets down rather than sending the shuttle. A campaign
  begins with the ship landed there (CampaignState::landed, saved, and sent in the travel message: protocol 34), at
  the hub's morning (the clock starts where the sun is up and rising there, StarSystem::SunHeight). The shipyard's
  work -- selling, upgrades -- is to be done there, and the tutorial may end there. What the hub is in the story is the
  user's to say (asked); for now its buildings say nothing about whose it is.
- **Landed, the ship stands on a pad** (ShipMap::FieldModel): the world's ground in its biome's colours, patches and
  low hills to the haze, the pad with its lamps, hangars, a tower, tanks, masts and low buildings. Its **landing gear**
  is part of the hull (gear_ parts), down while landed and as the cinematics put it. The sky over it is the world's own
  (SetGroundSky): the star's height and bearing from the place as the world turns, reddening low, night not black;
  a world without air keeps a black sky with stars. The haze never lets the edge of the ground show.
- **Two cinematics**: ship_takeoff (up off the pad, gear up, away over the hub into the haze) plays instead of
  ship_depart when leaving from a hub; ship_land (in over the hub, slowing, gear down, onto the pad) when the ship
  sets down at one -- after ship_arrive if it has just arrived. Both are on the stage with a copy of the field and the
  ground sky. The existing cinematics are unchanged.
- **Crossing between the stars needs an upgraded drive** (Travel::kCrossingTier = 1); the map says so.
- **System names** from several real star catalogues (KEPLER, GLIESE, HD, HIP, LHS, TOI, WOLF, ROSS, LTT, TYC) and
  numbers to 9999, so they are not all alike and rarely the same twice.
- **Planets**: the shared surface noise is gradient noise with each octave turned against the last (value noise
  showed its squares), warped coastlines, more octaves, rounded mountain ridges, and relief lighting from the lie of the
  land, in the sky and on the map. The planet below the ship has its **rings** in the sky, shadowed by it, and the
  planet's pole is tipped a little towards the eye so they are not seen edge on.
- **The sun**: its disc as big as the star really is from there (Environment::sunDisc), darker and redder at its rim,
  a glare round it and four faint spikes instead of eight hard rays. Under way the bow is on the way the ship is going
  (round the star when that is the way), so the destination is ahead and the sun holds its place in the sky.
- **The map**: the picture and everything drawn over it come from the same camera each frame (the shaking was the
  overlay a frame ahead of the picture); going between scales dives into (or pulls out of) the thing and fades through
  dark rather than cutting; left-drag turns more gently; panel text wraps; a list's IDs no longer collide.
- **The title and menus**: no letter-spacing anywhere; the Continue line says when the ship is landed.
- The objective no longer gives how far through a crossing it is.

## ADR-131: Kestrel Station, walking off the ship, plotting a course and setting out, and the galaxy opened by upgrades

- **Kestrel Station** (Game/World/KestrelStation): the CIRRA frontier logistics and contract station on the home world,
  from the user's direction -- where the ship is based when the campaign begins, and where the tutorial will end. Built in
  the ship's frame round its pad: Hangar Row (the ship's open bay, Bay 02, and Shipworks beside it), a street through the
  bay's gate with Operations Hall (ribbed concrete, silo towers, the CIRRA mark and the station's name over its
  entrance), Crew Services, the Research Annex (modules added onto modules) and the Navigation Relay's mast, Salvage
  Intake on the apron with its docks, containers and gantry crane, a fence round it all. Grounded industrial in the
  spirit of the user's reference, brighter: weathered concrete, bolted-on steel, conduits, gantries, sodium lamps on the
  street and floodlights in the bay. Solid to walk on and into; its dressing only to be seen; built only while the ship
  stands at a hub. Modular: a building is a block and its dressing, added without redesigning the rest. No interiors yet.
- **Signs** are drawn on the CPU (ScreenCanvas) into textures: lit panels, painted lettering, and the CIRRA mark drawn
  from rectangles and turned quads after the user's logo (the letters, the orange bar low in the A). "KESTREL STATION"
  only at Kestrel; other worlds' hubs have the same layout without its name (the home hub's area is named from
  universe.json, names.homeHub).
- **Off the ship**: a boarding door to port in the ops room -- the hull's skin and the wall cut round it, lined, the
  wall's screen, the hull's belt and a porthole moved out of its way -- with a hatch that slides into the wall. Opened and
  shut at a lit control either side of it (InteractionKind::Airlock, CampaignAction::Door, the state the campaign's,
  doorOpen); it opens only while the ship stands on the ground, shuts itself when the ship leaves, and is solid only when
  all the way shut. The station's stair meets it. A campaign begins outside, the door open, on the bay's floor at the
  stair's foot, looking at the ship.
- **The landing gear** is hidden as a set of parts on the prop (VehicleProp::SetPartsHidden), so a cinematic moving the
  ship never shows it again in flight.
- **Plot, then set out**: choosing somewhere on the map plots a course (CampaignState::plan, saved); nothing moves until
  someone sets out on it at the helm in the cockpit (a new interaction) or from the map's strip under its top bar, which
  also clears it. Going down where the ship already is, or landing at a hub the ship is over, stays immediate.
- **Leaving without a cinematic**: out of orbit the ship simply goes -- engines lit, a rumble, the dust starting slowly
  past the windows and streaming as it speeds up -- and the departure cinematic waits for the drive that makes trips all
  but instant (Travel::kInstantTier). Taking off from a hub and arriving keep theirs.
- **Arriving smoothly**: the bow is on the destination the whole way, so it is dead ahead; over the last of the approach
  the view turns into the orbit's (the body below, ahead) and grows to its size from orbit, and the orbit's slow turn is
  counted from arriving -- so nothing snaps. The ship is in the planet's shadow whenever the sun is behind it.
- **The galaxy opened by upgrades**: the charts reach Travel::ChartRange(sensor tier) round every system been to (and
  the ship); systems beyond are not drawn, listed or found by search. A crossing reaches Travel::CrossingRange(drive
  tier) -- none on the first drive, fourteen light years on the next, ten more each tier after. Both are drawn on the
  galaxy map. Protocol 35 (the campaign's new actions).
- **The map**: clicking picks reliably (the pointer kept through the press); a click on nothing keeps what was picked;
  small bodies are easier to hit; tooltips say what is under the pointer; the ship is drawn on the globe -- landed at its
  area, or going round on its orbit; zooming out at the limit no longer drags the map, zooming towards the pointer only
  going in and never by more than the view is across (it could throw the view off into empty space and strand it).
- **Stars that hold still**: each pixel looks for stars in the eight cells nearest it, the stars' points fixed in their
  cells (a star near its cell's edge was cut off, and the sky seemed to shake as the view moved); a cinematic's shake moves
  the camera without turning it.
- **Orbits that make sense**: moons start well outside their planet's rings and go round slower the further out they are
  (Kepler), so none laps another; the map draws moons outside the rings it draws.
- **The star**: on the map a granulated surface with spots, its rim darker and redder, a tight corona instead of a fog;
  out of the windows a white-hot disc with the colour in the light round it and two faint spikes.

## ADR-132: Kestrel rebuilt and lit, reversed depth, real planets out of the windows, a ship that turns slowly

- **Kestrel rebuilt** (Game/World/KestrelStation): blast walls round the bay with a gate onto the street, Shipworks as
  the bay's starboard wall, floodlight masts, a control booth up a ramp, the street with pavements, lamp posts, pipes on
  trestles; Operations Hall with a lobby under a canopy, the Annex, Crew Services, the Relay, a gatehouse, Salvage
  Intake. Walls only where a wall makes sense. 46 lamps (LevelLights::AddLamp: a light with no fitting of its own, the
  model drawing its head) light it at night, on their own circuit, on only while the ship stands there.
- **Nothing flickers**: paint lies a hand's breadth (kPaint, 5 cm) over what it is on and no two coats meet at one
  height; the pad and the road are poured into the slab (cut out of its squares) rather than laid on it; anything that
  ends against something else ends a little short or inside it. A test (ShipTests, SharedFaces) finds every pair of
  parts across the ship's hull and the station with faces within 5 mm of each other, facing the same way and overlapping,
  that nothing else covers -- what flickers -- and fails on any.
- **Reversed float depth** (Engine/Render/DepthConvention): depth is a 32-bit float written far = 0, near = 1, so the
  precision is spread evenly out to the horizon instead of crowded at the near plane; distant thin layers stopped
  flickering. Depth::Test(), Clear(), Format() and Perspective() are what everything asks for (the renderer, post
  processing, the scene's passes and their culling, the debug lines, the game's views, item icons), so the convention is
  in one place.
- **Stars that hold still while the view moves**: the sky's rays are built from the projection's scale terms and the
  view's turn alone; the inverse projection lost precision in its last row and the stars wobbled as the map panned and
  zoomed.
- **Planets out of the windows as worlds** (Game/PredationGameSpace.cpp): any body more than about a fifth of a degree
  across is drawn as a sphere by the planet renderer in a view of its own between the sky and the scene
  (Renderer::kViewSkyBodies), at a depth scaled to sit in order behind everything aboard; smaller ones stay points of
  light; a body in front of the star eclipses it. In orbit the ship goes round at 1.6 radii (1.35 for a gas giant), once
  in sixteen minutes, its nose pitched down to the ground below. Leaving, the world falls away behind (exponentially,
  from the orbit's distance to the real one) instead of vanishing; arriving, it is drawn in to the orbit's. A moon's trip
  is no longer instant (Travel::ArrivalDistance: arriving is near enough for the body's size, not a fixed distance).
- **The ship turns slowly** (the user asked): onto a new heading at no more than 0.07 radians a second, easing in and
  out, so the view out of the windows swings round rather than snapping.
- **Moons and planets**: airless bodies are cratered (PlanetCraterCells: craters in the eight nearest cells); rings cast
  no shadow on their planet. Daytime under air is sky, not stars: stars show only at night or where the air is thin.
- **Solar days**: the sun's height counts the planet's own orbit (StarSystem::SunOver), and days are 25 to 75 minutes.
- **The helm's screen** in the cockpit: where the ship is going and when it arrives, or that a course is waiting to be
  set out on; the objective's countdown is gone from the corner of the screen.
- **No soft-locks**: the boarding door will not shut on someone in the doorway ("Clear the doorway first"), and the ship
  will not leave the ground with anybody outside. A joiner while the ship is landed starts outside by the stair.
- **Leaning** keeps the eye 14 cm clear of what it leans towards, so nobody sees through a wall.
- **Cinematics**: the torch goes off, held things are hidden and the HUD with them; the stage's ship has a dark core
  inside its hull, glass in the cockpit and its door shut, so no gap in it shows the sky.

## ADR-133: One shared map, outposts, CIRRA's records, places by kind and by world, procedural outposts

- **One map for the crew**: whoever moves the map moves it for everybody. Each change is a MapView message -- who drove
  it, a count, the scale, the system, what is picked, the region, the camera -- sent reliably at most ten times a second
  while it changes; the host passes it on; the newest from each driver wins (the count compared as a 16-bit difference,
  so it wraps). The top bar says who moved it last. Everybody's own pointer is gone, and with it those fields of the
  travel message. Protocol 36.
- **One destination**: the map marks the one place the ship is going (DESTINATION, or COURSE once set out on), in the
  system and galaxy views alike.
- **Outposts**: where a ship sets down to land, refit and trade is an outpost on the map (the user's word, chosen over
  "hub"): "OUTPOST" and a number, Kestrel by its name. The site kind that was called an outpost is now a remote station.
- **The map says what things are**: labels for the outpost, the ship (beside its body, YOUR SHIP and what it is doing),
  the course; a legend for each scale; a region the ship is at says so.
- **CIRRA's records** (the user chose home and nearby): the home system is surveyed whole and every one of its landing
  areas found; systems within fifteen light years (kRecordsReach) have their planets on file but not their places. The
  map says, for anything picked, whether it is surveyed, on file, or not known.
- **Places by kind** (SitePlan::SiteKind): what a landing area is decides what is built there -- a research facility
  (three to five buildings), a remote station (two, a radio mast, tanks), a survey site (a shelter in broken ground), a
  wreck (what is left standing, a trail of debris, the great plates dug in on edge), a signal source (a tall mast with a
  red light). The kind rides in the top three bits of the site's seed, so one number still plans the same place on every
  machine. What waits at each is the story's, and is left for the user to decide.
- **Places by world**: a site takes its world's ground and rock colours, its sky at that hour (fog kept short), and has
  snow only where it is below -8 C and there is air to carry it.
- **Outposts planned from their seeds** (Game/World/Outpost; the user's direction: every outpost but the starting one
  procedural, so it feels like exploring): the pad and the stair are Kestrel's (shared: KestrelStation::Pad, Stair,
  PadDressing, PadMasts), so the ship stands and the crew step off the same way everywhere. A street runs along the
  pad's port side to an operations block straight across from the stair with the outpost's name over its door. Lots
  along the street and round the pad are filled from the seed: blocks (stores, a workshop, crew quarters...), sheds with
  great doors, habitat modules on legs, tank farms in their bunds, container yards (some under a gantry crane), one comms
  mast -- in one of five paint schemes, with blast walls behind the pad or not, a fence or not. The way the ship comes in
  and the cinematics' camera places are kept clear; the tests hold every seed to that, to the crew's spawn and the walk
  to the operations block being clear, and to the same flicker check as Kestrel. Kestrel Station stays built by hand:
  it is where the tutorial and the story begin. Outposts carry no CIRRA mark until the user says whose they are.
- **Lamps for whichever outpost it is**: the ship's map keeps 96 lamps ready round the rooms and as many round the stage
  (Outpost::kMaxLamps), made with the ship before any site's, and re-aims them for the outpost it stands at
  (LevelLights::Retune, which also forgets the old shadows); those not wanted go on a circuit never lit.
- **Signs at outposts** are drawn into a texture kept for each place in the list, as wide as any sign, so travelling from
  outpost to outpost makes no more of them; the quad shows only the part drawn into.
- **outpost_preview <seed>** (developers) shows any outpost round the landed ship without crossing to it.

## ADR-134: No public lobbies

The game is a campaign now, the host's, played among friends, so there is no public list of games (the user's call).
Hosting no longer asks who can join: friends come in with the lobby's code, and a game on the same network is still in
that network's list. Gone with it: the host page's choice and game-name box, the join page's *Public games* tab, the
lobby client's browsing role (LobbyClient::Browse, LobbyListing) and "listed" in what a host tells the server, and the
lobby server's `GET /list`. The deployed server goes on answering `/list` until it is deployed again; nothing asks it.

## ADR-135: Flying reworked; legs that fold and a stair that is the ship's; outposts with owners

- **Flying** (the user: the ship did not turn, the planets moved oddly, the planet vanished leaving and was flown through
  arriving, speed lines in orbit, too bright, frame drops). The orbit is part of the campaign (Travel::OrbitFrame, from
  where and when the ship came into orbit), the same on every machine, and the ship is held to it: the world stays put in
  the windows while its ground goes by. Setting out from orbit, the ship turns and swings round to the side of the world
  facing where it is going (Travel::AlignSeconds, 8 to 22 seconds, less with a better drive), then burns -- its push near a
  world limited to a constant times its distance from it (Travel::Gentleness), so the world falls away over about twenty
  seconds; straight out until well clear, so nothing turns it back into the world. Arriving, it closes on the near side
  of the world at its orbit's height and settles into orbit there (the closing speed falls with what is left), never
  less able to push than the target's own pull (a moon going round fast), and is measured against where the target was
  as the step began. The space view no longer fakes a departure or an arrival: it draws where the ship is.
- **Precision**: the ship's position is a double (a float an astronomical unit out cannot take the last steps to a small
  moon), bodies are placed in double (StarSystem::PositionD; an angle in float made them step), and the space view
  measures from the body near the ship. Protocol 37: the travel message carries the orbit, setting out, and its position
  in double.
- **Speed lines and engines** only while it burns, by its speed against the world it is leaving or coming to.
- **Planets lit as everything else** (the scene's shading gives back albedo over pi; the planets' did not divide, and were
  three times as bright as the hull beside them).
- **The world drawn first, then the sky where nothing is, then the bodies behind the world** (Renderer views reordered;
  the sky drawn at the far end of depth with a test; the bodies' depth far beyond the world's): the sky and a planet are
  worked out only where they show. About 12 ms a frame looking at a planet from the cockpit is now about 5.
- **Landing legs fold** up from hinges under the belly and down again, and the **boarding stair is the ship's**: it swings
  up level and slides in under the floor as the ship leaves, and out and down as it lands ("stair" cinematic markers),
  solid only all the way out. The landing cinematic's cameras moved off Kestrel's rebuilt walls; a test holds every landing
  and takeoff camera to a clear line to the ship at Kestrel and every outpost.
- **Outposts have owners** (the user's direction; universe.json "owners", data so organisations can be added): CIRRA
  (its mark, its standard outpost and building names), independent and industrial (a neutral operator number, functional
  signs, their own mix of buildings), and unknown/abandoned (dark, faded, few lamps, gaps in the fence). Kestrel is
  CIRRA's. The map says who runs each.
