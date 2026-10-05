# Project Predation: recordings and textures to get

A checklist. Tick things off (`[x]`) as you add them.

---

## Part 1: Voice recordings

### How to record

- **Format:** `.wav`, mono, 16-bit, 44.1 or 48 kHz. Your `orders_01/output.wav` is exactly right.
- **Any file name ending `.wav`** works. With several takes in one folder, the first by name is used.
- **Record dry:** no music, no reverb, and about a quarter of a second of silence at each end. The game adds the
  intercom/radio sound itself.
- **Delete `placeholder.wav`** from a folder once your recording is in it.
- **Intercom subtitles** go in `Assets/Data/intercom.json`, in the `"subtitle"` next to the folder's name.
- **A second take for a moment:** make a `<moment>_02` folder and add a second entry for it in `intercom.json`.
  The game picks one, the same on every player's machine. One take per moment is enough to start.

### Intercom: the ship's voice (`Assets/Audio/Intercom/<folder>`)

**On the ship**

- [x] `orders_01`: *new orders have come in*
  - Take 1 (recorded): "Attention all crew. New deployment orders have been received. Report to the briefing room."
  - Take 2 (`orders_02`): "Incoming transmission from the Company. All crew to the briefing room."
- [ ] `orbit_01`: *the ship has arrived over the site*
  - Take 1: "Burn complete. We are holding orbit over the site. The shuttle is ready in the hangar."
  - Take 2: "We have reached orbit. Board the shuttle when ready."
- [ ] `deploy_01`: *the shuttle is leaving the ship*
  - Take 1: "Bay doors open. Shuttle away. Beginning descent to the surface."
  - Take 2: "Shuttle is clear of the ship. Descent under way."
- [ ] `docked_01`: *the shuttle is back in the hangar*
  - Take 1: "Shuttle docked. Hangar secure. Report to the briefing room for debrief."
  - Take 2: "Docking complete. Welcome back aboard."

**On the planet**

- [ ] `arrival_01`: *landed, with a map*
  - Take 1: "Touchdown confirmed. Site map data is loaded. Locate the terminal and retrieve the data."
  - Take 2: "You're on the ground. Map data is on your units. Find the terminal."
- [ ] `arrival_no_map_01`: *landed, no map*
  - Take 1: "Touchdown confirmed. We have no map data for this site. You will have to find the terminal yourselves."
  - Take 2: "You're on the ground. Be advised: there is no layout on file for this facility."
- [ ] `power_out_01`: *someone found the terminal has no power*
  - Take 1: "The terminal has no power. Locate the building's breaker panel and restore it."
  - Take 2: "No power at the terminal. Check the breakers."
- [ ] `download_started_01`
  - Take 1: "Connection established. Download in progress. Stay with the terminal until it completes."
  - Take 2: "We're receiving. Hold that position until the transfer is finished."
- [ ] `download_done_01`
  - Take 1: "Download complete. Take the drive and return to the shuttle."
  - Take 2: "Transfer finished. Get the drive back to the shuttle."

**Leaving**

- [ ] `launch_01`: *the launch countdown has started*
  - Take 1: "Launch sequence initiated. All crew, get aboard the shuttle now."
  - Take 2: "The shuttle is preparing to lift off. Anyone not aboard will be left behind."
- [ ] `recovered_01`: *left with the data*
  - Take 1: "Lift-off confirmed. Data recovered. Good work."
  - Take 2: "Shuttle is clear of the surface with the data aboard. The Company thanks you."
- [ ] `not_recovered_01`: *left without the data*
  - Take 1: "Lift-off confirmed. The data was not recovered. This will be noted."
  - Take 2: "Shuttle is clear of the surface. No data aboard. Deployment failed."
- [ ] `left_behind_01`: *the shuttle has gone without you*
  - Take 1: "The shuttle has departed. You have been left on the surface."
  - Take 2: "Shuttle away. We are unable to return for you."

### Briefing voice-over (`Assets/Audio/Briefing/...`)

The briefing is put together from these pieces, so it always names the site it's showing. Record each on its own,
evenly paced, so they join cleanly. The phrase wording is a placeholder in `Assets/Data/briefing.json`; change it
there first if you want different words, then record to match.

**Phrases** (`Briefing/Phrases/<folder>`)

- [ ] `begin`: "Deployment briefing."
- [ ] `destination`: "Destination:"
- [ ] `site`: "Site:"
- [ ] `conditions`: "Surface conditions:"
- [ ] `objective`: "Objective: locate the terminal, download the data, and bring the drive back to the shuttle."
- [ ] `end`: "Deploy when ready."
- [ ] `map_given`: "A map of the site is on file."
- [ ] `map_missing`: "There is no map of the site on file."

**Words** (`Briefing/Words/<folder>`, one word each): the parts of planet and site names and the weather. Adding a
word to `Assets/Data/sites.json` means recording it here too.

- [ ] `kepler`
- [ ] `polar`
- [ ] `research`
- [ ] `facility`
- [ ] `site`
- [ ] `north`
- [ ] `cryosphere`
- [ ] `minus`
- [ ] `degrees`
- [ ] `wind`
- [ ] `metres`
- [ ] `per`
- [ ] `second`
- [ ] `visibility`
- [ ] `poor`
- [ ] `fair`
- [ ] `low`

**Numbers** (`Briefing/Numbers/<folder>`, said on their own). 91 is said "ninety" + "one"; a site number like 06 is
said a figure at a time, "zero" + "six".

- [ ] `0` to `9`: zero, one, two, three, four, five, six, seven, eight, nine
- [ ] `10` to `19`: ten, eleven, twelve, thirteen, fourteen, fifteen, sixteen, seventeen, eighteen, nineteen
- [ ] `20`, `30`, `40`, `50`, `60`, `70`, `80`, `90`: twenty, thirty, forty, fifty, sixty, seventy, eighty, ninety
- [ ] `hundred`

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

### 1. Site: the snow planet (seen most)

- [ ] `snow_ground`: packed, wind-blown snow
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

- [ ] `deck_plate`: diamond / tread plate (floors)
- [ ] `ship_panel`: painted sci-fi wall paneling, clean-ish
- [ ] `hangar_panel`: heavier, more worn panels (hangar walls and deck)
- [ ] `hull_dark`: dark metal (frames, trim, ribs)
- [ ] `hull_exterior`: large plated panels (the ship seen outside in cutscenes)
- [ ] `locker_metal`: lockers, the loadout locker, cabinets
- [ ] `fabric_cushion`: chairs, bunk mattresses
- [ ] `tabletop`: mess and briefing tables

### 4. Vehicles and props

- [ ] `shuttle_hull`: painted metal and trim
- [ ] `bay_door`: the hangar's bay doors
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

Screens, the site map and tracker, briefing slides, the sky, planets, snowfall and particles are all drawn by code.
