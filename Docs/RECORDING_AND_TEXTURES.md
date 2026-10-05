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

### What each set needs

| Map | Needed? |
|---|---|
| Base colour (albedo) | always |
| Normal, **OpenGL** style | always |
| Roughness | always |
| Metallic | metal surfaces only |
| Ambient occlusion (AO) | always |
| Height | optional, nice for later |

- **2K** for anything tiled across a level (ground, walls, floors), **1K** for small props. **Seamless / tiling**
  (surfaces are tiled across walls and floors, not wrapped onto one model).
- **Free sources (CC0):** [ambientCG](https://ambientcg.com), [Poly Haven](https://polyhaven.com).
- **Where and what to call them:** `Assets/Textures/<set>/<set>_BaseColor.png`, `<set>_Normal.png`, `<set>_Roughness.png`,
  `<set>_Metallic.png`, `<set>_AO.png`. For example `Assets/Textures/snow_ground/snow_ground_Normal.png`.
- **PNG or JPG** both work. An **EXR** normal map has to be saved out as PNG first, as **Non-Color** data (in
  Blender: open it, set the colour space to Non-Color, save as PNG 8-bit); a normal map that looks greyish-purple
  rather than light blue is in the wrong colour space and will tilt the lighting.
- **In game today:** base colour, normal and roughness are used, tiled by real size from every side. Metallic and AO
  are not read yet; keep them in the folder for later. `snow_02` on the snow ground is the first set in.

Listed most important first within each area.

### 1. Landing sites: the ground of each kind of world

A world's kind (its biome, in `Assets/Data/universe.json`) names the set its ground is to be laid with (`"site"` ->
`"surface"`). Landing sites do not take their world's look yet -- every site is still snow -- that is the next part of the
rework; only frozen worlds name a set so far (`snow_02`).
Each of these is one ground set; a rock set for cliffs and boulders to go with each would be nice later.

- [x] `snow_02`: packed, wind-blown snow (frozen worlds)
- [ ] `rock_ground`: grey-brown broken rock and grit (rocky worlds)
- [ ] `regolith`: pale dust and pebbles (barren worlds and airless moons)
- [ ] `sand`: rippled sand (desert worlds)
- [ ] `basalt`: black volcanic rock, a little ash (volcanic worlds)
- [ ] `wet_rock`: dark wet stone and shingle (oceanic worlds, their islands)
- [ ] `grass_dirt`: rough grass over dirt (temperate worlds)
- [ ] `jungle_floor`: leaf litter and mud (jungle worlds)
- [ ] `toxic_crust`: stained, crusted mineral ground (toxic worlds)
- [ ] `storm_flats`: hard wind-scoured ground (storm-dominated worlds)
- [ ] `crystal_ground`: glassy, faceted mineral (crystalline worlds)
- [ ] `rock_cliff`: dark, frost-dusted rock (the ring round the site, boulders)
- [ ] `concrete_pad`: weathered concrete (the landing pad)
- [ ] `building_cladding`: corrugated or paneled metal siding, weathered (building outsides)
- [ ] `painted_metal`: painted steel, one you can tint or several colours (freight containers, fuel tanks)
- [ ] `pipe_steel`: dull steel (overhead pipes, supports, lamp poles)

### 2. Facility interiors

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

### 3. The ship

The crew's own small ship (one deck: cockpit, ops room, crew section, shuttle bay, engine room). Its outside is
painted in the campaign's three colours, so the hull sets should be **light and neutral** (greyish white): the game
tints them.

- [ ] `deck_plate`: diamond / tread plate (floors)
- [ ] `ship_panel`: painted sci-fi wall paneling, clean-ish
- [ ] `bay_panel`: heavier, more worn panels (the shuttle bay's walls and deck)
- [ ] `hull_dark`: dark metal (frames, trim, ribs)
- [ ] `hull_plating`: large plated panels, neutral, to be tinted (the outside)
- [ ] `locker_metal`: lockers, the loadout locker, cabinets
- [ ] `fabric_cushion`: chairs, bunk mattresses
- [ ] `tabletop`: the galley counter and tables

### 4. Vehicles and props

- [ ] `shuttle_hull`: painted metal and trim
- [ ] `bay_door`: the shuttle bay's doors
- [ ] `ammo_crate`: olive painted metal
- [ ] `console_casing`: dark plastic or metal (terminals, consoles)

### 5. Player and weapons

- [ ] `suit_fabric`: heavy, padded environment-suit material
- [ ] `helmet_composite`: hard plastic or composite
- [ ] `glove_rubber`: rubber or leather
- [ ] `gun_metal`: dark, worn steel
- [ ] `weapon_polymer`: grips and bodies

### 6. Creature and nest (organic)

- [ ] `creature_skin`: tiling wet flesh or hide
- [ ] `nest_flesh`: tiling membrane or veined tissue
- [ ] `egg_sac`: translucent-looking membrane, subtle veins

### 7. Decals: images with transparency, not full PBR sets

- [ ] `hazard_stripes`: yellow and black
- [ ] `grime_streaks`: leaks and dirt to break up walls
- [ ] Signage and stencils (deck numbers, "LOADOUT", ...): hold off until the wording is decided.

### Not needed

Screens, the site map and tracker, briefing slides, the system map, the sky, planets (from space), snowfall and
particles are all drawn by code.
