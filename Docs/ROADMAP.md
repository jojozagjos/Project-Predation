# What is left

A list of gaps rather than achievements, for deciding what to do next. Updated 2026-09-24, evening.

## Where things stand

- **The player** walks, runs, sprints, crouches, crawls, leans, mantles, falls, gets hurt, dies as a
  ragdoll and spectates; carries two weapons with authored reloads and six items that each do
  something (see [ITEMS.md](ITEMS.md)); and has a torch whose cell runs down.
- **Multiplayer** is host-authoritative over UDP with prediction, lag compensation, host migration, a
  lobby with join codes, LAN discovery, UPnP, proximity voice, and everything in the world replicated
  -- doors, lockers, items and their uses, flares, nests, creatures and what they are doing.
- **The creatures** are bodies grown from seeds -- four plans, six builds, five kinds of head,
  markings and growths, runts to brutes -- never winged, sculpted as bone under muscle and posed
  procedurally, with sight, hearing and memory; temperaments; stalking from cover and creeping up
  behind, ambushes at doors and crawlspace mouths, going round gunfire, searching down vents or
  waiting at them, walking walls and ceilings, grabbing and carrying off, cocooning at the one nest,
  saying back what they heard players say, reading each other, copying the curious way, a director
  pacing them, and learning over a match which tactics work. See [AI.md](AI.md).
- **The world** is the test map and the creature lab, lit lamp by lamp, with ambience that follows
  where you are, a placeholder sound for everything (Assets/Audio), and an in-game model editor.

## Still missing from what exists

- **Melee or shoving.** Nothing to do about something close except shoot it.
- **Nameplates.** You cannot tell who anybody is at a distance.
- **Reconnecting.** A player who drops is gone for the round; migration covers only the host.
- **Real sounds.** Every sound is a synthesised placeholder, meant to be replaced by dropping
  recordings into its folder. Recordings have to be cleared for use (Docs/CREDITS.md); downloading any
  is a decision to make one at a time.
- **Real models for items.** Items are coloured shapes; weapons are authored. Items can move to the
  same authored models once there are any, and their use motions would drive them.
- **Creature bodies by size.** One navigation mesh with crawlspaces marked serves every body; a very
  wide one can still squeeze through a gap it should not. Ceilings are crossed above the floor's route,
  and a wall is walked only as far along as the one it went up.
- **Pack hunting, planned.** Creatures read each other -- one going for somebody tells the rest where
  they are, and the rest go round or wait rather than pile in -- but none plans a drive for another.

## The next phases

In the order the brief puts them, each building on the last. None is started.

### 1. The round

Lobby, everybody ready, then a briefing: the objective, what is known about the site, and sometimes
a map of the generated location -- sometimes not, when there is none to give. An arrival sequence for
each kind of location. The mission, then extraction, an extraction sequence, and back to the lobby.

The mission framework comes first and is built to be extended: an objective is a list of steps, each
a thing to find, reach, use or carry, with the first mission finding the black box or its data and
getting it out. The sample container is the stand-in for what is carried.

### 2. Generated facilities

Sites generated from a seed, like the creatures: non-linear, with loops rather than corridors, several
floors, height to use, outside ground and more than one building. Vents that run through the walls
and ceilings as a network creatures move and hide in. Flooding as real geometry -- water you wade and
swim through, that hides what is under it. Blocked areas and the tasks that open them, restoring power
first; and modifiers on a whole mission, the power out to begin with. The lamps, circuits and doors
already built are what these are made of.

Built so far (ADR-085): the seeded layout -- loops, two or three floors and their stairwells, the
duct network, doors and locked doors with a keycard, lamps with moods, lockers, supplies, clutter, a
way in and a nest room -- built into the world and sent as a seed. Still to come: power and circuits
as a task, flooding, outside ground and more than one building, and the round around it.

### 3. Dead, but not gone

A player who dies comes back as a CIRRA support drone: limited, slow to recharge, able to be knocked
down and disabled by a creature, and rebooting afterwards. What it can do for the living is the
design question -- lighting the way, marking things, opening something -- and it should never be
better than being alive.

### 4. Feel

Interaction animations for everything a hand does in the world -- doors, lockers, crates, cocoons,
panels -- and the handful of cinematic moments the round needs. A handheld map, fuzzy and incomplete,
that shows the generated site as far as it has been seen, and nothing about where anything is.

## Keeping the lore in mind

Docs/Project_Predation_Lore_Reference.md shapes these choices. Nothing here writes lore, and any
design choice the lore influences is asked about first.
