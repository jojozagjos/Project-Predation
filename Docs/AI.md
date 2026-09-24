# Creature AI

**Status**: Milestones 8 and 9 built -- the prototype, then the AI architecture: stalking, searching,
hiding places, curiosity, memory of places, learning over a match, and reproducibility from seed.
What exists now is listed first, and the rest of this document is the design it is heading towards.

## What exists (Milestones 8, 9 and most of 10)

| Piece | Where |
| --- | --- |
| A body from the seed, and what it can do | `Game/Creature/CreatureAnatomy.h` |
| Navigation mesh, paths, random points, moving along the surface | `Engine/Navigation/NavMesh.h` |
| Traits from a seed | `Game/Creature/CreatureTraits.h` |
| Sounds the creature can hear, and how far each carries | `Game/Creature/Noise.h` |
| Perception, memory, feelings, utility scoring, behaviours | `Game/Creature/CreatureBrain.h` |
| The body: route following, turning, a physics box, a placeholder visual | `Game/Creature/Creature.h` |
| Spawning, senses, noises, strikes, shots, the inspector and overlays | `Game/PredationGameCreatures.cpp` |

- **Traits**: aggression, fear, curiosity, persistence, perception, run and walk speed; and from
  Milestone 9, patience, stealth and a preference for loners. New traits are always drawn after the
  old ones, so a seed keeps meaning the same temperament.
- **Sight**: a 130 degree cone to 26 m (scaled by perception), three occlusion rays at head, chest and
  hips, scaled by distance, how tall the target is standing, how fast they are moving and the light
  where they stand (under a roof is dark; a lit torch is bright anywhere). Exposure has to fill before
  a player counts as seen; in the meantime the creature stops and turns to what caught its eye.
- **Hearing**: every noise has a reach; loudness falls off as a square root of distance over reach, and
  through a wall the reach is halved. Footsteps carry 1.5 m prone to 14 m sprinting; a gunshot 70 m.
  Voice is a fixed-reach noise every half second while somebody is transmitting, not yet scaled by
  microphone level.
- **Memory**: a track per player (last known place, confidence that fades over the creature's
  persistence, exposure, how much they have hurt it) and one open question -- the thing it is going to
  look into.
- **Feelings**: pain, fear and arousal.
- **Behaviours**: Roam, Investigate, Hunt, Attack, Retreat, Stalk, PlayDead, Search and Observe, scored per target as
  products of named considerations with a commitment bonus for the current one. A strike already being
  wound up, and a spring from playing dead, are seen through rather than weighed again.
- **Stalking**: a stealthy creature that has been seen does not charge. Hunting carries a "the moment"
  consideration -- 1 for a brazen creature, and for a stealthy one only as good as the opening: somebody
  looking the other way, on their own, or past the end of its patience. Stalking scores the other way
  round. It waits in cover found by a query: points scattered round the target and round itself,
  scored on being out of every known player's line of sight, about eleven metres off, dark, behind
  them, not far to walk, and not reached by walking past their nose. Exposed, it hurries to cover;
  hidden, it creeps, and every few seconds it leans out to a spot with a view to look -- which is how
  it learns they have turned away.
- **Watched, and gaze versus sight**: whether a player is looking its way is read from their gaze, and
  only while it can see them; it is believed for four seconds after, then not known, which counts as
  half an opening. Being hidden from them is not the same as them looking away -- confusing the two made
  it step out of cover straight into their view.
- **Playing dead**: straight after a wound that leaves it badly hurt, with somebody close, a creature
  more cunning (stealth and patience) than timid drops as if dead. It looks exactly like a death -- the
  same fall, eyes going dark, the body falling to whichever side has room -- but its brain runs. It
  springs at anybody who comes within reach, gets up and slips away when nobody is looking, and if shot
  while down gives up the act and runs, and is not believed again. At most twice a life.
- **Dead is dead**: on death the brain stops for good. Nothing afterwards perceives, decides or moves,
  and the intent is cleared -- a creature killed mid-strike used to leave the strike standing.
- **Arrival**: a game starts without the creature. With `ai.creatures` above nought -- it is nought for
  now, and creatures are made with `spawn_creature` -- about forty seconds in (`ai.arrival_seconds`,
  varied a little by seed) one appears somewhere at least twenty metres from everybody and out of
  every player's line of sight.
