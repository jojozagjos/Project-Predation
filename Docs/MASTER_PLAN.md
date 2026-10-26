# Master plan: everything before the tutorial and the ending

Written 2026-10-12, in answer to: *tell me the plan of how we will approach everything, in lots of detail -- a full list
before we start the ending and the tutorial.*

This is the one list. It pulls together [MAJOR_REWORK_PROPOSAL.md](MAJOR_REWORK_PROPOSAL.md) (the systems),
[WORLDS_FACILITIES_MISSIONS_PLAN.md](WORLDS_FACILITIES_MISSIONS_PLAN.md) (worlds, facilities, where to go),
[NEXT_STEPS.md](NEXT_STEPS.md) (open questions and notes) and everything you have raised in chat, and puts it in order.
Those documents keep the detail of *why*; this one says *what, in what order, and how*.

**How to use it.** You do not need to bring anything up again before its phase comes: if it is in here, it is planned.
The table at the end ([Where your requests went](#where-your-requests-went)) lists everything you have raised and the
phase it is in. Each phase ends playable, tested and pushed. Story, tutorial and ending stay last (your direction), and
nothing here writes lore: where a choice touches the story, it is marked **Your call**.

---

## Done so far (for reference)

- Open exploration: the ship, the 3D system map, travel, seeded systems and planets, outposts, landing regions, saves.
- Non-flat terrain by world; ground scatter by biome (boulders, spires, ice, crystal, basalt, leafy trees, conifers,
  shrubs, grass cards); weather by world (first pass); far sun shadows; planet rendering baked and fast.
- Outposts bigger and busier (size tiers, back streets, parked vehicles, pallets, beacons); owners (CIRRA, independent,
  industrial, abandoned).
- The ship: metal hull, landing gear, a boarding stair that slides out of a covered slot and swings down; the shuttle
  rebuilt (frame, doors, ramp, fin, engines, legs).
- Sky: no sun sweep on takeoff, no glow blobs; the sky fades to space only as the camera itself climbs.
- 74 new footstep clips by surface, each surface in its own folder.
- Every outpost world has air; the home system is the same in every campaign (fixed seed) and Kestrel's world is like
  Earth, so the start can be laid out by hand.

---

## Phase 1 -- Make it look real ✅

Everything you raised in the last message about looks. Order inside the phase is the order I will do them.

**Done (2026-10-14).** Everything above is in, with these notes:
- 1.4: pebbles, leaf litter (needles under conifers) and moss on rocks; no flowers yet (they need a picture of their own).
- 1.6: ambient occlusion (r.ao, not with multisampling); the near shadow cascade and the occlusion's small radius do the
  work of contact shadows -- say if anything still looks to float in sunlight.
- 1.8: drifts against the walls on snow and sand worlds, sparks from failing lamps, drops falling from damp ceilings. The
  outposts' own details (steam, smoke, windsocks and flags, blinking mast lights, oil stains, tyre tracks, cables and
  pipes) moved to the outpost rework in Phase 2: the outposts are being rebuilt there, and dressing them now would be
  thrown away.

### 1.1 Worn exteriors for the ship and the shuttle ✅
- **What:** painted metal that has been used: chipped edges, grime in the seams, streaks below vents and windows,
  scuffs where people walk, darker panel lines, the paint a little different from panel to panel.
- **How:** the downloaded texture sets (cargo_crate and the other worn-metal sets) as the base. To avoid the *tiling*
  look you disliked before: each panel gets its own texture offset and rotation (so no repeat lines up), the texture is
  big on the hull (a metre or more per repeat), and a second, larger "dirt" pattern breaks up whatever repeat is left.
  Edges and corners get extra wear from the shape itself (the shader brightens and roughens where faces meet).
- **Also:** functional decals only (hazard stripes, arrows, "NO STEP", panel numbers, the crew's marking slot);
  nothing with story in it.

### 1.2 Real crates and cargo, inside the ship and out ✅
- **What:** the textured cubes become actual objects: ribbed plastic crates with lids and handles, metal cases with
  latches and corner guards, steel drums, gas bottles in racks, cargo nets, pallets wrapped in film, tool chests,
  equipment cases with foam inserts.
- **How:** a small prop library made from parts (as the shuttle is): each prop a few shaped pieces with its own
  material, so the texture fits its shape. One library used by the ship, outposts, facilities and wrecks.

### 1.3 Weather you can feel ✅
- **Sand and dust (your example):** no more cubes. Soft, round, blurred sprites that fade near surfaces; streaks
  blowing past in the wind; and, most important, the air itself thickens: fog density and colour follow the storm, so
  in a sandstorm you see 30-60 m and everything is tinted. Gusts come and go, so visibility breathes.
- **Snow:** soft flakes that drift and swirl, faster streaks in a blizzard; upward faces whiten as it falls.
- **Rain:** streaks lit by lights, splashes on the ground, surfaces darker and shinier when wet, drips under edges;
  thunder and lightning in storms (a flash, then thunder a moment later).
- **Ash:** slow grey flakes; embers glowing in ash storms; a dim orange haze.
- **Wind** drives all of it from one value per site, which also moves the plants (1.4) and sets the sound.
- **Sound:** recorded loops by weather (wind, rain, sand hiss), louder outside, muffled indoors. CC0 where possible.

### 1.4 Grass and things moving in the wind ✅
- **Grass:** dense ground cover where the world has plant life, drawn in rings round the camera (dense close up, thin
  further away, none far out) so it costs little. Colour by biome, patchy, thinner on slopes and paths, none on roads
  and pads.
- **Wind sway:** grass bends and ripples in gusts; shrubs sway; trees bend at the trunk and their leaves flutter; dead
  trees creak. All done in the vertex shader (cheap), by how far each point is from its root.
- **Small things:** pebbles and small stones, leaf litter under trees, flowers or moss on some worlds.

### 1.5 The doorstep and the rest of the flicker (z-fighting) ✅
- The facility doorstep sill is fixed. The automatic test that finds flickering faces will be run on facilities and
  wrecks too, not only the ship and outposts, so new ones are caught before you see them.

### 1.6 Light and air ✅
- Ambient occlusion (darkening in corners and under things), contact shadows, light shafts in dusty or foggy air, fog
  colour matching the sky, auto-exposure tuned (stepping from a dark room into daylight adjusts like an eye).
- A pass on lamp brightness and bloom, so lights read as bright without glowing blobs.

### 1.7 Space (your earlier request: more to see) ✅
- A starfield with colour and brightness variety, the Milky Way band, a faint nebula backdrop per system; planets with a
  glowing rim of atmosphere, rings that cast shadows; the sun with a corona and a soft lens flare; moons in the sky from
  the ground; dust streaming past the windows when travelling (in already).

### 1.8 Little details for realism ✅
- Outposts: oil stains, tyre tracks, puddles after rain, steam from vents, smoke from stacks, windsocks and flags in the
  wind, blinking aircraft lights on masts, cables and pipes between buildings.
- Sites: wreckage and old equipment near where people were, tracks and footprints in snow and sand, drifts piling
  against walls.
- Effects: sparks from damaged machines, dripping water, flickering broken lights.

### 1.9 Quality settings ✅
- One preset that scales grass, scatter, shadows, weather and effects, so weaker machines still run well.

---

## Phase 2 -- Worlds that feel different (out of the bowl)

From WORLDS_FACILITIES_MISSIONS_PLAN.md W1-W4:
- ✅ **A real horizon** (W1): the site sits in open land with a far terrain ring out to a few kilometres. *Done 2026-10-18.*
  The edge of where you can go is the land itself (a ravine, a cliff, water) or a soft limit, never a ring of rock --
  *done 2026-10-19: ridges with cliff faces, ravines on canyon and mountain worlds, the sea at coasts, one pass each.*
- ✅ **Landmarks** (W3) -- *done 2026-10-19, natural ones*: arches, spires, buttes, balanced rocks, giant trees, crystal and
  ice towers scattered over the land by the world (in groups where its land makes them), near and far, never on the site,
  each its own shape, and old masts now and then. (Decided 2026-10-19, your pick: crashed hulks come with the wrecks in
  Phase 5, so any hulk seen can be explored.)
- **Landform types** (W2) -- *partly; 2026-10-20 five more: ice sheets (pressure ridges, crevasses, peaks standing out),
  glacial valleys (a U-shaped trough), lava plains (hummocks, blisters, a volcano's cone far off), karst (sinkholes, limestone
  towers round it), highlands (moor and tors on a plateau, the land falling away); each world's landforms at their own size round the site (ranges, mesas, great
  craters, swells); the rest to come*: mesa country, canyons, crater fields, ice sheets, dune seas, lava plains, pillars and
  sinkholes, glacial valleys, highlands, crystal fields.
- **Each world's own light, air and sound** (W4) -- *partly: each star its own colour and size; ambience by weather*:
  the star's colour, thick air warm and hazy, thin air with a dark sky at noon, moons at night, ambience by biome.
- **Landing areas that feel like somewhere** (your note): a reason the pad is there, things round it, a way in. *First
  pass done 2026-10-20: red beacons pulsing at the pad's edges, cargo staged beside it (a container or two, a fuel tank),
  a windsock filled by the wind, trails worn from it to the doors and out along the valleys.*
- **Outposts on the world's own ground**, with the same scatter and weather round them. *First part done 2026-10-20: the
  world's own terrain round every outpost (its landforms, grass, trees and stones, the slab levelled into it, the land
  running on with no cliffs, the ship's way in kept low, a soft limit where it ends); its weather still to come. Kestrel
  keeps its own for now, and the copy the landing cinematics film from the air still stands on plain ground.*
- **A full outpost rework** (you, 2026-10-12: "way too small and don't make sense at all"): outposts laid out as
  real places with a reason to exist -- a landing field with taxiways and service bays, a town or works round it (operations,
  fuel, cargo, workshops, quarters, a canteen, power and water), roads that lead somewhere, the land round it used (fields,
  mines, antennas) -- sized by the outpost's importance, many times bigger than now, and walkable. And sized for the ship
  as upgrades make it bigger (you, 2026-10-13): fields, pads and hangars that take the largest hull it can grow to, not
  only the one it starts as. With real buildings people would use (hangars, workshops, warehouses, offices, quarters,
  a canteen, a control tower), the world's own ground under it -- terrain, grass and scatter as at a site -- and its
  weather (rain, snow, dust, storms and lightning, as at sites).
  And (you, 2026-10-19): the world's own terrain under and round it; pretty big; a large pad, sized for the ship as it
  will grow; redesigned so the buildings and everything between them connect (today some do not); fitted to where it is
  -- snowed on, green, sandy, as the world is; air everywhere people are. My additions, for you to say yes or no to: a road
  in from the land round it and the field's lights to bring the ship down, service vehicles at the pad, and the world's
  weather showing on it (snow banked against walls and ploughed off the field, sand drifted against the hangars).
- **The outposts' details** (moved from 1.8): steam from vents, smoke from stacks, windsocks and flags in the wind,
  blinking lights on masts, oil stains and tyre tracks, cables and pipes between buildings.
- ✅ **Ground of more than one kind** (you, 2026-10-18) -- *done 2026-10-18: a second surface per world in ragged patches,
  sand beaches along the sea, footsteps to match; 2026-10-20 by how the land lies: hollows darker and damp, rises paler and dry, trails worn bare from the pad to the doors and out along the valleys*: the ground
  blended from several surfaces -- soil, gravel, grass, sand, rock -- in patches by how the land lies (wet hollows, dry
  rises, worn paths), not one texture over the whole world.
- **Every world its own** (you, 2026-10-19: "all planets need to be unique ... lava ... toxic planets") -- *partly done
  2026-10-19*: lava pools on volcanic worlds (crusted, glowing, lighting their banks, burning), acid on toxic ones (wading
  burns), frozen lakes and seas on icy ones (walked on), steam and gas vents, mist in jungle
  hollows, frozen waterfalls down cliffs, spindrift in the wind, lightning glass on storm worlds.
- ✅ **Signs of the organism beyond the creatures** (you, 2026-10-19; faint early and plainer later, kinds my pick; then, the same day: wherever the creatures are, so at every mission site, plainer the harder the place) --
  *first pass done*: root-like branching rock formations, one shared rhythm (a faint
  glow, lamps dipping) the same on every world. Their colours, how far out they are plainest and the rhythm's
  timing are data (universe.json `signs`) for you to set when the story is written. Old human finds wait for the records.
  (Decided 2026-10-19, your pick: crystal does not glow; a glow in the dark stays the organisms' sign.)
- ✅ **Trees and grass over all the land** (you, 2026-10-18: "just around the facility") -- *done 2026-10-18*: woods and
  clearings round the site and out to the horizon, grass over all the ground you can walk on, thinner in the woods.
- ✅ **Water you can see** (moved up from Phase 12, 2026-10-14, because so much else has to know about it) -- *done
  2026-10-18; streams and frozen lakes still to come*: a water surface
  (reflections, darker with depth, gentle waves, foam at the shore, rain rings in a storm); a sea level at oceanic, island
  and coast sites, and lakes and streams elsewhere, with buildings and pads kept on dry ground; wading in the shallows
  (slower, splashing steps and sound); deep water the edge of where you can go until swimming comes in Phase 12.

---

## Phase 3 -- Why go there: the mission foundation

From the proposal's step 2 and the worlds plan's M1-M3:
- **Site systems and an objective graph:** what a site *is* (its systems: power, doors, terminals, data) and what can be
  done there, as data. The current download-the-data mission becomes the first template.
- **On the map:** an orbit scan shows each landing region with a category icon (structure, wreck, signal, anomaly,
  survey) and a one-line read ("Crash site, recent. Owner unknown. Possible salvage."); better sensors add threat and
  the suit rating needed. Places with nothing say so.
- **Contracts** at outposts and over the intercom, marked on the map with what is wanted and the reward; accepting
  plots the course. **Leads** found on sites add markers with the note that led there. **The ship's log** lists open
  contracts and leads.
- **On the site:** a short landing brief (recorded voice clips plus text); the objective tracker points only at what
  the crew knows; objectives appear as they are learnt; a **ping** so players can point things out to each other.
- **The debrief:** what was recovered, what it is worth, what it unlocked, what was lost.
- **The early game made linear** (your idea, now possible because home is fixed): the first few contracts are set ones
  in the home system that walk a new player through landing, salvage, the map and the first upgrade, before the galaxy
  opens up. Content for this is written with you.

---

## Phase 4 -- Facilities: bigger, fuller, all different

Your notes: they all look the same outside, feel small, are just room-hallway-stairs, need lots more in them, vents
should look like vents, and the lamp path is not loved. From the worlds plan F1-F4:

- **Underground and underwater (you, 2026-10-20):** caves reached from workings, mines, flooded caves to swim through,
  underwater facilities -- planned under Phase 13, built with this rework. (Basements are in already.)
- **Every outpost building enterable (you, 2026-10-25):** built with this rework -- the outposts' blocks, sheds, hangars and
  habitats given interiors from the same generator (built as you near them, so arriving stays quick). Sealed shells until then.
- **Facility types**, each with its own recipe: research lab, mining operation, comms relay, habitat, refinery, cargo
  depot, observatory, survey camp. Each has its own outside shape, rooms, props and lighting.
- **Rooms with a purpose and sensible neighbours:** a clean room behind an airlock, a control room overlooking a hall,
  quarters near a mess.
- **Space that feels big:** double-height halls with catwalks and a mezzanine, warehouse floors with racking, atriums,
  long sightlines and windows to the outside, ramps and spiral stairs, lifts and ladders, several buildings joined by
  covered walkways and tunnels, service tunnels and maintenance shafts.
- **Lots in them:** dressing by purpose (benches, sinks, fume hoods, turbines, valves, bunks, lockers, kitchens, server
  racks, specimen stores), cables and pipes along ceilings, functional signage and labels.
- **Vents that look like vents:** grilles, frames, ducting running to them.
- **Condition you can read:** power out (emergency lighting), fire, stripped, left in a hurry, frozen, overgrown,
  occupied (nest growth).
- **Outsides that sit in the land:** dug into hillsides, on stilts, fences and gates, vehicle bays, solar arrays,
  antennas, cargo; snow-buried or half collapsed by condition.
- **The way in** (replacing the lamp row): one of a lit entrance, light spilling from a door, a worn path, signage, or
  a beacon -- chosen per site.
- **Owners** change materials, colours, signage and upkeep, as outposts already do.
- **Mirrors in what is generated** (you, 2026-10-13): the mirrors that exist now (the test map's, polished, brushed and
  matte) placed by the generator where they belong -- washrooms, changing rooms, quarters, labs, polished panels and steel
  -- in facilities, outposts, stations and wrecks; cracked, fogged or dirty by condition.

---

## Phase 5 -- Wrecks, salvage and the hold (decided as the first new place)

- Broken hulls you walk into, laid out like a ship; conditions (recent, old, picked over, occupied, environmental
  damage, unexplained); owners; the flight recorder (a factual log entry, plus slots for your recordings); salvage into
  the ship's hold with credit values and tags (upgrades, contracts, research).

---

## Phase 6 -- Creatures and the AI

Your direction (2026-10-06): keep the existing intelligence; change where creatures come from. Your answers
(2026-10-12): native wildlife on some worlds, a subtle shared signature, limited information sharing, organisms can
fight each other.

1. **Creatures from the world:** placed because the place says so (an occupied wreck, a nest in a cave, a contaminated
   facility, a roamer outside); many places have none; no arrival timer. A threat estimate with a confidence on the
   map. A director that paces encounters per player.
2. **The nest** (your note): rethink what it is, how it grows and how it reads, with the shared signature.
3. **The mind rework:** cognition profiles, group roles, limited information sharing (calls that carry a little),
   habits players can learn, tells; organisms of different kinds at one site can fight.
4. **The body rework:** body plans beyond today's four, new ways of moving, a skin shader; procedural voices pitched by
   body size.
5. **Native wildlife:** harmless local animals on some worlds (fliers, grazers, scuttlers, fish later) with simple
   steering, so the organisms stand out.

**Your call** when we get there: what feels wrong with the AI today, in play, if anything.

---

## Phase 7 -- The loop: armoury, contracts, credits, upgrades

- A physical **armoury** stocked from what the campaign owns; a small free kit always there; better gear scarce.
- **Credits and components** from salvage and contracts; **upgrades** at outposts (drive, sensors, hold, ship size,
  modules); some tiers need a found component, so exploring moves the ship on.
- **A wipe** (your answer): lose field gear and salvage, plus a capped 10-20% credit fee; permanent progression stays.
- **Research:** samples and data into knowledge (better scans, leads).
- **The jump cutscene made again**, with the drive upgrades (you, 2026-10-13): it turns while it plays and the player is
  sometimes seen flying for a moment; redone as one steady shot of the ship going, nobody seen out of place.

---

## Phase 8 -- Things to work out on site

- **Codes** found on clue objects (functional text, generated -- your answer allows it), **power routing**, **repair**,
  **diagnosis**, as objective steps; complications and optional goals.
- Clues readable at a distance; colour-blind safe markers.

---

## Phase 9 -- Suits and the crew's look

- A real suit model instead of the box people; the crew's own markings first, a CIRRA contractor patch second (your
  answer); customization; shared suit tiers with hazard ratings; modules you can see on the suit; a first-person body.

---

## Phase 10 -- The ship grows

- New sections along the spine with upgrades (armoury first, then lab, workshop); the ship's inside dressed to match
  (lived-in crew quarters, the mess, equipment).

---

## Phase 11 -- Space places: stations and derelicts

Your direction (2026-10-12): stations are places you can **get things at** and **have missions at**, and some are
abandoned.

- **Working stations**, in orbit or deep in a system: dock (docking console plus cinematic) and walk aboard -- trade,
  refit and upgrades, a contracts board, crew services, the station's own people and owners (as outposts have them:
  CIRRA, independent, industrial). They work as orbital outposts.
