# What comes next

A plan for the open-exploration campaign from here, written 2026-10-06. It sits beside [REWORK_DESIGN.md](REWORK_DESIGN.md)
(the design this all follows), [ROADMAP.md](ROADMAP.md) (what is built) and the textures to get, in
[RECORDING_AND_TEXTURES.md](RECORDING_AND_TEXTURES.md). Nothing here writes lore: where a choice touches the story, it is a
question for you (marked **Your call**), and the game is built so the answer can be dropped in as data later.

---

## A. Decided, being built

### 1. Wrecks (your answers, 2026-10-06)

- **Type and condition are separate.** A site's *kind* (wreck, signal source, survey site, remote station, facility) and
  its *condition* are two systems; any kind can turn up in several conditions. Both come from the site's seed, so every
  player sees the same place.
- **A wreck's conditions:** recent crash, old wreck, picked over, occupied, environmental damage, mysterious/unexplained.
  An occupied wreck is more dangerous but **not** automatically richer.
- **Owners, as outposts have them:** CIRRA, independent, industrial, unknown. The owner decides the wreck's look,
  signage, cargo, logs and salvage.
- **The flight recorder** always writes a factual entry to the ship's log (where, when, what condition, whose ship), and
  has slots for authored recordings you can fill in later, by owner and condition.
- **Salvage** goes into the ship's hold with a credit value. Some items are tagged as useful for upgrades, contracts or
  research rather than just for selling.
- **Exploring:** the wreck is a broken hull you can walk into -- its rooms laid out as the facility generator lays out a
  building, dressed as a ship -- with cargo, components and the recorder inside, and the story of what happened told by
  what is lying about (scorching, cut panels, drifts, nests), by condition.

### 2. Arrival cinematics, short

You see the ship actually fly in; then a brief reveal of where it has arrived (a few seconds), then straight back to
play, aboard, in orbit -- nothing jumps.

### 3. The ground is not flat

Built (ADR-136):

- **Terrain from the seed and the world:** a height field for every site, shaped by the world's terrain kind (flat,
  rolling, mountainous, canyons, cratered, dunes), closed in by steep rock instead of a ring of blocks.
- **Buildings and pads on levelled ground**, cut into the slope (the cut's face drawn as rock), with the ways between
  them levelled too.

Next:
- **Things on the ground, by world:** boulder fields, rock spires, ice formations, crystal growths, dunes, lava crust,
  dead or living vegetation for temperate and jungle worlds, wreckage and old equipment near people.
- **Ground textures by slope:** the world's ground on the flat, rock showing through on the steep.
- **Weather that is the world's:** snow is in; rain, dust, ash and fog next, with wind.
- The same for outposts: each on its own piece of the world's ground, not a flat plain.

---

## B. Needs your decisions

### 4. What there is to find on planets

What I suggest (game systems, no story in them):

| Find | What it is | What it gives |
|---|---|---|
| Salvage | cargo, components, equipment, in wrecks, stations, camps | credits; some tagged for upgrades, contracts, research |
| Data | terminals, recorders, black boxes, survey logs | log entries; contract objectives; sometimes a lead (a new place on the map) |
| Samples | geology, atmosphere, biology, found by scanning | research; survey contracts; the log |
| Signals | beacons, transmissions, unexplained signals | a trail to follow to one of several outcomes |
| Anomalies | odd natural things -- formations, fields, phenomena | research; the log; story threads |
| Leads | coordinates, charts, references in logs | new places and systems on the map |
| Story fragments | pieces of the larger mystery | the main story (see 7) |

Places to find them (from the design's list): wrecks, signal sources, survey sites, remote stations, facilities,
abandoned camps, crashed transports, cave mouths, communication towers, research sites, hidden structures, natural
anomalies. Not every place is a fight; some are only strange, beautiful or useful.

**Your call:** which kinds of place beyond the five that exist; which (if any) are story places; and whether creatures
can be anywhere or only in some kinds of place and conditions.

### 5. Progression

What I suggest: credits from salvage and contracts buy upgrades at outposts (drive, sensors, hold, ship size, research,
utility modules, cosmetics); some upgrade tiers also need a tagged component found out there, so exploring -- not just
earning -- moves the ship on; the drive and sensors open the galaxy (as they already do on the map); research turns
samples and data into knowledge (better scans, the log filling in, leads).

**Your call:** how long a campaign to the end of the main story should take; whether every outpost owner trades and
refits (does an abandoned outpost let you refit? an industrial one sell only some upgrades?); whether credits are lost on
a crew wipe (the design says some).

### 6. Missions and contracts

What I suggest: contracts as data templates -- recover something from somewhere, survey a place, investigate a signal,
answer a distress call, a research request, salvage a wreck -- offered at outposts and over the intercom, briefed at the
navigation table (the video briefing already exists), optional, paying credits, components or leads. The current
"download the data" objective becomes one template of many.

**Your call:** which contract types first; who offers them (CIRRA only, or other owners too, and how their offers
differ); the voice and wording of briefings (lore: I would leave slots for recorded lines, as the intercom has).

### 7. The story and the ending

The design already sets the shape: the larger story emerges slowly through exploring; discoveries that seem unrelated
start to point at something CIRRA and others know of but never reached; the player's crew pieces it together and gets
there first; the destination looks unlike anything else in the game and is explorable; then "main story complete", free
play continues.

What I can build without deciding any of it:

- **Story threads as data:** fragments that can be placed in places, logs and recordings; the log gathering them; rules
  for when enough of a thread is known to reveal the next step (a lead, a new system, a coordinate).
- **The way there:** a final destination hidden on the map until its thread is complete, reached by a drive or route
  that the story unlocks.
- **The endgame's own rendering:** room for its own shaders, sky and light, as the design asks.

**Your call (lore):** what the final destination is (the design's placeholder: something alive on an enormous scale,
not final); what the threads are and where they start; how the tutorial incident ties in; how much CIRRA knows; how many
pieces it takes. I will not decide any of these. If it helps, I can draft a *structure* (how many threads, how pieces are
found, how the reveal happens) with blanks for you to fill.

### 8. The creature AI

The AI today is large (AI.md): temperaments, tactics, nests, crawlspaces, learning, a director, and more.

**Your call:** what isn't working for you? For example:

- it feels predictable, unfair, or dumb at particular moments (which?);
- it was built for the old one-facility missions and should change for open exploration -- creatures belonging to some
  places and conditions (an occupied wreck, a nest in a cave), roaming open ground, following the crew back to the shuttle;
- different creatures for different worlds (lore: what kinds);
- it costs too much frame time with several about.

Tell me what you want it to feel like and I will plan the rework against that.

### 9. The loadout

Today every player takes the same fixed kit from the loadout locker.

What I suggest for the campaign: the ship has an armoury stocked from the hold -- what you buy at outposts and bring back
from sites -- and each player takes what is there, limited by what they can carry; what is lost on a site is lost; the
basic kit is always there so nobody goes down empty-handed.

**Your call:** is that the direction? What should always be free, and what should be scarce?

---

## C. Suggested order

1. Terrain that is not flat (built), then things on the ground by world, weather by world
2. Wrecks, salvage and the hold (decided) -- with short arrival cinematics
3. Contracts, the economy at outposts, and the loadout
4. Signal sources
5. Survey sites
6. The story machinery (threads, leads, the final destination hidden on the map)
7. The AI rework, once you have said what it should feel like
8. Remote stations
9. The endgame