- **One rule worth knowing**: a noise made by somebody it can see, or somebody who has hurt it, is not
  a question to go and answer. It updates where they are, and closes any open question near them.
  Without it a creature shot in the back walked off to "investigate the gunshot" instead of turning on
  the shooter.
- **Networking**: the host runs every mind; clients are sent each creature's seed and body state and
  draw a copy (ADR-064).

- **Searching**: arriving where somebody was and finding nobody lowers how sure it is, which hands
  over to Search. It plans a round: suspected lockers first, likeliest first, then a few places spread
  round where they were heading, and goes through them, looking round at each. It finishes the round
  whatever its confidence does (that is what searching is for), then gives them up -- until a sound or
  a glimpse.
- **Lockers**: it knows where every locker is (they are furniture), never who is in one until it pulls
  the door. It suspects one from a locker door heard and not seen, from somebody vanishing right
  beside it (0.9 when they vanished at the door), and from seeing it shut -- but a shut door means only
  what it has learnt: next to nothing until the first time it opens a shut one and finds somebody,
  after which a shut locker is a question it goes and asks. Seeing one standing open clears it. Found,
  the person is dragged out by the same path as climbing out, and it goes straight for them.
- **Curiosity (Observe)**: a curious, not very aggressive creature that has not been hurt by somebody
  follows them openly at six to ten metres and watches, backs away if they come close -- facing them,
  not turning its back -- and gets bored after 15 to 60 seconds depending on its curiosity. What it does
  then is whatever else was scoring underneath. Attacking now weighs aggression heavily, so a gentle
  creature that somebody walks up to gives ground rather than lashing out.
- **Memory of places**: the ground in four-metre squares, warmed by seeing or hearing somebody there
  and cooling over a few minutes. Wandering drifts towards the warm squares more often than not, so a
  creature left alone prowls where people go.
- **Learning over a match**: how much a shut locker means somebody behind it, and a hiding habit from
  people found and locker doors heard, which widens how many lockers a search takes in. Forgotten when
  the match ends.
- **Several at once**: up to eight, each with its own mind, all replicated. Each is told where the others
  are and keeps its own length from them, so a group arriving together spreads out instead of standing
  inside one another. Eight together cost about two hundredths of a millisecond a tick. Hunting as a
  pack -- one that has seen you telling the others -- would start from knowing where the others are,
  which each already does; it is not built.

Developer commands: `spawn_creature [seed] [ahead [metres]]`, `creature_clear`, `creature_hurt [amount]`,
`creature_pose`, `creature_mind`, `hide [locker]` and `ai_brain`, plus the `ai.creatures`, `ai.seed` and `ai.arrival_seconds` settings.
The AI, Perception and Navigation debug categories draw the overlays; while stalking, the cover it
weighed is drawn as posts (orange hidden, purple seen, taller for better) with the chosen spot ringed.

### Bodies

A seed makes a body as well as a temperament, from a random stream of its own so no seed's temperament
changed when bodies arrived (`CreatureAnatomy`). There are four kinds, each about as common as the others,
so the next one could be anything:

- a **crawler**: gaunt and shaped too much like a person, on long arms and bent legs, with no tail and
  sometimes a second, smaller pair of arms;
- **four legs** under a long body;
- **six legs** splayed wide, low to the floor;
- **two legs** under a body balanced by a tail.

Each body has a neck, a head and a jaw; none, two, four or six eyes; sound-catching frills; a tail;
armour plates and spines. Every limb stands on the ground, so none can be a wing.

On top of the kind, every seed draws its own details: how far the face juts, how far the jaw hangs open,
how many teeth and how long, how plainly the ribs and spine show, how many fingers, how long the claws
are, how heavy the brow, how swollen the skull. Two creatures of the same kind rarely look alike.