- **Abandoned and damaged stations**: the facility systems (power, doors, terminals, codes) in a station's shape --
  ring corridors, docking spines, hab modules, hydroponics, reactor sections -- with conditions as wrecks have them
  (power out, vented sections, overrun); salvage, logs and missions inside.
- **Derelict ships** drifting in a system: smaller than a station, a ship's layout, the same conditions.
- **Missions at them**: contracts to recover, repair, investigate or rescue at a station; leads that point to one.
- **Getting there**: no zero-G, no EVA, no manual docking (your answer) -- stations keep gravity; docking is a console
  and a cinematic.
- **Scan-and-fly-past points of interest**: debris fields, beacons, wrecks too small to board.

---

## Phase 12 -- Water (your order)

(The water surface, sea levels at sites and wading are in Phase 2.)

1. Flooded interiors and swimming; 2. deeper open water; 3. underwater outside places; 4. the deep ocean; 5. huge aquatic
organisms. And (you, 2026-10-18) water worlds: planets that are nearly all ocean, reached by platforms and islands, with
underwater structures and life.

Already in (Phase 2): the water surface (sky, sun, waves with the wind, rain rings, whitecaps in a gale, lightning, the
torch, clear shallows, foam washing at the shore), the sea at coasts and round islands, lakes in low ground, wading
(slower, splashing, a lapping shore), the deep water as the edge until swimming. Still to come there: streams, frozen
lakes on cold worlds, recorded water sounds in place of the made ones.

