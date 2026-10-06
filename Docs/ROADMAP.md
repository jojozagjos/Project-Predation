# What is left

The game is being reworked into an open exploration campaign ([REWORK_DESIGN.md](REWORK_DESIGN.md)). This is how far
it has got, in the order the design's vertical slice puts things. Updated 2026-10-05.

## Built

- **Campaigns** (ADR-124): begun with a name and a seed or carried on; saved by the host (a hand save and an
  autosave, written safely); sent to everybody else. Continue, New campaign and Load campaign on the title.
- **A universe from a seed** (ADR-124): star systems, planets and moons, worlds combined from biome, air, terrain,
  weather, settlement and specials (Assets/Data/universe.json); landing regions, some charted, the rest to be found.
- **The navigation map and travel** (ADR-125, ADR-129): the navigation table in the ops room opens a 3D map at three
  scales -- the galaxy, as far as anybody scrolls; a system; a body's globe with its landing areas -- with right-drag to
  move, left-click to pick, and the wheel going from one scale to the next. A course is flown, round the star rather
  than through it, with time to walk the ship, redirected or called off part way; other systems are crossed to in a
  minute or more; arriving scans the body and finds places to land, chosen on the globe. The map is one for the whole
  crew: whoever moves it moves it for everybody (ADR-133). It says what each thing is, what CIRRA has on file about it,
  where the ship is, and the one place it is going. Space out of the windows is the system as it is (ADR-132): planets
  and moons drawn as the worlds they are, the one just left falling away behind rather than vanishing, the ship turning
  slowly onto its heading, the sun eclipsed by what is in front of it.
- **Kestrel Station** (ADR-130, ADR-131): the campaign begins outside the ship on its pad at Kestrel, the CIRRA station
  on the home world -- Hangar Row, Operations Hall, Shipworks, Salvage Intake, Crew Services, the Research Annex and the
  Navigation Relay, signed with the CIRRA mark -- under that world's sky at its time of day, lit at night, nothing on it
  flickering (ADR-132). Up the stair and in through the boarding door. Taking off and setting down have their own
  cinematics. Kestrel stays built by hand: it is where the tutorial and the story begin.
- **Outposts on every other world** (ADR-133, ADR-135): each system's outpost planned from its seed -- an operations block
  with its name on it across the street from the stair, and sheds, blocks, habitats, tank farms, container yards and comms
  masts round the pad, each outpost its own -- and run by an owner: CIRRA, independent, industrial, or nobody any more.
- **Flying** (ADR-135): held in orbit, turning and swinging round to set out, leaving a world gently enough to watch it
  fall away, settling into orbit on arrival; legs that fold and a boarding stair that is the ship's.
- **Places on planets by kind** (ADR-133): every landing area leads somewhere of its own -- a research facility, a
  remote station, a survey site, a wreck, a signal source -- on its world's own ground, under its sky, with snow only
  where it is cold enough and there is air to carry it.
- **Plotting a course and setting out** (ADR-131): plotted on the map, set out on from the helm; no cinematic leaving
  orbit until the instant-travel drive. The charts and a crossing's reach grow with the sensors and the drive.
- **The crew's own ship** (ADR-127): small, one deck, a shuttle bay; its outside follows the campaign's colours and
  its drive and sensor upgrades.
- **The title and lobby** reworked round campaigns; no host migration (the host leaving ends the game, ADR-126).
- **Briefings only when there is one** (ADR-126): they come back with contracts.
- Before the rework and still there: the player, weapons and items, the creatures (AI.md), the facility sites and
  their data-recovery objective, the support drone, cinematics and their editor, proximity voice, the model editor.

## Next, in order

1. **Landing regions that look like their world, and what waits at each kind of place**: the ground's colour, the rock
   and the sky are the world's now (ADR-133); still to come, ground textures other than the snow set and weather that
   changes (rain, dust, ash). The kinds of place exist -- what is at a wreck, a signal source, a survey site or a remote
   station is the story's, and waits on the lore. Caves, camps and towers after.
2. **What there is to do there**: salvage and components to carry back, points of interest found by exploring, the
   log filled in by what is found, a region remembered as it was left (what was taken, what was opened).
3. **Kestrel's interiors**: Operations Hall (contracts), Shipworks (upgrades), Salvage Intake (selling), Crew Services
   -- buildings to walk into, each a module of its own.
4. **The economy and the shipyard**: credits for what is brought back; the hub to sell salvage at, buy upgrades
   (the drive -- the first upgrade opens other systems --, sensors, storage, the ship's size) and change the ship's
   colours; the ship gaining sections as it grows. What other outposts are in the story.
5. **The ship's log**: everything found, kept and shown; entries marked and pinned.
6. **CIRRA's contracts**: optional work offered over the intercom, briefed at the navigation table, paying credits
   and components.
7. **A crew wipe** that costs what was carried and some credits, never the ship or what has been found.
8. **The tutorial**: a short story-driven prologue ([TUTORIAL_CHECKLIST.md](TUTORIAL_CHECKLIST.md)).
9. Beyond the slice: more biomes and places, the story's discoveries, the endgame.

## Still missing from what exists

- **Melee or shoving.** Nothing to do about something close except shoot it.
- **Nameplates.** You cannot tell who anybody is at a distance.
- **Reconnecting.** A player who drops is gone; they can join the campaign again.
- **Real sounds and voice.** Most sounds are synthesised placeholders and the intercom has placeholders to record
  over ([RECORDING_AND_TEXTURES.md](RECORDING_AND_TEXTURES.md)).
- **Real models for items.** Items are coloured shapes; weapons are authored.

## Keeping the lore in mind

Docs/Project_Predation_Lore_Reference.md shapes these choices. Nothing here writes lore, and any design choice the lore
influences is asked about first.