What the body can do is worked out from it (`CreatureCapabilities`) and handed to the brain as it is
made: run and walk speed from leg length and weight; sight from its eyes (none at all without them,
though it still knows somebody touching distance away); hearing from its frills, and best of all when it
has no eyes; reach from its neck and head (and a crawler's arms); how hard it hits from its weight and
jaw; its health from its bulk -- a lot of it, 700 to 4000, so a medium body takes two or three carbine
magazines -- and armour from its plates, which stop up to 30% of every round. Pain is measured against
that health, so a round hurts a big body less than a small one.

It is drawn as **one skinned mesh over a skeleton**, not a kit of parts (`CreatureSkin`):

- Its bones, muscle, ribs, spine, shoulder blades, hip bones, skull and joints are described as shapes
  -- rounded cones and ellipsoids -- blended smoothly into one another, with the eye sockets, nostrils,
  sunken temples and the mouth carved out of them. One surface is drawn over the lot (a signed distance
  field turned into triangles by surface nets), the way clay covers an armature, then simplified with
  meshoptimizer to 7,000 to 16,000 triangles. A knee is where the thigh and the shin are the same
  surface; the neck grows out of the shoulders; the ribs stand out of the chest as far as the seed's
  `ribs` says.
- Every vertex is **painted**: skin, yellowed bone where the skin is thin (skull, knuckles), black in the
  sockets and nostrils, dark red gums, teeth, claws, bruise-coloured blotches, veins, darker undersides
  and dirty hands and feet, and darker in every crease -- between ribs, in the armpits, deep in the
  sockets -- from how much of the body is close round it. Gums and teeth are wetter than skin.
- Teeth, claws, fingers and spines are too fine to sculpt, so they are built as they were and fixed to
  their bone, painted to match. Each eye socket has a dim pinprick of light deep inside it.
- Every vertex is weighted to up to four bones by how near it is to each, so the skin stretches across a
  joint rather than creasing at it. A skin takes 30 to 100 ms to build, on every core, and is kept by
  seed, so a creature made again costs nothing.

Every frame a **procedural rig** poses the skeleton (`CreatureRig`), with nothing authored:

- **Feet are planted in the world.** A foot stays where it was put down while the body moves over it,
  and steps -- lifted, carried and put down again on the ground found by a ray -- only once the body has
  moved far enough from it, and only while the feet it walks in step with are down: diagonal pairs on
  four limbs, two tripods on six, alternating on two. That one rule makes a walk, a run, a turn on the
  spot (stepping round, not spinning on planted feet) and a scramble up stairs.
- The body rides at the height of the ground under its feet and tips to the slope between its front
  and back feet. It bobs and sways with the stride, and its spine bends into a turn.
- The head follows what it is looking at, within what a neck can do, spread over both neck bones and
  the skull, and twitches every few seconds: a quick jerk that settles. The jaw hangs as open as this
  one's does, works as it breathes, and gapes to strike and to call. The chest breathes, faster when it
  is excited. The tail swings on springs and drags on the floor rather than through it.
- Blows are poses of their own: a **swipe** draws one arm up and back and rakes it across; a **bite**
  draws the head back and snaps it forward; a **lunge** gathers, then throws the whole body forward with
  both arms reaching; a **grab** closes both hands on somebody; it **carries** them against its chest; a
  **call** throws the head back; it **shoulders** into a door. A round makes it **flinch** away from the
  hit and settle back.
- In the air, its feet tuck up under it; it pitches nose-up leaving the ground and nose-down landing.

It dies -- or plays dead -- as a **ragdoll** (`CreatureRagdoll`): a Jolt capsule for every bone with
swing-and-twist limits for a spine, a neck, a shoulder and a knee, falling from the pose it was in, the
way it was moving, and pushed by the round that brought it down. Every machine runs its own. Playing
dead is the same fall, and it gets up by easing every bone from where it lies to standing over a second.

Rounds find it through **a capsule on every bone** (the hit zones, on a physics layer that touches
nothing), so where it is hit matters: the head takes 2.2 times the damage, the neck 1.5, the body 1, an
upper limb 0.75, a lower one 0.6, a hand, a foot or the tail 0.45. A box round its torso is what players
bump into. Lying as a ragdoll, its pieces are what rounds find.

Sight passes through creatures, its own body and any other, so two big bodies of a pack standing close
do not blind each other.

To look at one: `ai.freeze 1` stops every creature where it stands, and `spawn_creature <seed> ahead
<metres> <degrees>` puts one in front of you, turned by that many degrees (90 for a side view).

### Temperament

Dangerous is not the same as malicious, and not every creature wants anything to do with a person. Each
seed is one of four temperaments, drawn last from the seed and weighted by the rest of what it is:

| Temperament | Share | What it does about people |
|---|---|---|
| Predator | about 58% | Hunts them. Everything below this table is about predators unless it says otherwise. |
| Territorial | about 20% | Has ground -- sixteen metres round where it first found itself, or round its nest. Somebody on it gets a threat display (it rears up, as for a call, but nothing else comes). Staying on it five seconds after being warned, or coming within five metres, and it fights. It chases a little way off its ground and no further, and walks only its own ground when it wanders. |
| Timid | about 12% | Keeps away: somebody in sight within eighteen metres is somebody to get away from, out of their sight. It only fights at arm's length, and only once it has been hurt by them or has been cornered -- somebody staying within three metres of it for two and a half seconds while it tries to get away. |
| Curious | about 10% | Watches and follows (Observe). |

Whatever the temperament, somebody who has hurt it in the last minute is somebody it means harm to. The
timid ones are the fearful, unaggressive seeds; the curious ones the curious, unaggressive ones.

Only some creatures take people. A grab, and everything after it -- being carried off, the nest, the
cocoon -- is for predators and territorial ones with a nesting trait above a half. Those above two thirds
build a nest to take their catches to. The rest fight and kill, or keep away.

`ai.temperament predator|territorial|timid|curious` makes every new creature that temperament, to try one
out. The brain inspector shows it on a tag at the top, and for each player whether it means them harm.

### Attacks

It has four ways of hurting somebody, each with its own quick timing, and barely a pause between them
for something that means it (the more aggressive, the shorter the blows and the pauses):

| Blow | Lands after | From | Damage |
|---|---|---|---|
| Swipe | 0.25 s | within reach | 0.75 of its strike |
| Bite | 0.31 s | within reach | 1.15, and 1.5 into somebody it holds |
| Lunge | 0.52 s | 2.6 to 7 m, facing them, nothing in the way | 1.05, from further than it could reach |
| Grab | 0.3 s | within reach | none: it has hold of them |

Close in, it rakes and bites in turn. From a few metres off, facing somebody, it throws itself at them.
Somebody on their own -- more than six metres from anybody else -- is who it grabs, the more so the more
it prefers loners -- if it is one of the ones that takes people at all (see Temperament). The game checks every blow again when it lands: somebody who got out of the way is not
hit, and seeing it coming is what the wind-up is for. It can strike up at somebody on something as high
as it can rear (`verticalReach`, from its height).

### Grabbed, dragged and wrapped up

Grabbed, a player is carried along in front of it, facing it, pinned where it has them on every
machine. It takes them away from the others: to its nest when it has one, or else as far from everybody
else as it can get, out of their sight. There it kills them, bite by bite -- or, at its nest, wraps them
in a cocoon against the wall beside its heart and leaves them there, alive, bleeding four health a second,
until they die or somebody cuts them free (use the cocoon).

Getting free:

- **Struggle.** Every fresh press of jump works a hand loose; enough of them, quickly, and they are out.
- **Be shot free.** A creature that takes six percent of its health while it holds somebody lets go of
  them, frightened.
- **Shoot it yourself.** Nothing stops somebody being dragged from firing, and its head is right there.

### Getting at people

- **Jumping and climbing.** The navigation mesh finds every ledge a body could jump across -- from the
  lip of a crate or a platform, out and down to floor within four and a half metres, clear of the level
  all the way -- and bakes them in as links, flagged by height. Each creature routes only through the
  jumps its body can make: those that climb (six legs, or light) get up 2.7 m; the rest as high as
  their legs throw them, between 0.8 and 1.95 m. A drop too high to jump back up is one way. It jumps
  along an arc with its feet tucked.
- **Somebody out of reach** -- up where no jump or climb gets it, and higher than it can rear -- is paced
  under, looked up at, and called about, every few seconds, for twelve seconds; then it goes to wait in
  cover somewhere they will have to come down to.
- **Doors.** A shut door across its route is a wall the navigation mesh runs straight through, so it
  stops at one, pulls it open, and goes on. A locked one it throws itself at until it gives -- two to
  six blows, fewer for a heavier body. Every blow is loud.
- **Calls.** Somebody it cannot reach, it calls about. Every creature within 45 metres hears a call and
  comes at a run.
- **The nest.** Where there is a nest, the creatures come out of it, take what they catch to it, and go
  back to it to heal when they are hurt -- three percent of their health a second. Nowhere else do they
  heal at all. Somebody shooting its heart brings its builder back whatever it was doing, afraid or not,
  and turns it on whoever did it; see The nest, below.
- **Turning.** Looking round is its head, swung either side of the way it was facing when it began, not
  its body. It does not turn towards anything right under its nose, turns on the spot only for more than
  a small turn, and slower than on the run. (It used to swing its look relative to the way it was facing
  at that moment, which chased its own nose round in circles.)

### The nest

A nest is a living thing rooted in a wall, not a heap on the floor. The creature that builds one works at
a spot for fourteen seconds; then a heart the size of a person's chest appears on the nearest broad wall
(the edge of a doorway does not count), at about chest height, lower under a low roof. With no wall near
it hangs from the roof, and in the open with nothing overhead it lies on the ground. Vessels root it
into the wall round about.

From the heart it spreads. Lines are cast out from the heart in every direction, and wherever each first
meets the level -- the wall it is on, the floor, the ceiling, the wall across the room -- is a patch of
growth that comes up in its turn: near surfaces within the first minute, the furthest, seven metres off,
at about five (`ai.nest_growth_seconds`). Every machine casts the same lines at the same level and grows
it on the same clock from the same seed, so nothing about the growth is ever sent: the host says where a
nest was built and how old it is, and a newcomer sees it grown as far as everybody else does.

None of it is solid. The first nests were a mound with a box round it, and the creature that built one
stood inside it while it did, so the box went up round the creature and it never got out. Nothing about
a nest blocks feet or changes the navigation mesh now.

The heart beats -- slowly, quicker with somebody near and quicker again as it is hurt -- and is heard, and
glows faintly red through its skin with each beat, enough to find it by in the dark. It can be shot
(`ai.nest_health`, 200): the round finds it as it finds a creature, and each hit is sent to everybody.
When it bursts the growth withers and darkens, anybody wrapped up at it comes loose, the creature has
no home to heal at or bring anybody back to, and it knows who did it.

`nest_here` builds one where you stand, `nest_grow 120` ages every nest two minutes, and `nest_hurt 50`
shoots the nearest heart.

### The creature lab

`lab` takes everybody in the game to the creature lab and restarts the creatures from its nest; `testmap`
goes back. It is built into the same world as the test map, 170 metres east, and has somewhere to try
everything above (see `Game/World/LabMap.h` for the layout):

- **the crate yard**: blocks 0.5, 1.0, 1.5, 2.0, 2.6 and 3.4 m high, for what can jump onto what, and a
  watchtower up a stair too narrow for anything but a person, which nothing can reach;
- **the crawlspace**: a tunnel 1.3 m high a person can crawl into and no creature follows;
- **the corridor of doors**: an ordinary door, a room of four lockers, and a locked store room;
- **the balcony**: up a ramp, with a 2.4 m drop off it;
- **the pillar forest**: roofed and dark, for being stalked in;
- **the nest chamber**: dark, enclosed, and empty until something builds there.

There is a carbine, a pistol, medical kits and an ammunition crate at the spawn.

### Crawlspaces

The navigation mesh is built for a body crawling -- a metre of headroom -- and floor with less than a
standing body's height over it is marked as crawlspace. Only a body that fits is routed along it: every
small creature, and the lower of the middling ones (`fitsVents`, from its anatomy). One that fits
follows somebody in, flattened to its belly and no faster than a crawl, on every machine.

One that does not fit no longer takes the roof of the tunnel for the person lying in it. A route only
counts as reaching somewhere when it ends at the same height, so the roof is "as near as it gets", and
hunting, searching or stalking somebody it cannot follow scores almost nothing. What it does instead is
wait beside the mouth they are nearest or heading for, out of the line of the tunnel, and take them as
they come out -- a swipe in at arm's length if they come close to the opening, never a lunge into it.
The mouths are found when the mesh is built, where crawlspace floor meets standing floor.

### Walls and ceilings

A creature whose body climbs (six legs, or light) can go up a wall and across a ceiling. It goes to the
foot of the nearest wall that reaches a ceiling between 2.2 and 5 metres up, climbs it facing it, turns
over onto the ceiling, and moves across it over the route it would walk on the floor below -- the
navigation mesh is still what it plans on, and what its brain is told is where it is. Where the ceiling
ends, or gets too high or too low, it lets go. Its whole body is drawn in the frame of what it is on,
legs bent and body flat to it, and its feet find the wall or the ceiling as they find the floor.

It climbs to wait: stalking from cover, or lying in wait, where there is a ceiling, it goes up. Up there
it is seen only by somebody looking up at it, it does not lean out to look -- it can see from where it
is -- and it follows across the ceiling to new cover that has one over it. Going for somebody from up
there is letting go on top of them: it drops, turning over as it falls, and lands beside them. Anything
else it wants to do, it comes down for.

Every machine is told what it is clinging to, and which way the wall faces, in two bits and seven more
for the wall, and eases its body round to match. `creature_climb up|down|auto` sends every creature that
can climb up, or down, or back to its own mind.

### Tactics

Where it hides, waits and comes from are questions about places, answered from the navigation mesh and
two questions the game answers: is a line clear, and how walled in is a point (eight rays at body height,
a stride and a half long). None of it is a list a map provides.

- **Cover is something to be behind.** A place out of every player's sight scores nearly everything, as
  before, but now only as much as it is walled in, and half again when something solid stands between
  it and them within a couple of strides. Out of sight in the middle of an empty floor -- where they
  happen not to be looking from -- was where it kept choosing to hide. With nowhere near them worth
  it, it backs off out of their sight rather than standing in the open.
- **Following.** A stalker picks cover round where they will be in a second and a half, not where they
  are; the patient ones prefer cover ahead of somebody on the move, where they are going.
- **Ambush.** Somebody who has gone out of its sight through a door, with the door between them, is
  waited for on the far side of it, flat against the wall beside the frame -- by the stealthy and
  patient, for fifteen to fifty seconds, until they come through or are heard somewhere else. The
  `from hiding` consideration makes striking out of an ambush or a stalk more likely. One that climbs
  waits up on the ceiling where there is one.
- **Shooting.** Gunfire it cannot put a person to is not walked straight at, unless the creature is
  brazen or close. It goes round: to somewhere six to fourteen metres off the source, off to the side
  of the way straight in, out of sight of it and not reached by passing it. There it stops and
  listens, longer if it is patient, and waits for more shooting to stop. Then it creeps in -- or, if it
  is the patient sort, waits by the nearest way out of there instead. A lot of shooting is a fight,
  which the fearful keep out of.
- **Changing its mind.** Going straight back to what it has only just given up now needs a better
  reason than it had for stopping, fading over eight seconds; and a place where it was frightened --
  somebody it kept away from, or shooting -- is somewhere it does not go and look into for a while.
  A timid one used to go from keeping away from somebody to looking into their footsteps and back,
  every few seconds, for as long as they stood there.

The overlay (`ai_debug`) shows each: the ambush spot in red with a line to what it watches, the flank
point in blue from the shooting, a dim red ring round every place it is keeping clear of, and the mouths
of the crawlspaces in pink.

## Goal

A creature that feels like a living organism reacting to its surroundings, not an NPC running a state
machine. Believable intelligence comes from perception, memory, traits, internal state, environmental
awareness, utility scoring, behavior selection, procedural movement, situational reactions, individual
variation, and adaptation from experience. No symbolic hypothesis reasoning, no planning, no world model.

## Architecture

Utility-scored behaviors with hierarchical execution, fed by event-driven perception and bounded memory.

```
world sound events, visibility queries
        |
   Perception  (vision cone + occlusion + light level + exposure accumulator; hearing thresholds)
        |
   Memory / Blackboard  (per-player tracks, spatial memory, self state)  <--  Traits (static per seed)
        |
   Behavior scoring  (each behavior x each target: product of named considerations)
        |
   Selection  (commitment bonus for the running behavior, cooldowns)
        |
   Behavior execution  (small internal step sequences, tactical point queries)
        |
   Locomotion intent  (path over capability-filtered navmesh, urgency, posture)
        |
   Procedural animation (see ANIMATION.md)
```

### Stimuli

The world emits sound events with position, loudness, type, and source entity: footsteps, running, gunfire,
doors, dropped objects, equipment, impacts, and player voice. Voice becomes a sound event whose loudness comes
from microphone activity. No audio data reaches the AI.

### Perception

Vision: field of view cone, range, occlusion raycasts against several body points, and a light-level estimate
at the target. Exposure accumulates while a player is visible and decays when not, so a glimpse is not a
detection. Hearing: loudness attenuated by distance and occlusion, compared to a trait-driven threshold.
Update rate 10 to 20 Hz.

### Memory

Per-player track: last known position, decaying confidence, estimated velocity, isolation estimate from
observed teammate distances, and small counters such as "hurt me", "used loud weapon", "hid in locker",
"tends to separate". Spatial memory: recent noise positions, interesting objects, a decaying heatmap of
corridors players use, hiding spots seen in use. All bounded, all decaying.

### Traits (static per seed)

Aggression, fear, curiosity, territoriality, patience, persistence, stealth preference, risk tolerance, pain
response, isolation preference, confidence, sociability toward humans, environmental curiosity. Traits shape
the response curves used in scoring and the dynamics of internal state.

### Internal state

A handful of continuous values: pain, fear, arousal, confidence, curiosity. Events push them; traits set how
fast they move and where they settle. A creature shot once may become frightened, enraged, or switch from open
attack to stalking depending on traits and state.

### Behaviors

Stalk, Hunt, Attack, Ambush, Hide, Investigate, Search, Retreat, Observe (curious approach), Roam,
CheckHidingSpot, Reposition, CarryToLair. Each behavior scores itself per candidate target from a list of
named considerations multiplied together (distance, isolation, confidence, pain, light, exposure, memory
recency, trait weights). "Stalk Player 2" and "Stalk Player 3" are separate options. Selection applies a
commitment bonus to the running behavior and cooldowns so decisions do not flicker. Decision rate 5 to 10 Hz.

### Tactical point queries

Candidate points sampled from the navmesh around a target and scored by criteria: not visible to any player,
in darkness, behind the target, near cover, elevated if the anatomy allows, inside a vent if the body fits.
Stalking, ambushing, and hiding emerge from these queries instead of scripts.

### Locomotion

Behaviors request movement with urgency and posture. Detour paths over the creature's size-class navmesh with
capability-filtered off-mesh links (vent, climb, ceiling, squeeze) produce velocity and pose intents.

### Adaptation

An experience layer nudges priors slowly and imperfectly: locker-check likelihood rises with observed locker
use, corridor watch weights follow the heatmap, per-player wariness follows damage dealt. Everything decays.

### Isolation awareness

Derived from observed teammate distances and time since a player was seen near another player. Some
creatures weight isolated targets heavily and will shadow a group waiting for a split.

### Capture

Attacks resolve to outcomes rather than a single damage event: injury, knockdown, grab, drag, restraint,
capture, carry to lair. See the interaction rig in ANIMATION.md.

## Seeds and reproducibility

`spawn_creature <seed>` recreates the same initial organism: morphology, proportions, capabilities,
temperament, sensory parameters, behavioral preferences. Experience during a mission changes behavior, so the
same seed does not guarantee identical behavior forever. Seeded generation is covered by unit tests.

## Debugging

Every decision tick writes a snapshot into a ring buffer: scored options with per-consideration breakdown,
current behavior and goal, traits, internal state, memories, target, last known positions, perception events.
A timeline logs transitions with reasons. The brain inspector renders the latest snapshot; the host can stream
snapshots to clients when debugging is enabled. Visual overlays cover vision cone, hearing events, detection
strength, paths, target, last known positions, hiding and ambush candidates, vent routes, climbable surfaces,
and reachable areas.

Example of the option list the inspector shows:

```
Stalk Player 2       0.81
Hide                 0.67
Investigate Noise    0.41
Attack Player 1      0.25
Retreat              0.12
```
