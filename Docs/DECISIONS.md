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

**Status**: accepted, 2026-09-13

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
