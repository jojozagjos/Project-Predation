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
  minute or more; arriving scans the body and finds places to land, chosen on the globe; everybody sees what the
  others are pointing at. Space out of the windows is the system as it is: other worlds as points of light or discs at
  their real size, the sun setting behind the planet below in orbit.
- **Starting landed at the hub** (ADR-130) on the settled home world: the ship on its gear on a pad, under that world's
  sky at its time of day; taking off and setting down have their own cinematics. Crossing to other systems needs an
  upgraded drive.
- **The crew's own ship** (ADR-127): small, one deck, a shuttle bay; its outside follows the campaign's colours and
  its drive and sensor upgrades.
- **The title and lobby** reworked round campaigns; no host migration (the host leaving ends the game, ADR-126).
- **Briefings only when there is one** (ADR-126): they come back with contracts.
- Before the rework and still there: the player, weapons and items, the creatures (AI.md), the facility sites and
  their data-recovery objective, the support drone, cinematics and their editor, proximity voice, the model editor.

## Next, in order

1. **Landing regions that look like their world**: the ground's texture and colour, the rock, the sky, fog and light
   by where the region is on its planet and the time of day there, weather (snow, rain, dust, ash) that changes; more
   than snow. And more kinds of place to explore than a facility: wrecks, camps, a signal source, a tower, a cave mouth,
   some quiet and some not.
2. **What there is to do there**: salvage and components to carry back, points of interest found by exploring, the
   log filled in by what is found, a region remembered as it was left (what was taken, what was opened).
3. **The economy and the shipyard**: credits for what is brought back; the hub to sell salvage at, buy upgrades
   (the drive -- the first upgrade opens other systems --, sensors, storage, the ship's size) and change the ship's
   colours; the ship gaining sections as it grows. Walking off the ship at the hub. What the hub is in the story.
4. **The ship's log**: everything found, kept and shown; entries marked and pinned.
5. **CIRRA's contracts**: optional work offered over the intercom, briefed at the navigation table, paying credits
   and components.
6. **A crew wipe** that costs what was carried and some credits, never the ship or what has been found.
7. **The tutorial**: a short story-driven prologue ([TUTORIAL_CHECKLIST.md](TUTORIAL_CHECKLIST.md)).
8. Beyond the slice: more biomes and places, the story's discoveries, the endgame.

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