**Plan for water worlds and better water (you, 2026-10-20):**
- **Waves that can get really big, by the weather.** The surface moved on the GPU as a sum of travelling waves (Gerstner:
  the crests sharpen and the water moves round in circles, not only up and down), their height, length and direction
  from the world's wind and weather -- glassy in a calm, a chop in a breeze, swells of several metres in a storm, and on a
  water world's open ocean, storm seas of ten metres and more. The same waves worked out on the CPU for whatever floats or
  swims (a swimmer lifted and dropped by them, the shore's wash timed to them), the same for everybody from the campaign
  clock. Spray and blown foam off the crests in a gale.
- **More real water:** the colour by depth and what is in it (blue open ocean, green coastal, brown silty), light
  through the crests, foam that streaks and fades along the waves, caustics on the shallow floor, a proper sky
  reflection with the sun's glitter path.
- **Water worlds:** nearly all ocean -- a site there is a platform or a rig on legs, a floating station, or a chain of
  small islands and reefs; walkways between, the sea all round and below; ladders down to the water and moon pools in
  the floors; underwater structures and wrecks below; a shuttle landing on a pad.
- **Under the surface:** the ocean floor going down to the deep, kelp-like growth and life (Phase 6's creatures), light
  dying away with depth to dark, the torch the only light down there.
- Order: waves and the water's look first (every coast and lake gains), then the platform site, then underwater places.

---

## Phase 13 -- More kinds of place

- **People at outposts** (you, 2026-10-20: maybe later, not now -- moved here from Phase 2): workers walking between
  buildings, loading cargo, at the fuel pumps.
- Signal sources (an uncertain signal to track, several outcomes), survey sites (discovery for the ship's log), caves,
  anomalies, remote stations with the bigger facility systems.

**Plan for underground (you, 2026-10-20) -- to be built with the Phase 4 facility rework (you, 2026-10-20), basements
already in:** the ground is a height field, which cannot have anything under it, so
underground places are built as their own pieces, set into it:
- **Caves (you, 2026-10-20: where a facility or something needed to be down there):** reached from workings -- a mine,
  a tunnel, a facility's lowest level breaking into a cave; a network of passages and chambers made as their own mesh
  (carved from a 3D field, smoothed into rock),
  entered where it meets a hillside or a crater wall or a sinkhole -- the ground cut away round the mouth so the two
  join. Narrow squeezes to crawl, drops to climb, pools, an underground river; lit only by what you bring, glowing
  growth or crystals in some; their own echo. Lava tubes on volcanic worlds, ice caves on frozen ones.
- **Underground facilities (you: like the normal ones; owners and kinds as the outposts' are):** levels below a site's
  buildings (the generator already stacks floors), reached by stairs,
  a lift or a ladder shaft; or whole facilities that are mostly underground with only an entrance block showing -- a
  bunker, a mine with its headframe and shafts and tunnels, a vault.
- **What it needs underneath:** the minimap, the creatures' navigation, sound and lighting all handle levels already
  (the facilities have them); the new parts are the cave mesh and how its mouth meets the ground.
- **Basements (you, 2026-10-20):** the normal facilities have levels below ground too, as well as the floors above.
- **Flooded caves (you):** a cave or working flooded, to be swum through to reach something a job needs -- the
  swimming, underwater light and murk that are in already, with air to think about (Phase 12).
- Order: basements and underground facility levels first (reusing the buildings), then mines, then caves, then
  flooded ones.

---

## Phase 14 -- Sound

- Ambience by place and weather; the ship's hum and creaks; outposts' activity; machinery on a powered site; a suit
  radio (so comms can fail); creature voices by body; more footstep surfaces.

---

## Phase 15 -- Interface, settings, credits

- The map clearer (categories, contracts, leads); the HUD's objective list; the ping; settings (quality preset, audio,
  controls, accessibility); a **credits screen** (also needed for the CC-BY footstep sounds); save/load screens.

---

## Phase 16 -- Under the hood (alongside every phase)

- Performance budgets per phase (frame time with grass, weather, facilities, creatures), level of detail, culling.
- Multiplayer sync for each new system; save versioning (older saves refused while it is just you, where that saves
  work).
- Developer tools for each system (spawn, preview, report), as now.

---

## Phase 17 -- Story machinery, then the story, the tutorial and the ending (last)

- **Machinery first, as data:** story threads, fragments placed in places, logs and recordings; rules for when enough
  is known to reveal the next step; the final destination hidden on the map until its thread is complete.
- **Then the story, written and reviewed by you** (your locked direction is in memory; nothing is built from it until
  you go through every point).
- **The starting area reworked for the tutorial** (you, 2026-10-12): Kestrel Station rebuilt as the place the tutorial
  happens -- with the full outpost rework's scale and sense, on its Earth-like world.
- **The tutorial** at Kestrel Station (approach, establishing shot, hangar, the starting ship), teaching controls and
  the systems that exist by then (codes, power, suit ratings).
- **The ending**, with its own look.

---

## Where your requests went

| You raised | Where |
|---|---|
| Sky snapping to stars leaving a planet | ✅ Done (sky follows the camera's height) |
| Shuttle back door gap, fin not connecting | ✅ Done |
| Ship stair floating up, undercarriage | ✅ Done (slot and flap, real frame) |
| Doorstep z-fighting | ✅ Done; test extended in 1.5 |
| Outposts need air | ✅ Done |
| Same starting planet every seed; linear start | ✅ Done (fixed home system); the set early contracts in Phase 3 |
| Worn texture on ship and shuttle | ✅ 1.1 |
| Crates look like scratched cubes | ✅ 1.2 |
| Weather looks bad; sand is cubes and does not block vision | ✅ 1.3 |
| Grass on planets; trees and bushes in the wind | ✅ Done (grass cover round you on grassy worlds; plants sway with the wind) |
| Denser trees where needed | ✅ Done (groves on temperate worlds, dense forest on jungle worlds) |
| Other things to look real | ✅ 1.6-1.8 |
| More to see in space | ✅ 1.7 |
| Shadows further | ✅ Done (far shadow map) |
| Facilities look the same outside, feel small, need more in them | Phase 4 |
| Vents should look like vents | Phase 4 |
| Lamps leading the way | Phase 4 (the way in) |
| Landing area should feel better | Phase 2 |
| Going to planets feels like nothing is there; why go places | Phase 3 |
| Worlds and facilities feel like a retexture | Phases 2 and 4 |
| Telling players where missions are | Phase 3 |
| The nest needs rethinking | Phase 6.2 |
| The AI | Phase 6 |
| Footsteps (done), metal footsteps | ✅ Done; more surfaces in Phase 14 |
| Outposts small and dead; a full outpost rework | ✅ First pass done; the full rework is in Phase 2 |
| Starting area reworked for the tutorial | Phase 17 |
| Starting planet like Earth | ✅ Done |
| Ship glowing in the sun in space | ✅ Done (rougher hull, space sun no brighter than a clear day) |
| Z-fighting on the stair | ✅ Done (and a test for it) |
| Version number to know the build | ✅ Done: `v0.2.0 build N`, N counted automatically; the version goes up a step each phase |
| Stations: get things there, missions there, abandoned ones | Phase 11 |
| Stair and landing gear textures; shuttle engines and stripes | ✅ Done |
| Nose without collision; gear in the floor; light through the bay doors | ✅ Done |
| Stuck in the closing door; the doorway message | ✅ Done (the door waits for you; no message) |
| Running wobble | ✅ Done (toned down) |
| Landing area picked for you | ✅ Done (you choose it on the map) |
| Debug windows behind the map and cinematics | ✅ Done |
| Landing title card | ✅ Done (redesigned; real local time) |
| Sand: in the shuttle, in buildings, pouring out of buildings; the shuttle seen through fog; clear blue sky in a storm; planets too sharp in the sky | ✅ Done |
| Flare kept after it was dropped | ✅ Done |
| Sand footsteps | ✅ Done (new recording) |
| Terrain row on the planet panel | ✅ Removed |
| Floating cubes under the stair | ✅ Done (the side beams were being drawn as their short end pieces) |
| Landing title card too modern | ✅ Reverted to the old one (keeps the real local time) |
| Containers that do not look like containers | ✅ Done (corrugated freight containers everywhere) |
| Freeze when closing a lobby | ✅ Done (the router is no longer waited on) |
| Graphics settings must not remove shadows | ✅ Done (shadows are always on; presets scale distance, grass, weather, mirrors, edges) |
| Out-of-breath sound | ✅ Done (your recording; CC-BY, credited) |
| Credits for the CC-BY sounds | Phase 15 |
| Low graphics showing as Custom; the shadow slider going lower than it can be set | ✅ Done (Low stays Low; the slider starts where shadows can) |
| Landing area still picked for you | ✅ Done (the planet view picks none either) |
| Debug windows under the cinematic bars | ✅ Done (the bars and fades are drawn under the windows) |
| Outside to inside switching at once | ✅ Done (the air eases over half a second through a doorway) |
| Sand in buildings; the shuttle odd through a doorway | ✅ Done |
| Grass sitting on the ground, popping in and out, ending in a line | ✅ Done (grows out of the ground with distance; matched to the ground; thins out at its edge) |
| Lamps gone, only their lights; a block floating in the sky | ✅ A likely cause fixed (a mesh given back while still drawn); tell me if you see it again |
| Ground texture tiling | ✅ Done |
| Standing on the shuttle and ship roofs | ✅ Done |
| Jump cutscene turning, the player seen flying | Phase 7, with the drive upgrades |
| Outposts too small for a ship that will grow | Phase 2, in the full outpost rework |
| Thunder and lightning that light everything; realistic clouds for the weather | ✅ Done (designed thunder for now; recordings would be better) |
| Weather and lightning the same for every player | ✅ Done (and the rule from now on: anything everybody would compare is synced) |
| Head through the shuttle's side to its prompt | ✅ Done (its nose solid at its skin; a test checks all along it) |
| Clouds that look real; the whole sky and the ground lit by lightning | ✅ Done |
| Grass and trees showing their card edges | ✅ Done (tufts fan from a root; crowns lit as one mass; edge-on cards thin away) |
| Planets blurry close to; weather visible from space and the map | ✅ Done (detail worked out over the baked maps; storm systems, dust veils, lightning on the night side) |
| Every sun looks the same | ✅ Done (its own colour, its real size from where you are, a little brighter or dimmer by its light) |
| Ambience: rain, wind and storms you can hear, nothing out of place, muffled indoors | ✅ Done (recorded weather, CC0; drips only where it is wet) |
| Straight to a world in another system; nothing to see arriving at a system | ✅ Done |
| The ship going over the landing area before the shuttle can go down | ✅ Done |
| Outposts: buildings, ground, grass, weather; people walking about | Phase 2 (people: your call) |
| Mirrors placed by generation | Phase 4 (facilities), and outposts, stations and wrecks as they come |
| Clouds swaying back and forth fast | ✅ Done (they drift one way with the wind, by the campaign clock) |
| Noisy pattern in the torch beam | ✅ Done (no more jitter in its haze) |
| Rain loud inside buildings; facility lit by lightning indoors | ✅ Done (loud on the top floor only; lightning lights only what the sky reaches) |
| Snow on the character the moment they step out | ✅ Done (bodies, creatures, the drone and held things are never snowed or rained on) |
| Thunder and much else too loud | ✅ Done (weather and ambience recordings levelled to one loudness, thunder and roof rain turned down); say what is still off |
| Mud steps sounded like guts; one surface over the whole world | ✅ Done (mud steps removed, that ground uses gravel; the world's rock and soil show through in patches) |
| Pause and inventory open together; moving while paused | ✅ Done (pause stops movement and every action; Escape closes the inventory first) |
| Planets and moons drawn over each other on the map | ✅ Done (moons spaced by size, planets pushed out to make room; tested) |
| Sound in space in the cutscenes | ✅ Done (a camera outside the ship in space hears nothing but music) |
| The near-instant travel cutscene | ✅ Removed (both leaving and arriving, with a near-instant drive) |
| Ship snapping into place on the globe | ✅ Done (drawn where it really is round its orbit all the time) |
| A crash site appearing the moment the ship came back | ✅ Done (places are found as the orbit carries the ship over them, after a while, not on arrival) |
| Third person: the character jittering when strafing | ✅ Done (the camera was framing last frame's pose) |
| Weather starting up and changing smoothly | ✅ Done (each world's weather comes and goes in spells over minutes, building to storms at its peaks, the same for every player) |
| Snow and puddles appearing at once; the shuttle arriving snowed on | ✅ Done (the ground as the last hour left it on arrival, building up over minutes after; the shuttle comes down clean and gathers it; the ground under it keeps what was there when it landed) |
| Snow on sloped edges; snow cleared under creatures | ✅ Done |
| A storm's sky white, its clouds unrealistic | ✅ Done (a closed low deck, the day dimmed and greyed under it, the horizon lost in the rain) |
| Planet lightning in squares | ✅ Done (soft flashes in the cloud, a few at a time) |
| Torch looking like it lit far mountains; off centre | ✅ Done (its haze drawn over the sky too and toned down; aimed in at the crosshair) |
| Travel far too fast with a top drive; the ship snapping over a site | ✅ Done (no faster past drive tier 6 within a system; moving over a site always takes some seconds) |
| Sound in cinematics; the launch beep too loud | ✅ Cinematic sounds kept (only wind and ambience go in space); beep turned down |
| Weather too loud indoors | ✅ Done (rain, wind and thunder much quieter and more muffled under a roof; the roof's drumming only loud on the top floor) |
| A landing area appearing out of nowhere; scanners | ✅ Scanning removed for now: every place on a world and everything about it known at once (ideas for what scanners could become: your call) |
| Grass growing in as you walk; tufts sunk in the ground | ✅ Done (all of it out to 28 m, then thinning out past 100 m by fading, not sinking; only a little sunk in snow) |
| A ringed world's rings from its ground | ✅ Done (a band across the sky, the world's shadow across it at night) |
| The sun jumping as the ship got over a landing spot; turning too fast | ✅ Done (its way round stays steady to the end, tested; big swings turned at a ship's pace; turning no longer sped up much by the drive) |
| Leaving for another system: the planet vanishing, full speed at once | ✅ Done (twenty seconds leaving first: turning, gathering speed, the world falling behind) |
| A planet jumping as the ship came into orbit | ✅ Done (it settles down to the orbit's height, not dropped onto it) |
| Rock on the flat ground; footsteps not matching | ✅ Done (small outcrops on rises only; footsteps on them sound of rock) |
| sandstorm_2 sounding bad; the scan's on-screen text | ✅ Removed |
| Footprints in everything soft, filled in by the weather | ✅ Done (sand, snow, soft earth, snow lying on any ground, mud in the rain; sooner gone in falling snow, blowing sand, rain) |
| The ship spinning round to line up for a site; the planet snapping as it gets there | ✅ Done (it goes on the way it was going and bends round to the place, no swing at the end) |
| Rocks and leaves inside buildings; drops lying on the floor | ✅ Done |
| The test map seen from a site | ✅ Done (sites moved 6 km from the testing area, the lab and the ship) |
| The gale sound | ✅ Done (a real strong wind, recorded with a SoundField microphone, CC0) |
| The arrival cinematic gone for a new world | ✅ Back |
| Leaving an outpost straight from the map | ✅ Done (the map plots it; the ship takes off from the helm) |
| Planets and moons in the sky as white circles | ✅ Done (faces of their own) |
| Terrain and lamps missing after a while; things bugging out | ✅ Done (each site gives its meshes back; the grass, far too heavy, cut to a third) |
| The countdown in the corner | ✅ Removed |
| Seeing the ship overhead from a world's ground | ✅ Done (a point of sunlit light going over at night) |
| Ship rotation when changing course felt strange | ✅ Changed earlier (the turn no longer swings through the planet); tell me if it still feels off |
| Ground all one texture | ✅ Done (a second surface in patches on every world; sand beaches; footsteps match) |
| Water as real as it can be, with the weather | ✅ Done (waves with the wind, rain rings, whitecaps in a gale, lightning and the torch on it, foam at the shore) |
| Swimming, underwater places and life, water worlds | Phase 12 |
| Trees and grass only round the facility | ✅ Done (woods and clearings out to the horizon; grass everywhere you can walk) |
| Water at every site; walking off forever | ✅ Done (lakes at about half the places; the edge is cliffs, ravines or the sea, with a pass somewhere) |
| Distant trees looking poor | ✅ Done (crowns of several lumps, tiered conifers, softly lit; nearer ones out to 200 m) |
| A quicker way to test planets | ✅ Done: `test_site <biome> [ground] [water] [seed]` from anywhere, the title screen too |
| Console autocomplete | ✅ Done (finds any part of a name, shows usage and argument values, arrow keys pick) |
| Outposts: terrain, big, a pad for the grown ship, buildings that connect, fitted to the world, air | Phase 2, the outpost rework |
| Landmarks right by the facility; the same landmark everywhere | ✅ Done (scattered over the land near and far; each its own shape) |
| Every planet unique; lava; things on toxic worlds | Partly done (lava, acid, ice, vents, mist, spindrift); more per world to come |
| Signs of the organism besides the creatures | ✅ First pass (formations at every creature site, one shared rhythm; tunable in universe.json) |
| Everything synced in multiplayer | ✅ Checked: all of the world comes from the site seed, the shared place and the shared clock; burns dealt by the host; the site test commands are solo-only |
| Ending and tutorial | Phase 17, last |

---

## Where we are

Phase 1 is done. Phase 2 is under way (2026-10-18): the horizon, water, mixed ground and the woods are in; next the edge of
the land (cliffs, ravines, the sea) and water only at some sites, then landforms, landmarks, each world's light and air,
landing areas and the outpost rework. Then Phase 3.
