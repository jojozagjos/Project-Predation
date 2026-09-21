# Creature AI

**Status**: Milestone 8 prototype built; the full architecture below lands in Milestone 9. What
exists now is listed first, and the rest of this document is the design it is heading towards.

## What exists (Milestone 8)

| Piece | Where |
| --- | --- |
| Navigation mesh, paths, random points, moving along the surface | `Engine/Navigation/NavMesh.h` |
| Traits from a seed | `Game/Creature/CreatureTraits.h` |
| Sounds the creature can hear, and how far each carries | `Game/Creature/Noise.h` |
| Perception, memory, feelings, utility scoring, behaviours | `Game/Creature/CreatureBrain.h` |
| The body: route following, turning, a physics box, a placeholder visual | `Game/Creature/Creature.h` |
| Spawning, senses, noises, strikes, shots, the inspector and overlays | `Game/PredationGameCreatures.cpp` |

- **Traits**: aggression, fear, curiosity, persistence, perception, run and walk speed.
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
- **Behaviours**: Roam, Investigate, Hunt, Attack and Retreat, scored per target as products of named
  considerations with a commitment bonus for the current one.
- **One rule worth knowing**: a noise made by somebody it can see, or somebody who has hurt it, is not
  a question to go and answer. It updates where they are, and closes any open question near them.
  Without it a creature shot in the back walked off to "investigate the gunshot" instead of turning on
  the shooter.
- **Networking**: the host runs every mind; clients are sent each creature's seed and body state and
  draw a copy (ADR-064).

Developer commands: `spawn_creature [seed] [ahead]`, `creature_clear`, `creature_hurt [amount]` and
`ai_brain`, plus the `ai.creatures` and `ai.seed` settings. The AI, Perception and Navigation debug
categories draw the overlays.

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
