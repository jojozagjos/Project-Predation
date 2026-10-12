# Major rework: design and technical proposal

Written 2026-10-11 in answer to `PROJECT_PREDATION_MAJOR_REWORK_PROMPT.md` (the prompt), after going through the code and
the docs. It does not restate the prompt. For each part it says what is there now, what to keep, what to change, what is
new, and where the prompt is asking for something that costs more than it gives -- with a cheaper way to get most of it.

Nothing here writes lore. Where a choice touches the story it is in [section 34](#34-decisions-that-need-you), as a
question.

Sizes are relative, for planning: **S** a day or two, **M** about a week, **L** a few weeks, **XL** more than a month.

---

## The short version

1. **Most of the foundations are already here, and better than the prompt assumes.** The creature mind already has
   perception, memory, temperaments, stalking, ambush, luring with mimicked voices, playing dead, searching, a brood that
   reads each other, learning, a director and per-seed quirks (AI.md). Bodies are already one sculpted, skinned
   mesh per seed with a procedural rig. The facility generator already makes looping multi-floor buildings with ducts,
   locked doors and a reachability check, and lamps already sit on power circuits. The save is already "seed plus
   changes". The rework is mostly **re-organising and extending**, not replacing.
2. **Two things genuinely need replacing:** the mission (one hard-coded template: terminal, download, carry, launch) and
   the player's body (boxes on a skeleton -- no suit can be built on it).
3. **The one piece of new architecture everything else hangs on is "site systems + objective graph + validator"**:
   power, access, pressure/water and information modelled as small graphs inside each site, objectives as steps over
   them, and a solver that proves a generated mission can be finished before anyone sees it. Wrecks, stations, codes,
   repairs, contracts and underwater objectives all sit on top of it. It should be built first.
4. **Underwater is the most expensive thing in the prompt by a distance** -- water rendering, swimming, buoyancy, 3D
   navigation, aquatic bodies and gaits, flooded-site generation, underwater audio. I recommend it in three steps,
   cheapest and scariest first: **flooded interiors**, then **shore and shallow water**, then **open ocean and the huge
   creatures**. Section 11.
5. **Zero gravity, free EVA and manual ship docking are each a new game genre.** I recommend none of them for now
   (stations keep their gravity; docking is a short console sequence, not flying). Section 13 -- your call.
6. **Intelligence must be visible to count.** A creature that is clever where nobody can see it reads as a random one.
   Every clever behaviour should have a tell -- a sound, a pause, a posture, a line in the ship's log. Section 8.

---

## 1. Current systems that can remain

| System | Where | Why it stays |
|---|---|---|
| Creature mind: utility-scored behaviours, per-player tracks, exposure-based sight, hearing, memory of places, lockers, curiosity, stalking, ambush, luring with mimicked voice, playing dead, feeding, nests, capture, climbing walls and ceilings, crawlspaces, doors, calls | `Game/Creature/CreatureBrain.*` (5,900 lines) | It already is the "intelligence from behaviour, not stats" the prompt asks for. |
| Brood learning (`TacticLearner`), per-creature learning, quirks | `Game/Creature/` | Already per-encounter adaptation, bounded and decaying. |
| Director (pressure, give them room, nudge an idle creature) | `PredationGameCreatures.cpp` | The right shape; it needs more inputs (section 9). |
| Body from seed: anatomy, capabilities from the body, SDF skin with surface nets, vertex painting, bone weights | `CreatureAnatomy`, `CreatureSkin` | Sculpting a body as one surface is exactly what makes a coherent organism rather than a kit. |
| Procedural rig: planted feet, gait groups, spine bend, head tracking, strikes, ragdoll | `CreatureRig`, `CreatureRagdoll` | Procedural animation is the only kind that works on procedural bodies. |
| Universe from seed: systems, bodies, biomes, atmospheres, weather, owners, regions | `Game/Campaign/Universe.*`, `universe.json` | Data-driven already; new POI kinds and hazards are new rows. |
| Travel, orbits, the shared map, map holder | `Travel.*`, `PredationGameMap*.cpp` | Done and working. |
| Site terrain, textures, lighting, sky | `SiteTerrain`, `SiteMap`, shaders | Recent, and the base for surface variety. |
| Facility generator | `FacilityLayout` | Grid of rooms/corridors/stairs/ducts with locked doors and `Reachable()` -- the start of the solver. |
| Save as seed + deltas, `extra` kept for newer saves, version number | `CampaignState` | Already what the prompt's section 35 describes. |
| Host-authoritative networking, seed-only replication of generated things | `Game/Net`, `Engine/Net` | Everything new follows the same rule: send seeds and state changes, never meshes. |
| Cinematics system, ship, shuttle, outposts, Kestrel | `Game/Cinematic`, `ShipMap`, `Outpost` | Stays. |
| Inventory (slots, no weight), items as mechanics, weapons | `Game/Items`, `Game/Weapons` | Stays; gains campaign ownership (armoury). |

## 2. Systems needing modification

| System | Change | Size |
|---|---|---|
| Traits | Add a **cognition profile** (section 8) alongside temperament; regroup the behaviours it unlocks. | M |
| Brood | From "everyone reads everyone in sight" to **information that travels** (sight, calls, nest) and **roles** (section 6). | M |
| Director | Track pressure **per player**, hand out **attack tokens**, scale for player count, never spawn. | M |
| Creature spawning | Replace the arrival timer (`ai.arrival_seconds`) with **placement from site state** (section 8 of the prompt; our section 9). | M |
| Anatomy | From four fixed kinds to a **body-plan grammar** (section 5). | L |
| Rig | New gaits: undulating, swimming, slithering, heavy lumbering; weight cues. | L |
| NavMesh | One mesh per **size class** (today one, for a 0.35 m-wide body); plus a volume graph for water. | M |
| FacilityLayout | Styles (station, wreck hull, flooded plant), **zones** (pressure, water, power), and objective slots. | L |
| Loadout | From "same kit for everyone" to an **armoury** of campaign-owned gear. | M |
| CampaignState | New keys for POI state, solved codes, repaired systems, research, suit, armoury (section 27). | S per system |
| Mission briefing | From one template to briefings assembled from the contract and its objective graph (still recorded clips, never TTS). | M |
| Ship | Sections added along the spine as upgrades (armoury, lab, workshop, med bay...). | L |

## 3. Systems needing replacement

- **The mission** (`Game/Mission/Mission.*`, `PredationGameMission.cpp`): a fixed sequence of stages. Replaced by the
  objective graph (section 16). The data download becomes one template in it, so nothing is lost.
- **The player's body** (`PlayerBody`): boxes on a skeleton. Replaced by a sculpted suit (section 21). The skeleton,
  animation and IK stay.
- **Timed creature arrival.** Removed, as you asked on 2026-10-06 and again in the prompt.

## 4. New architecture required

```
Universe (seed)  ->  Region  ->  POI = kind + condition + owner + hazards + ecology    (all from the seed)
                                   |
                         Site build: terrain, structures, interiors (FacilityLayout + styles)
                                   |
                         Site systems: power graph, access graph, zones (pressure / water / heat),
                                       information (logs, codes, clues), salvage
                                   |
                  Contract (optional) + objective graph  -->  Validator (solver)  --> reject and regenerate
                                   |
                         Ecology: which creatures, where, why (or none)
                                   |
                  Runtime: host owns state; deltas to clients; deltas to the save
```

Four new modules, each plain data plus rules, testable without the engine (as `FacilityLayout` and `MissionPlan` are now):

1. **`SiteSystems`**: the graphs inside a site. Power: sources (generator, battery, mains), relays, breakers, loads
   (lamps, doors, pumps, terminals, life support). Access: doors with how they open (free, powered, keycard, code,
   manual crank, cut through). Zones: rooms grouped into pressure/water/temperature zones joined by doors and vents.
   Information: things that carry a fact (a terminal, a note, an ID badge, a stencil on a pipe).
2. **`Objectives`**: a graph of steps, each with a goal ("zone 3 pressurised", "item X on the shuttle", "code for door 7
   known") and prerequisites. Steps are things players can see and do, not scripts.
3. **`MissionValidator`**: plays the site from the entrance with an abstract crew -- what is reachable, what can be
   powered, what is known -- until every required step is done or it is stuck. Stuck means regenerate that part.
   This is how the prompt's section 36 list is enforced, all of it, mechanically.
4. **`Ecology`**: from world, biome, POI condition, threat and seed: none, one, or a few creatures, each with a reason
   (a nest under the floor, a roamer, an occupant of the wreck), placed before anyone arrives.

## 5. Procedural creature generation

**Now:** four kinds (crawler, four legs, six legs, two legs and tail), then per-seed details. Every limb is a leg.

**Proposal: a body-plan grammar,** in this order of decisions, each constraining the next:

1. **Medium and locomotion** (from the site's ecology): walker, climber, burrower/crawlspace, swimmer, amphibious.
   This comes first so that anatomy follows behaviour, which is the prompt's main point.
2. **Mass class**: small, medium, large, huge. Sets bone thickness, gait (light skitter to slow plant-and-shift), how
   doors and ducts treat it, and its nav size class.
3. **Body axis**: segments along a spine (3 to 12), how it tapers, where the mass sits, whether the head is distinct.
4. **Appendage pairs per segment**: legs, arms, grasping limbs, fins, paddles, or none -- chosen from what the locomotion
   needs, then a few "spare" ones from the seed. A swimmer gets a flattened tail and fins *because* it swims; a walker
   that happens to be near water does not.
5. **Sensors and feeding**: eyes (count, size, placement), pits, frills, whiskers/feelers; jaw type (snap, crush,
   filter, proboscis), grasping structures.
6. **Covering**: skin, plates, quills, ridges, translucent patches; scars and wounds from the seed; age.
7. **Adaptation layer** from the world (section 2 below).

Everything still becomes one SDF surface, so new parts blend in rather than being bolted on. Capabilities are still
read from the body (speed from legs, sight from eyes), so a new body plan automatically behaves differently.

**Keep reproducibility**: new draws come after old ones in the random stream, as now; old seeds keep their old bodies
until we choose to bump the creature version.

**Challenge:** "full control of every listed feature" multiplies into combinations nobody can test. Group the seed's
choices under a handful of **archetypes per locomotion** (for walkers: stalker, brute, skitterer, sentinel), with the
grammar varying everything inside them. Most creatures then look coherent, and the debug lab can force any archetype.

## 6. Creature visuals and materials

- **Now:** vertex-painted skin, bone, gums, claws, creases from cavity; an "organic" flag in the mesh shader.
- **Add:** a skin shader that layers **tileable organic detail** (a few CC0 skin/scale/chitin sets, triplanar in the
  body's own space, as the hull now is), **subsurface-ish wrap lighting** and a **wet sheen** where the seed says. Colour
  and pattern from the seed and adaptation (countershading, stripes, blotches, bioluminescent spots).
- **The shared signature** (section 2 of the prompt): one or two subtle features every body carries in some form --
  for example a particular vein pattern, a structure behind the jaw, a colour under the skin seen only when wounded.
  *What* it is, is lore: section 34.
- **Damage that stays**: wounds and scars painted into the vertex colours where it was hit, kept for the encounter.
- **LOD**: the skin is 7k to 16k triangles; a huge creature needs a second, simplified surface (meshoptimizer is
  already in) and the far one swapped in.

## 7. Creature animation

- Keep the procedural rig. Add **gaits** per locomotion: lateral undulation (swimming and slithering), sculling with
  fins, a heavy walk where the body visibly loads each leg, a climbing reach-and-pull.
- **Weight**: a big body's steps lag, its spine sags between supports, its head moves last; anything huge moves slowly
  and turns wide. All rig parameters, scaled from mass.
- **Thought and intent** -- the cheapest high-value work in this whole list: hesitation (a pause with the head
  cocked), attention (head and then body turning), sniffing at something, testing a door with a limb before
  shouldering it, flinching from light, looking back at a dead one. Each is a short procedural pose, like the strikes
  now, triggered by the mind. These are the *tells* of section 8.

## 8. Creature brain and intelligence

**Now:** traits (aggression, fear, curiosity, persistence, perception, patience, stealth, preference for loners,
nesting), four temperaments, twelve quirks, learning.

**Proposal: a cognition profile of six axes** -- not eighteen. The prompt's eighteen traits collapse into these, and each
axis *gates or scales* behaviours that exist or are added:

| Axis | What it covers (from the prompt's list) | It unlocks or scales |
|---|---|---|
| Memory | memory duration, hiding-spot learning, recognising habits | how long tracks last; lockers/hiding learned; the habit model below |
| Foresight | predicting movement, ambush planning, cutting off routes | following to where they will be; ambush at doors; waiting at exits |
| Craft | deception, luring, mimicry, playing dead | Lure, mimic, PlayDead, fake retreats |
| Understanding | doors, routes, light, weapons, dangerous equipment, environmental manipulation | opening/forcing doors, using ducts, learning a tool is dangerous, putting out lights, triggering a noise to draw players |
| Curiosity | curiosity, experimentation | Observe, copying, examining dropped objects, testing a door |
| Sociality | cooperation, isolating players | roles in a group (section 6 of the prompt), information sharing |

Plus **patience, persistence and risk tolerance** as the temperament-side dials they already nearly are.

- An animalistic creature is low on all six; a frightening one is high on Foresight, Craft and Understanding but may be
  physically weak -- exactly the prompt's "high threat, weak body".
- **Habit model** (new, small): per player, counts of "goes left at junctions", "uses the torch a lot", "hides in
  lockers", "always reloads after a fight", "splits from the group". High-Memory creatures plan against the habits.
- **Learning that tools are dangerous** extends what lights already do: lit then shot twice and lights frighten it. The
  same for the welder, a mine, a flare, a gun type.
- **No cheating** stays the rule: nothing reads the players' state except through its senses. The director knows, and
  never tells (as now).
- **Tells (essential).** The habit model and foresight are invisible unless shown: it pauses at the junction they always
  take; it puts out the light they rely on *where they can see the light go*; it waits at the exit they used last time.
  The ship's log can record what the crew observed ("it avoided the welder after the second time") -- functional, not
  lore.

## 9. Threat level

- **A site's threat is a sum of named contributions**, each data-driven: creatures (each scored by cognition, body,
  numbers, abilities), environment (heat, toxins, pressure, depth, radiation), structure (instability, flooding),
  remoteness (how far from the shuttle; comms), and unknowns.
- **Shown with confidence**: what the ship's sensors and records support. "THREAT: HIGH -- LOW CONFIDENCE" when the
  sensors are poor or the site is unvisited. Better sensors narrow it. Being there and seeing narrows it more.
- Harder places shift the odds towards higher cognition and stranger bodies -- not towards more health and damage. The
  creature's health stays a function of its body, as now.

## 10. Aquatic creature generation

- Through the same grammar: medium = water, locomotion = swimmer. Undulating spine, fins/paddles, streamlined head, eyes
  placed for depth (large, or none and big pits), lateral sensing.
- **Huge creatures are not bosses**: their brains are a simpler **ecology agent** (migrate, feed, circle, investigate a
  light, ignore) on top of steering, with the full mind kept for the few that interact closely. Cheap to run at a
  distance; awe comes from scale, sound and slowness, not AI depth.
- Rendering a 60 m body: low-poly far LOD, a few hundred bones at most (fewer: segments drive groups), and fog doing
  most of the work.

## 11. Underwater player mechanics

**Challenge first.** This is the costliest feature in the prompt. Everything below must exist before one underwater
mission works: water surface rendering, underwater fog and light, a swim controller, buoyancy for bodies and items,
water volumes in physics, aquatic navigation, aquatic gaits, flooded-site generation, muffled audio and comms. I
estimate it at XL even phased. Phase it:

1. **Flooded interiors (L).** Rooms in a facility, station or wreck with a water level: wading, then swimming in a
   flooded corridor. The layout is already a 3D grid of cells, so a flooded room is a set of cells with a level, and
   swimming creatures path over cells, not a navmesh. Highest horror value per hour of work in the whole prompt.
2. **Shore and shallow water (L).** A water plane over site terrain (lakes, rivers, shorelines), visibility falling off
   underwater, swimming at the surface and diving a little. Needs terrain that knows its water level.
3. **Open ocean, depth and huge creatures (XL).** Ocean worlds' sites as platforms and rigs over deep water; dives,
   trenches, submerged structures; sonar; the big ecology agents.

**Movement** (all phases): swim speed with inertia, a gentle buoyancy towards neutral, ascend/descend on keys, slow turns
for heavy gear, currents as a force field from the site. Visibility from the sea's look (clear, dark, silty, toxic).
Pressure as suit rating vs depth: past it, warnings, then damage -- no air meter by default (the prompt's section 18).

## 12. Underwater environments and POIs

From the prompt's list, by phase: phase 1 -- flooded facility levels, a flooded station section, a sunk wreck interior;
phase 2 -- shore wrecks, coastal stations with a moon-pool, lake research huts, underwater cave mouths; phase 3 -- reefs,
vents, trenches, submerged installations, the huge creatures. Each sea has a look and a hazard set as data, as biomes do.

## 13. Abandoned stations and space POIs

- **Interiors from the facility generator** with a station style (modules joined by spines and rings rather than
  corridors on a grid, airlocks between pressure zones, maintenance crawlways as the ducts already are).
- **Type and condition separate**, as for wrecks: the prompt's list of conditions maps onto site systems directly --
  depressurised (zones at zero), partially powered (some sources dead), emergency only (battery source, low lights),
  frozen (temperature zone), contaminated (hazard zone), stripped (salvage removed), occupied / nest-infested
  (ecology), apparently intact (nothing wrong -- and that is the point).
- **Docking**: a short sequence at the ship's console (line up, extend the collar, equalise) and a cinematic -- not
  flying the ship. Manual piloting would be a new genre; see section 34.
- **Gravity**: stations keep theirs. Zero-g movement for players and creatures is a new controller, a new rig mode and
  new physics everywhere -- XL for little gain. Recommend not now.
- **EVA**: none at first. If wanted later, short tethered walks along a hull between two airlocks, with magnetic boots
  (which keep normal walking) -- a scoped version of the prompt's "consider scoped EVA".
- **Exterior views**: station exteriors from the ship's windows and the docking cinematic, built like outposts are (as
  merged props).
- Other space POIs (debris fields, cargo fields, relays, probes, satellites) are **things to scan and fly past, and
  sometimes dock with**, not new interiors each: a scan, a log entry, a lead, salvage brought aboard by the shuttle.

## 14. Planetary POI expansion

The prompt's list sorted by what it costs to make, so the cheap ones come first:

- **Mostly existing parts, new rules:** crash sites (wrecks), camps, survey sites, comm towers, distress sources,
  impact sites, signal sources, mining/industrial/research stations (outpost generator + interiors).
- **Need terrain features:** caves and lava tubes (a cave interior generator; FacilityLayout-like but organic -- SDF
  carved, like the creatures), canyon facilities, buried and frozen structures (terrain over a building).
- **Need water:** coastal structures, submerged facilities, underwater caves and wrecks (section 11's phases).
- **Natural wonders and biological sites:** mostly art -- scatter, formations, light. Cheap once scatter exists.

## 15. Loot and discovery

- Keep the seven finds (salvage, data, samples, signals, anomalies, leads, story fragments).
- **Salvage is physical**: an item in the world, carried or bagged, brought to the shuttle; its value and tags from
  owner and condition. Some is too big to carry: a "salvage tag" marks it for the shuttle's winch (a scoped mechanic).
- **Upgrades = credits + a meaningful component**, as the prompt says: tier 2 of the drive needs credits *and* a
  recovered injector, found on wrecks of a certain kind -- so exploring moves the ship on, earning alone does not.
- **Samples** (biological, geological, atmospheric) come from a scanner and a sample kit; they feed research
  (section 26), not a crafting grind.

## 16. Mission and objective architecture

- **Objective graph**: steps with goals and prerequisites over the site systems. A step's goal is a state ("door 7
  open", "zone 3 drained", "black box on the shuttle", "the failed relay identified"), checked by the host every tick.
- **Steps are verbs the world supports**: go, find, take, carry, place, power, reroute, repair (swap a part), enter a
  code, scan, sample, read, deduce (a code from clues), choose. New verbs are added as the world gains mechanics.
- **The download-and-extract mission** is a template: find terminal -> (power) -> download -> carry -> extract.
- **HUD**: objectives show as the crew learns them, not all at once; optional goals marked as such.
- **State replication**: the graph's step states and the site systems' states (a few bytes each) to every client, and to
  the save as deltas.

## 17. Procedural mission generation

- **Templates as data** (JSON, like everything else): a contract type picks a **situation** template ("recover X from a
  dead station", "find why the relay failed"), which picks **chains** of steps with slots: which room, which item,
  which door, which power source. Templates declare what the site must have; sites that cannot host one are skipped.
- **Complications** are separate small templates applied on top: a door that needs a code, a section flooded, power
  failing again, comms down, a creature where the objective is.
- **Optional goals** are separate chains that never gate the main one (checked by the validator).
- **Variety target**: about 15 situation templates x 10 complications x the site's own variation at first, growing to
  40+ situations for a 15-25 hour campaign. Measured, not guessed: a debug command generates a hundred and lists how
  many were distinct.

## 18. Investigation, codes, power and repair systems

- **Codes**: 4 digits (data: code length per template). A code's digits are spread over 2-3 *clue objects*, each a thing
  in the world with consistent formatting (a stencil, a note, a badge, a log line). Validator rule: no clue behind the
  door it opens; at least one route to every clue.
- **Clues are logical, not riddles**: "the date on the shift log" + "the room number on the badge" -- never wordplay.
  Numbers are language-free and readable aloud in multiplayer.
- **Power**: sources, relays, breakers and loads as a graph. Missions: reset breakers in two rooms; choose which of two
  loads the limited supply runs; replace a blown relay with one found elsewhere; start generators in the right order (the
  order on a placard in the plant room). All solvable by the validator, all shown physically (lamps, fans, door lights).
- **Repair**: a broken part (a relay, a pump impeller, an antenna module) is an item; repairing is carrying the right
  one there and fitting it (a short hold-to-use). No minigames at first.
- **Diagnosis**: something keeps failing (a breaker trips again after a minute); the cause is in another room (a
  shorted load). Found by looking, confirmed by isolating it.

## 19. Mission validation and solo/multiplayer

- The validator (section 4) runs every generated mission before it is offered. Its rules are the prompt's list; each
  is a test in `Tests/` too.
- **Solo fallbacks are part of the template, not an afterthought**: anything two people do at once has a sequential
  version (a breaker that holds for 20 seconds; a door with a manual crank; a remote panel). The validator runs every
  mission as one player as well as four.
- **Separation without teleporting**: doors that close behind (power cut), a flooded section that one swims and one
  walks round, comms dropping in a shielded area. Each is a site state, temporary, and the validator checks the group
  can always rejoin.

## 20. Shared suit progression

- Functional suit tiers live in the campaign (like ship upgrades): environment ratings (temperature range, toxins,
  pressure, radiation, depth), mobility (climbing assist, fall recovery, underwater propulsion), sensors (scanner modes,
  low light, sonar, hazard detection), protection, capacity (slots, sample space).
- **Ratings gate where the crew can go** rather than draining a meter: a site's hazards are compared with the suit's
  ratings; below them the map says so before landing, and on the ground the suit warns and then hurts. This is the
  prompt's "progression expands where you can go" without babysitting.
- Bought at outposts with credits + components, as ship upgrades are.

## 21. Visible suit upgrades

- **A new suit model is needed** (the body is boxes now). Build it the way creatures are built: SDF shapes for torso,
  limbs, the round helmet, ribbed joints, turned into one skinned surface, plus hard parts (backpack, rings, hoses,
  connectors) as attached meshes. Retro-industrial: off-white/grey, charcoal joints, orange/yellow safety accents,
  stencils, wear.
- **Sockets** on the suit for modules (backpack, helmet, forearms, belt, legs). Each upgrade tier adds a module mesh in
  its socket: a bigger life-support pack, a helmet lamp array, a sonar puck, sample canisters on the belt, propulsion
  pods for water. Capability grows; the silhouette stays a person in a suit.
- First person: the forearms and helmet rim are what the player sees of it -- worth making good.

## 22. Suit customization

- Primary, secondary and accent colours; visor tint; wear/finish (clean to beaten); a decal or patch from a small set;
  an ID number. Helmet styles later (each is a model).
- Per player, stored on the player's machine (it is theirs, not the campaign's), sent when they join, replicated in the
  join message. No stats.

## 23. Ship progression and physical expansion

- Already planned: one persistent ship that grows by sections along its spine. Each section is a room module
  (armoury, research lab, suit workshop, med bay, workshop, bigger hold, crew quarters, observation, improved airlock,
  water-gear support). `ShipMap` builds rooms from code now, so a section is a room recipe plus a slot.
- The starting ship stays small and worn; upgrades visibly clean, repair and extend it (the hull look already follows
  tiers).
- No fuel grind (as decided).

## 24. Armoury and loadout

As you decided on 2026-10-06: a physical armoury stocked by gear the campaign owns; take before an expedition; what comes
back stays; what is lost is lost; a small free baseline kit always; inventory slots, no weight. It needs: an armoury room
(section 23), campaign-owned item counts (`CampaignState.cargo` exists), a pickup/return flow, and loss on death resolved
at the end of an expedition.

## 25. Economy and contracts

- Contracts are data templates offered at outposts and by signal (and later the intercom), from a few kinds of
  issuer (CIRRA, independents, industrial, research, station operators) -- each with which contract types it offers and
  how it pays. No faction politics: issuers differ in what they want, not in reputation webs.
- A contract = a site + a situation template + rewards (credits, components, gear, leads, services). Optional, always.
- Credits are earned from salvage sold at outposts and contracts; spent on ship and suit tiers and gear.

## 26. Research progression

- Samples and data turn into **research** at the ship's lab (once built): each discovery is a small unlock -- a scanner
  mode, better threat confidence, recognising a creature archetype from a distance, a log entry that connects things.
- Research is **discovery, not currency**: no points to grind; specific finds unlock specific things.
- It is also where the shared biology shows: comparing samples from different worlds is how the crew notices the common
  signature. How fast and how clearly that happens is lore (section 34).

## 27. Save changes

New keys in `CampaignState`, each a delta by key, each with a default so old saves load:
`pois` (state per POI key: condition changes, salvaged, solved codes, repaired systems, water drained, doors forced),
`contracts` (offered, accepted, done), `research` (unlocked ids), `suit` (tiers), `armoury` (owned counts; `cargo`
exists), `discoveries` (creature archetypes seen, by seed), `story` (exists). Suit appearance is per player, not in the
campaign. Bump `kVersion` and migrate. While you are the only player, a dev-only "this save is too old" is acceptable too.

## 28. Multiplayer changes

- New replicated state: objective graph and site systems (step and node states), POI changes, water levels, suit
  appearance at join, suit tiers with the campaign, group roles are host-side only.
- **Comms as a system**: today voice is proximity only. A **suit radio** channel (everyone, any distance) that the world
  can degrade -- static in shielded areas, cut by interference -- makes "comms died" a real situation. Proximity voice
  stays (and stays what creatures hear and mimic).
- Determinism stays: generated things are sent as seeds.

## 29. Rendering changes

- **Water**: a surface with reflection (the planar reflection pass already exists for the test map's mirror) and
  refraction, underwater fog and colour, light shafts (screen-space), caustics (projected), particles. Phased with
  section 11.
- **Creature skin** shader (section 6) and the suit's materials.
- **Space**: more to look at -- nebula and galaxy-band skyboxes per region, asteroid belts and debris fields as
  instanced rocks, station and relay lights, distant ship traffic near outposts, comet tails, gas giant storms.
  (Asked for on 2026-10-11 as a later pass; fits here.)
- **Planet surfaces**: ground scatter (next in the current order), weather by world.
- **LOD** for huge creatures, merged props for big sites (exists).

## 30. Performance risks

| Risk | Mitigation |
|---|---|
| Building a creature skin takes 30-100 ms | Build at site load on worker threads (it is already cached by seed); never mid-encounter. |
| Several minds at once | 8 cost 0.02 ms a tick today; fine. Ecology agents for distant/huge ones. |
| 3D water navigation | Cell graphs in flooded interiors; a coarse sparse voxel grid (2-4 m) for open water, searched on demand, cached per site. |
| Huge creatures | LOD surfaces, fewer bones, update rate by distance. |
| Water rendering | One water plane per site; underwater fog instead of volumetrics; reflections at reduced resolution. |
| Site generation | Already threaded (terrain); interiors and systems are cheap data. |
| Mission generation and validation | Data-only search over small graphs; milliseconds. Regenerate budget capped. |
| Dynamic power/doors | Event-driven (state changes), not per-frame. |

## 31. Developer and debug tools

Already there: brain inspector, overlays, `spawn_creature`, lab map, `ai.*` settings, perf report. Add:

- **Creature lab panel**: seed (lock/regenerate), archetype, locomotion, biome/adaptation, threat; anatomy readout;
  cognition axes; spawn in the lab, in water, huge; save interesting seeds to a list.
- **Mission panel**: choose POI, condition, contract type; generate; show the objective graph and site systems as a
  diagram; regenerate one step; force a code, power routing or flooding; run the validator solo and four-player; show
  why a generation was rejected.
- **Site inspector**: POI condition, ecology and why each creature is there, loot list, power/access/zone states live.
- **Suit/ship**: force tiers (`upgrade` exists for ship).
- **Water**: depth, current, visibility at the cursor.

## 32. Implementation order

Each step ends playable and committed. Earlier steps are what later ones stand on.

1. **Finish the world pass already under way**: ground scatter and weather by world; the space visuals pass.
2. **Site systems + objective graph + validator + mission panel**, with today's download mission rebuilt as the first
   template. *The foundation.*
3. **Wrecks** (decided) on it: interiors, conditions, owners, recorder, salvage to the hold.
4. **Creatures from the world**: ecology placement, threat model with confidence, per-player director tokens. No
   arrival timer.
5. **Armoury, contracts, credits, components** -- the loop closes.
6. **Codes, power routing, repair, diagnosis** as objective verbs; complications; optional goals.
7. **Cognition profile, group roles, information sharing, habit model, tells** (the mind rework).
8. **Suit model, customization, shared suit tiers and hazard ratings, visible modules.**
9. **Ship sections** (armoury first, then lab, workshop...). **Research.**
10. **Body-plan grammar, new gaits, skin shader** (the body rework).
11. **Space POIs**: derelict ships and stations (interiors, docking sequence), scan-and-fly-past POIs.
12. **Water, phase 1**: flooded interiors, swimming, aquatic creatures in rooms. Then phase 2 (shores), then 3 (ocean).
13. **Signals, survey sites, caves**; story machinery (data only); then the story, written by you; then the endgame.

## 33. What can be built incrementally

Almost all of it, which is the reason for the order above:

- The objective graph starts with the one template that exists and gains verbs one at a time.
- The cognition axes can start as a re-labelling of current traits (behaviour unchanged), then gate one behaviour at a
  time.
- The body grammar can produce today's four kinds first, then add plans.
- Suit modules are added per tier; the suit model can arrive before any functional tier.
- Water arrives as interiors before any ocean exists.
- Each POI kind is independent.

## 34. Decisions that need you

**I think these are worth deciding before I lock them.**

1. **Native wildlife.** The lore says alien life in general is known, and the organisms are unusual even so. Should
   worlds have ordinary native life (plants, small animals, the huge passive sea creatures) that is *not* part of the
   organism -- or is every creature an expression of it? This decides what the "awe" encounters are, and how rare the
   organisms feel.
2. **The shared signature.** What do the organisms visibly share (section 6), and how early can a careful player notice
   it? I would only build a slot for it until you say.
3. **Information between creatures.** Can creatures of the same place share what they know beyond sight and sound (a
   call that carries information; something through the nest)? Independent organisms argues for sight and sound only;
   the shared biology could justify a little more. And do different organisms at one site ever fight each other, or
   ignore each other?
4. **Text in clues.** Codes need clue objects: shift logs, badges, maintenance notes. May I write *functional* procedural
   text templates (for example "MAINT. LOG -- RELAY B3 REPLACED -- ACCESS 4_1_"), with people's names drawn from a name
   list -- or should every text be a slot you fill? This decides how much variety the codes can have.
5. **Underwater order.** Flooded interiors first (cheapest, most horror), or open water first (most spectacle)?
6. **Zero gravity, EVA, manual docking.** My recommendation: none for now (stations keep gravity; docking is a console
   sequence plus cinematic; no free EVA). Agree?
7. **Death penalty.** With an armoury, suit tiers and credits, what does a crew wipe cost? (Today: the design says
   "some" credits.) Options: lose what was carried only; lose carried + a share of credits; lose carried + the
   expedition's contract.
8. **Suit identity.** CIRRA contractor markings on the suits, or the crew's own (or the player's choice of patch)?

## Things the prompt does not mention that matter

- **Procedural creature voices.** Procedural bodies need procedural sound: calls, breathing and footsteps pitched and
  filtered by body size and mouth type. Today creatures share recorded clips. This is part of "coherent organism".
- **A suit radio** (section 28): without one, "comms died" cannot happen.
- **Readability for co-op deduction**: clues written large enough to read across a room in a helmet camera's view, and
  a way to point at something for others (a ping).
- **Difficulty and player count**: the director's tokens and the threat model should scale with how many are playing;
  maybe a difficulty setting that changes cognition odds, not damage.
- **Save breakage during the rework**: some steps (the mission replacement) are much cheaper if older saves are simply
  refused while you are the only player.
- **Content volume**: 15-25 hours of variety depends more on the number of situation templates than on any system. They
  are data; once the framework exists, adding them is fast, and you may want to write some yourself.
- **Tutorial**: it will need to teach codes, power and the suit's ratings as well as the controls; best written after
  step 8.
