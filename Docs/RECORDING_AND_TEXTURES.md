# Project Predation: recordings and textures to get

A checklist. Tick things off (`[x]`) as you add them.

---

## Part 1: Voice recordings

### How to record

- **Format:** `.wav`, mono, 16-bit, 44.1 or 48 kHz. Your `orders_01/output.wav` is exactly right. (Your
  `Player/jump` recording is not: the game reads `jump_1.wav` as having no WAV header, so jumping is silent. Export it
  again as a WAV.)
- **Any file name ending `.wav`** works. With several takes in one folder, the first by name is used.
- **Record dry:** no music, no reverb, and about a quarter of a second of silence at each end. The game adds the
  intercom/radio sound itself.
- **Delete `placeholder.wav`** from a folder once your recording is in it.
- **Intercom subtitles** go in `Assets/Data/intercom.json`, in the `"subtitle"` next to the folder's name.
- **A second take for a moment:** make a `<moment>_02` folder and add a second entry for it in `intercom.json`.
  The game picks one, the same on every player's machine. One take per moment is enough to start.

### Intercom: the ship's voice (`Assets/Audio/Intercom/<folder>`)

The intercom is **your own ship's** voice now, not CIRRA's: the crew are contractors flying their own ship, choosing
where to go. Nothing should give orders or send people to a briefing room (there isn't one; the ops room has the
navigation table). The wording below is only a **suggestion**, to show what each moment is for. Write your own, put
it in the `"subtitle"` in `Assets/Data/intercom.json`, and record to match.

Each moment has a folder with a `placeholder.wav` in it; put your recording in and delete the placeholder.

**Travel** (new with the system map)

- [ ] `course_set_01`: *a destination is chosen and the ship is leaving.* Suggestion: "Course set. Departing now."
- [ ] `course_changed_01`: *a new destination while already under way.* Suggestion: "New course plotted. Adjusting
  heading."
- [ ] `course_stopped_01`: *the course is called off; the ship comes to a stop.* Suggestion: "Course cancelled. Bringing
  us to a stop."
- [ ] `orbit_01`: *arrived in orbit of a planet or moon.* Suggestion: "We have reached orbit. The shuttle is ready in
  the bay."
- [ ] `region_found_01`: *the ship's scan found somewhere new to land.* Suggestion: "Surface scan complete. A new
  landing site has been marked."

**Going down and coming back**

- [ ] `deploy_01`: *the shuttle is leaving the ship.* Suggestion: "Bay doors open. Shuttle away."
- [ ] `docked_01`: *the shuttle is back in the bay.* Suggestion: "Shuttle docked. Bay secure." (The old line sent
  people to the briefing room for a debrief; there is no debrief now.)

**On the ground** (a site with a facility and its terminal)

- [ ] `arrival_01`: *landed, with a map.* Suggestion: "Touchdown. Site map is on your devices."
- [ ] `arrival_no_map_01`: *landed, no map.* Suggestion: "Touchdown. We have no map of this site."
- [ ] `power_out_01`: *someone found the terminal has no power.* Suggestion: "No power at the terminal. Find the
  building's breaker."
- [ ] `download_started_01`: Suggestion: "Download started. Stay with the terminal until it finishes."
- [ ] `download_done_01`: Suggestion: "Download complete. Take the drive back to the shuttle."

**Leaving**

- [ ] `launch_01`: *the launch countdown has started.* Suggestion: "Launch sequence started. Everyone aboard the
  shuttle."
