# Worlds, facilities, and knowing where to go

Written 2026-10-12, in answer to: *the worlds and facilities feel too close to the same thing, like a retexture; how will
the game tell players where the missions are; what else should improve.* It sits under
[MAJOR_REWORK_PROPOSAL.md](MAJOR_REWORK_PROPOSAL.md) (whose step 2 -- site systems and the objective graph -- most of the
mission part depends on) and next to [NEXT_STEPS.md](NEXT_STEPS.md). Nothing here is lore; where text or names matter,
the rework answers (2026-10-12) say functional text can be generated and story text is yours.

---

## Why everything feels the same

Being honest about the causes, because each one has a different fix:

1. **Every site is the same shape**: a few hundred metres of open floor ringed by a cliff wall. A different texture on the
   floor and the wall does not change that you are always standing in the same bowl.
2. **Every facility comes from one generator with one recipe**: rooms and corridors on a three-metre grid, five room
   kinds, the same doors, lamps and props, in boxes of the same proportions. The exterior is a box per building.
3. **Nothing says what a place is for or what happened there.** A research station, a mining outpost and a relay
   differ by their sign.
4. **Every world sounds and lights alike**: the same silence, the same white light, the same fog.
5. **There is no reason to be at a given place**: no contract, no lead, nothing on the map that says what is there.

## Worlds

### W1. Out of the bowl (the biggest single change)

- A site sits in **open landscape with a real horizon**: the near ground as now (walkable, detailed), and round it a
  **far terrain ring** out to a few kilometres -- low-detail, drawn only, shaped by the same world terrain -- so you see
  ridges, mesas, plains and craters going away, not a wall.
- Where players can go is bounded **by the land itself** (a ravine, a cliff, deep water, a glacier edge) or, on open
  plains, by a soft limit (the suit's comms range, the shuttle's recall), never a ring of rock.
- Cost: the far ring is cheap (a few thousand triangles, shadowed by the far map that now exists).

### W2. Landform archetypes, not six heightmap styles

Each world picks one archetype and a few features from its seed and biome:

| Archetype | What you see |
|---|---|
| Mesa country | flat-topped buttes, talus slopes, dry washes between |
| Canyon network | narrow canyons you walk in, overhangs, a river bed |
| Crater field | overlapping craters, rims you climb, ejecta boulders |
| Ice sheet | flat white with pressure ridges, crevasses, seracs |
| Dune sea | long dunes with slip faces, rock outcrops between |
| Lava plain | black flows with lobes and tubes, glowing cracks on the active ones |
| Karst / pillars | eroded pillars and sinkholes |
| Glacial valley | U-shaped valley, moraines, a frozen lake |
| Highland | rolling hills, scree, a ridge line |
| Crystal fields | outcrops of crystal on bare rock (with SiteScatter's crystal) |
| Shoreline / islands | land and water (needs the water work: flooded interiors come first) |

### W3. A landmark at every landing region

One thing you can see from the pad and navigate by -- an arch, a lone spire, a crashed hulk, a huge crater wall, a
frozen waterfall, a derelict mast -- different per region. It also makes regions memorable on the map.

### W4. Each world's own light, air and sound

- Light from its **star** (colour, size, how bright), its **air** (thick air scatters and warms, thin air leaves a black
  sky at noon), its **moons** at night (some of this is in).
- **Ambience by biome**: wind (by weather), ice creaks, sand hiss, distant thunder, insects and calls where there is
  native life, silence where there is no air. Recorded loops (CC0), as footsteps are now.
- **Wind you can see**: grass, shrubs and trees moving with the weather's wind (a vertex shader sway; cheap).

### W5. Native life (your answer: yes, not everywhere)

Harmless local fauna on some worlds -- small fliers, grazers, scuttlers, fish once there is water -- simple steering, not
the creature mind. It makes the organisms stand out by contrast, and a world with life feels different from a dead one.

### W6. More on the ground

Lava crust and vents, wreckage and old equipment near where people were, tracks and footprints, snow and sand drifting
against things, water and ice surfaces. (Boulders, spires, ice, crystal, basalt, trees, shrubs and grass are in.)

## Facilities

### F1. Facility types, each with its own recipe

A facility is a **type** (what it was for) and a **condition** (what happened) -- the same split as wrecks. Types, each
data-driven with its own exterior massing, room programme, props and lighting:

| Type | Outside | Inside |
|---|---|---|
| Research lab | low modular blocks, a clean-room annex, sample stores | labs, clean rooms with airlocks, specimen stores, offices |
| Mining operation | headframe or open pit, conveyors, ore sheds | processing hall, control room, workshop, crew quarters |
| Comms relay | dish field, a low bunker | equipment racks, generator room, a small crew space |
| Habitat | domes or modules on stilts joined by tubes | quarters, mess, medical bay, hydroponics |
| Refinery / plant | tank farm, pipe racks, flare stack | turbine/generator halls, catwalks, control rooms |
| Cargo depot | warehouses, gantry cranes, container yards | warehouse floor with racking, office mezzanine |
| Observatory | a dome on a ridge | instrument hall, data centre |
| Survey camp | prefab huts, a drill rig, tents | small: a lab hut, a bunk hut |

### F2. Interiors that are not all one grid

- **Room programmes** per type: which rooms it must have and which must be next to which (a clean room behind an
  airlock, a control room overlooking the hall).
- **Shapes**: double-height halls with catwalks and a mezzanine, L-shaped rooms, ramps and spiral stairs, service tunnels
  and maintenance shafts, **vents that look like vents** (grilles, ducting).
- **Dressing by purpose**: a lab's benches, sinks, fume hoods; a plant's turbines and valves; quarters' bunks, lockers and
  a kitchen. Functional signage and labels generated (allowed: functional text).
- **Condition you can read**: power out (emergency lighting), fire (scorch, collapsed ceiling), stripped (empty racks,
  cut cables), abandoned in a hurry (things left on tables), frozen (frost, ice), occupied (nest growth).

### F3. Exteriors that sit in the land

Buildings dug into hillsides, on stilts over gullies, joined by covered walkways and tunnels, a perimeter fence with a
gate, a vehicle bay, solar arrays, antennas, cargo -- and the condition outside as well as in (snow-buried, overgrown,
half collapsed). **How the way in reads**: replace the row of lamps with a lit entrance, signage, a path worn into the
ground, light spilling from a door, a beacon -- one of these per site rather than always lamps.

### F4. Owners

CIRRA, independent, industrial and abandoned each change materials, colours, signage and how kept the place is (built for
outposts; extend to facilities).

## Telling players where to go

The rule: **every place you can land says why you would**, and **every objective is learnt, not given as a waypoint**
unless the contract gives it.

### M1. On the map (the ship)

- **Orbit scan** reveals a world's landing regions with a **category icon** -- structure, wreck, signal, anomaly, survey
  interest -- and a one-line read: *"Crash site, recent. Owner unknown. Possible salvage."* Better sensors add detail,
  threat (with its confidence) and environment (suit rating needed).
- **Nothing of note**: regions with nothing are still landable but say so, so a trip there is a choice.
- **Contracts** (outposts' board, the intercom) mark their place on the map with what is wanted and the reward;
  accepting plots the course.
- **Leads** found on sites (a log entry, a chart, a recorder) add a marker -- *"LEAD: from the relay's log"* -- with the
  note that led there.
- **The ship's log** keeps open contracts and leads as a short list: where, why, what is known.

### M2. On the site

- **A short landing brief** from the contract or scan (recorded voice clips plus on-screen text, as briefings are now).
- **The objective tracker** (the device that exists) points at what the crew knows the place of -- a contract's target,
  a lead's location -- not at what they have not found.
- **Objectives appear as they are learnt** (the objective graph): finding the terminal says what is on it; reading a log
  gives the next step; the HUD keeps a short list.
- **The site tells you**: signage, lit entrances, power state, paths -- diegetic first, HUD second.
- **A ping** (co-op): point at something and the others see it for a few seconds.

### M3. Coming back with something

The debrief: what was recovered, what it is worth, what it unlocked (a lead, research, a contract done), and what was
lost -- so every trip ends with a reason for the next.

## Other improvements worth doing

- **The crew's own look**: a real suit model in place of the box people (proposal section 21), with customization.
- **Audio**: ambience by place, the ship's hum and creaks, the outpost's activity, the site's machinery when powered.
- **Wind and motion** in scatter, smoke from stacks, steam vents, flags -- the world moving.
- **Credits screen** (also needed for the CC-BY footsteps).
- **Settings**: a quality preset that scales foliage, shadows and scatter density.
- **Readability**: objective text and signs legible at a distance; colour-blind safe markers.

## Order

1. **Out of the bowl (W1)** and **landmarks (W3)** -- the largest gain in "every world is different" per hour.
2. **Site systems + objective graph** (proposal step 2), with **scan categories, contracts and leads on the map (M1)**
   and **objectives learnt on site (M2)** -- the "why go there".
3. **Facility types and room programmes (F1, F2)**, wrecks first (decided).
4. **Landform archetypes (W2)**, **light/air/sound (W4)**, **wind in scatter**.
5. **Exteriors in the land (F3)**, **native life (W5)**, the rest of W6.