- [ ] `recovered_01`: *left with the data.* Suggestion: "Lift-off. The data is aboard." (The old lines thanked you on
  the Company's behalf; it is your ship talking now.)
- [ ] `not_recovered_01`: *left without the data.* Suggestion: "Lift-off. We did not get the data."
- [ ] `left_behind_01`: *the shuttle has gone without you.* Suggestion: "The shuttle has gone. You are still on the
  surface."

**For later: CIRRA's contracts**

- [x] `orders_01` (recorded): *a briefing has come in.* It no longer plays on its own. It will come back for CIRRA's
  contracts, which are optional offers, so the recorded take ("New deployment orders have been received. Report to the
  briefing room.") will need replacing: say that a contract offer is waiting, not that there are orders.

### Contract briefings (`Assets/Audio/Briefing/...`): for later

Briefings now play only for **CIRRA's contracts**, which come with the next part of the rework. They are still put
together from recorded pieces, so they can name the place they are about. **Don't record new pieces yet**: the
contract briefing script (`Assets/Data/briefing.json`) will be rewritten first, for contracts instead of orders. Then
this list will be final. What is here now:

**Phrases** (`Briefing/Phrases/<folder>`): placeholders, never recorded, written for the old orders. They will change
with the script.

- [ ] `begin`, `destination`, `site`, `conditions`, `objective`, `end`, `map_given`, `map_missing`

**Words** (`Briefing/Words/<folder>`): the words planet and site names are made of. Names come from
`Assets/Data/universe.json` now, so these are the words it can produce:

- There already (placeholders): `kepler`, `polar`, `research`, `facility`, `site`, `north`, `cryosphere`, `minus`, `degrees`, `wind`,
  `metres`, `per`, `second`, `visibility`, `poor`, `fair`, `low`
- New, when the script is ready: `good` (visibility); the moon prefix said letter by letter, `l` and `v` ("L V four
  two six"); and the site words: `archipelago`, `basin`, `caldera`, `canopy`, `coast`, `crash`, `crater`, `delta`, `dune`,
  `equatorial`, `field`, `flats`, `glacial`, `highlands`, `ice`, `islands`, `lava`, `lowlands`, `mining`, `outpost`,
  `plains`, `plateau`, `relay`, `ridge`, `rift`, `salt`, `sea`, `sector`, `shard`, `shelf`, `shipyard`, `signal`, `source`,
  `south`, `station`, `survey`, `terminator`, `valley`, `waste`
- Adding a name to `universe.json` (a catalogue, an area, a kind of site) adds words to this list.

**Numbers** (`Briefing/Numbers/<folder>`): placeholders. A planet's numeral is said as a number ("Kepler two one seven,
four"); site and moon numbers are said a figure at a time.

- [ ] `0` to `19`, the tens (`20` ... `90`), `hundred`

---

## Part 2: Textures (PBR)

### Where things stand

Almost everything in the game is drawn in **flat colours** today. Only one texture set is in: `snow_02`, laid on the
ground of every landing site (tinted to the world's own ground colour). Each set below gets wired in when it arrives --
a few lines of code per surface -- so they can come in any order. The **first ten** (just below) change the most of what
you see.

### What each set needs

| Map | Needed? |
|---|---|
| Base colour (albedo) | always |
| Normal, **OpenGL** style | always |
| Roughness | always |
| Metallic | metal surfaces only |
| Ambient occlusion (AO) | always |
| Height | optional, nice for later |

- **2K** for anything tiled across a level (ground, walls, floors), **1K** for small props. **Seamless / tiling**: the
  game tiles them by real size across every surface rather than wrapping them onto one model.
- **Free sources (CC0):** [ambientCG](https://ambientcg.com), [Poly Haven](https://polyhaven.com).
- **Where and what to call them:** `Assets/Textures/<set>/<set>_BaseColor.png`, `<set>_Normal.png`, `<set>_Roughness.png`,
  `<set>_Metallic.png`, `<set>_AO.png`. For example `Assets/Textures/rock_ground/rock_ground_Normal.png`.
- **PNG or JPG** both work. An **EXR** normal map has to be saved out as PNG first, as **Non-Color** data (in Blender:
  open it, set the colour space to Non-Color, save as PNG 8-bit); a normal map that looks greyish-purple rather than
  light blue is in the wrong colour space and tilts the lighting.
- **Neutral, light colours** for anything the game tints (marked *tinted*): the ship's hull takes the campaign's colours,
  outposts their owner's paint, a world's ground its own colour. A pale grey version of the material is best.
- Base colour, normal and roughness are used; metallic and AO are not read yet -- keep them in the folder for later.

### The first ten

1. `regolith`: pale dust and pebbles (barren worlds, airless moons -- the most common ground) *tinted*
2. `rock_ground`: grey-brown broken rock and grit (rocky worlds) *tinted*
3. `rock_cliff`: big rock faces (the rock round every site, boulders, outposts' hills) *tinted*
4. `concrete_slab`: weathered poured concrete in large squares (Kestrel's and every outpost's slab, the pad)
5. `asphalt`: road surface (the street at Kestrel and the outposts)
6. `building_cladding`: corrugated or paneled metal siding, weathered (every building's outside) *tinted*
7. `hull_plating`: large plated panels (the ship's outside) *tinted*
8. `deck_plate`: tread / diamond plate (the ship's floors, the boarding ramp, gantries)
9. `ship_panel`: sci-fi wall paneling, clean-ish (the ship's rooms) *tinted*
10. `facility_floor`: worn concrete or industrial vinyl (inside every site building)

### 1. Ground of each kind of world (`universe.json` biomes, `"site"` -> `"surface"`)

- [x] `snow_02`: packed, wind-blown snow (frozen worlds)
- [ ] `regolith`: pale dust and pebbles (barren worlds and airless moons)
- [ ] `rock_ground`: grey-brown broken rock and grit (rocky worlds)
- [ ] `sand`: rippled sand (desert worlds)
- [ ] `basalt`: black volcanic rock, a little ash (volcanic worlds)
- [ ] `wet_rock`: dark wet stone and shingle (oceanic worlds, their islands)
- [ ] `grass_dirt`: rough grass over dirt (temperate worlds)
- [ ] `jungle_floor`: leaf litter and mud (jungle worlds)
- [ ] `toxic_crust`: stained, crusted mineral ground (toxic worlds)
- [ ] `storm_flats`: hard wind-scoured ground (storm-dominated worlds)
- [ ] `crystal_ground`: glassy, faceted mineral (crystalline worlds)
- [ ] `rock_cliff`: large rock faces (cliffs, boulders, hills)
- [ ] `ice_cliff`: blue-white ice faces (frozen worlds' cliffs, later)

When the ground stops being flat (see NEXT_STEPS.md), a **second ground set per world** for slopes (rock showing
through on steep ground) makes the biggest difference; the rock sets above double as that.

### 2. Kestrel Station and the outposts

- [ ] `concrete_slab`: the slab everything stands on, and the pad
- [ ] `asphalt`: the street
- [ ] `pavement`: paving slabs or kerbed concrete (the pavements)
- [ ] `blast_wall`: heavy cast concrete, stained (the walls round the pad, Kestrel's port wall)
- [ ] `building_cladding`: corrugated or paneled metal siding *tinted*
- [ ] `shed_roof`: ribbed metal roofing
- [ ] `painted_metal`: painted steel *tinted* (freight containers, tanks, the gantry crane)
- [ ] `pipe_steel`: dull steel (pipes, trestles, lamp posts, masts)
- [ ] `fence_mesh`: chain-link or welded mesh, **with transparency** (the fences)
- [ ] `habitat_shell`: pressurised module skin, panel seams *tinted*

### 3. Facility interiors (every site building)

- [ ] `facility_floor`: worn concrete or industrial vinyl
- [ ] `facility_wall`: painted plaster or paneling, institutional
- [ ] `facility_ceiling`: ceiling tiles or bare concrete
- [ ] `facility_stairs`: concrete or metal treads with grip plate
- [ ] `facility_door`: painted metal door
- [ ] `metal_grate`: ducts and vents
- [ ] `shelf_steel`: painted steel (shelves, cabinets)
- [ ] `worktop`: laminate or steel (benches, tables)
- [ ] `concrete_pillar`
- [ ] `plant_grime`: dirtier concrete or metal (plant rooms)

### 4. The ship

- [ ] `hull_plating`: large plated panels, the outside *tinted*
- [ ] `hull_dark`: dark metal (frames, trim, ribs, the engine block)
- [ ] `deck_plate`: diamond / tread plate (floors, the ramp)
- [ ] `ship_panel`: painted wall paneling, clean-ish *tinted*
- [ ] `bay_panel`: heavier, more worn panels (the shuttle bay's walls and deck)
- [ ] `locker_metal`: lockers, the loadout locker, cabinets
- [ ] `fabric_cushion`: chairs, bunk mattresses
- [ ] `tabletop`: the galley counter, the navigation table
- [ ] `cockpit_glass`: tinted glass, faint scratches (the cockpit's windows, from outside)

### 5. Wrecks (being built next)

- [ ] `hull_scorched`: burnt, blistered hull plating (a recent crash)
- [ ] `hull_weathered`: faded, pitted plating with streaks (an old wreck) *tinted*
- [ ] `cut_metal`: plate with torch-cut edges (a picked-over wreck)
- [ ] `debris_mixed`: twisted metal and cable bundles (small pieces)

### 6. Vehicles and props

- [ ] `shuttle_hull`: painted metal and trim
- [ ] `bay_door`: the shuttle bay's doors
- [ ] `ammo_crate`: olive painted metal
- [ ] `console_casing`: dark plastic or metal (terminals, consoles)
- [ ] `cargo_crate`: salvage crates and pallets

### 7. Player and weapons

- [ ] `suit_fabric`: heavy, padded environment-suit material
- [ ] `helmet_composite`: hard plastic or composite
- [ ] `glove_rubber`: rubber or leather
- [ ] `gun_metal`: dark, worn steel
- [ ] `weapon_polymer`: grips and bodies

### 8. Creature and nest (organic)

- [ ] `creature_skin`: tiling wet flesh or hide
- [ ] `nest_flesh`: tiling membrane or veined tissue
- [ ] `egg_sac`: translucent-looking membrane, subtle veins

### 9. Decals: images with transparency, not full PBR sets

- [ ] `hazard_stripes`: yellow and black
- [ ] `grime_streaks`: leaks and dirt to break up walls
- [ ] `scorch_marks`: burn marks (wreck sites, fights)
- [ ] `tyre_tracks` / `footprints`: for ground near outposts and camps
- Signs and stencils are drawn by the game (the CIRRA mark, outpost names, building names) -- no textures needed.

### Not needed

Screens, signs, the site map and tracker, briefing slides, the navigation map, the sky, planets and moons (from space and
on the map), the star, snowfall, dust and particles are all drawn by code.
