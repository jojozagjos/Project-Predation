# Fix list

Everything raised in play on 2026-10-19, in one place so nothing is lost. Each item: what you said, what is
behind it, and where it stands. Ticked when done and pushed; the commit is noted.

Status: **Open**, **Doing**, **Done** (commit), **Planned** (a bigger piece that belongs to a phase of
[MASTER_PLAN.md](MASTER_PLAN.md)).

---

## Feel and performance

| # | You said | What is behind it | Status |
|---|---|---|---|
| 1 | The game feels choppy though it says about 111 FPS | Found: every 30 frames the picture's brightness was read back from the graphics card for tuning, and reading anything back makes the game wait for the card to finish: a held frame of 30 to 70 ms three or four times a second. Now only read when asked (eye_measure). Long frames over 800 frames: 47 before, 2 after | Done |
| 2 | Jungle about 53 FPS: optimise trees and grass | Every leaf card of every near tree drawn, far trees as many triangles as near; grass a mesh rebuilt as you walk. Fewer, cheaper draws at distance (impostors), grass drawn by instancing | Open |
| 3 | A big pause setting out from an outpost | The takeoff cinematic shows a copy of the outpost, and it was built the moment the takeoff began: the whole outpost in one frame. Now made ready, hidden, while the ship stands there | Done |
| 4 | Two different loading screens; want one | The real loading screen now looks like the boot cards (black, what is loading low on the right, a hairline filling along the foot) and the pretend loading card that followed it is gone: loading runs straight into "made by" | Done |

## Grass, trees, ground

| # | You said | What is behind it | Status |
|---|---|---|---|
| 5 | Rework the grass so it can always be seen | The tufts were much darker than the pale ground between them and thinned fast past 25 m, so the field turned to bare lawn. Now the ground under grass is a deeper, more even green, the far tufts are wider to cover for being fewer, and grass reaches 120 m instead of 90 | Done |
| 6 | Pine trees from a distance (from below) look bad | A pine was a trunk with whorls of needle cards and nothing behind them: the sky through it from below and from any way off. Each now has a solid dark cone inside the whorls, the cards its outline | Done |
| 7 | Details right next to the site, then nothing further out | Rocks, ice, crystal and basalt were laid only within 84 m of the site. They now go as far as the trees (200 m), and past that a far band of the bigger ones, a few faces each, goes on to 900 m as the far trees do | Done |
| 8 | Footprints: the rectangle of the print can be seen | The print was shaded by a normal worked out at each point, painted a colour worked out at each point, and weathered less than the ground; the ground blends its corners' normals and colours across each triangle. The print now takes them exactly as the ground is drawn, and weathers as it does. To check in play | Done (check) |
| 9 | Rain and puddles do not look real; should all the ground be wet after rain? | Yes. The ground is now soaked evenly while it rains and dries in patches after. Puddles were small, many and pale with the sky (read as snow); now a few metres across and few, only once it is soaked, on truly flat ground, growing while it rains and shrinking as it dries: dark, flat-faced, a darker rim round each | Done |

## Water

| # | You said | What is behind it | Status |
|---|---|---|---|
| 10 | Water splash particles look bad | The splash is small solid boxes from the chip system; water wants soft droplets and a ring on the surface | Open |
| 11 | Teleported back in water; should be able to swim out a fair way | Swimming: past about 1.35 m deep you float with your eye just over the surface and swim (shift faster, on stamina); jump rises, crouch dives; pushing into a bank you climb out onto it. Out as far as anywhere else at a site (the comms range). Acid keeps its wading limit. Still to come: a swimming pose for the body others see | Done (pose to come) |
| 12 | Footsteps in water wrong; walking-in-water sounds bad | Ground footsteps play under the splash; water wants wading sounds by depth, no ground step | Open |
| 13 | No ocean or water sounds | The shore loop is a made sound and only at the shore; recorded surf, lapping, wind over water (needs your yes to download) | Open |
| 14 | Water should have waves | The surface is flat with moving normals; real swell needs the mesh moved (and things floating on it) | Open |
| 15 | Underwater: a camera effect, sounds muffled; seeing clearly under the water and the edge of the map is wrong | Under water now: murk all round in the water's own colour (a few metres to see by, more the clearer the water), no sky, the surface overhead bright and wavering; everything in the world heard dull and quieter (the radio and the interface clear). Deep water is fully opaque from above | Done |
| 16 | Water and lava look noisy from far away | Their ripples, rain rings, cracks and churn were drawn however small on the screen, so far off they glittered. Each now fades as it gets finer than a pixel or two; far lava is dark crust with patches of dull red glow, far water calm and reflecting | Done |

## Worlds being different

| # | You said | What is behind it | Status |
|---|---|---|---|
| 17 | The desert's sand tornadoes do not look good | Dust devils as sprites looked like blobs and leaned tens of metres off their bases (the wind read in the wrong units). Removed until they can be done properly | Done (4fa60db) |
| 18 | Signs of the organism: wherever a creature is, there should always be signs, more with a harder situation | Creatures come to every mission site, so every one now has signs: faint near home, plainer further out. Sites have no difficulty of their own yet; distance from home stands in for it until they do | Done |
| 19 | Different sky colours, sunsets and sunrises by where you are | The sky's colour from the star and the air: thick air, thin air, dust, a red or blue star | Planned (Phase 2, W4) |
| 20 | Sites feel the same: each needs its own obstacles and challenges (water swimming, toxic radiation, lava...) | Hazards by world: toxic air and radiation, heat, cold, storms, water; what the suit must deal with | Open |
| 21 | Water worlds (like Subnautica), and larger maps for larger facilities | Planned (Phase 12, and the facility rework, Phase 4) | Planned |
| 22 | Facilities built round or with lava and water; destroyed or working facilities (holes in walls) | Planned (Phase 4 facility types and conditions) | Planned |

## Facilities

| # | You said | What is behind it | Status |
|---|---|---|---|
| 23 | Light flickering through a door, appearing and vanishing | A lamp's light let through a doorway (its "spill") switching on and off as the door's state is looked up | Open |
| 24 | Opening a door, the shadow shakes as the lighting updates | A lamp's shadow map redrawn as the door moves, at a coarse resolution, so its edge crawls | Open |

## Interface

| # | You said | What is behind it | Status |
|---|---|---|---|
| 25 | Landing title card: time, temperature, wind and visibility hard to tell apart | Were all on one line; now each on a line of its own, set a little apart from the place names | Done |
| 26 | Snow on the ground going back and forth fast | Blown snow placed by the clock times the gusting wind: each gust threw it metres. Now moved a frame at a time; and at the right speed (the wind is metres a second) | Done (4fa60db) |
| 27 | A straight line across the sea to the horizon: shading error? | A bug: the cliffs round a site kept 15% of their height on past their back for ever, a step in the land along straight lines out from the site; over low ground at a coast it stood out of the sea as a dead straight strip. Now gone back to the land | Done |

## Raised later the same day

| # | You said | What is behind it | Status |
|---|---|---|---|
| 28 | The light through a door: the door was shut | A failing lamp beyond it was left out while dark, gave its shadow away, and came back lit with nothing in its way, through the door, until its shadow was drawn again. Dark, it now keeps its shadow | Done (check) |
| 29 | Footprints still look bad from the side | Each print is still a card a few millimetres over the ground; seen low, its edges show | Open |
| 30 | Snow worlds: the frozen falls clip into things and look bad | Laid on a straight line from the cliff's lip to its foot, the fall went into the rock where the face bulged and stood off it where it fell back. Each column now follows the face as it is at each height | Done |
| 31 | The organism's signs had missing faces | Each ring of a branch was turned square to it afresh at each step, and flipped a quarter turn where a root dipped steeply: the length between twisted flat into a blade, faces inside out. The rings are now carried along the branch. And nothing scattered lies across them any more (rocks were laid on their roots) | Done |
| 32 | A lag spike choosing where to land on the map | Something built on the spot when a landing area is picked | Open |
| 33 | On a moon with no air, wind should not be heard | Right. Out on a world without air there is now no wind and the ears are in vacuum; inside its buildings and the shuttle, which have air, sound carries but no wind is heard through the walls | Done |
| 34 | Should the organism's signs move or breathe; more kinds of them? | Yours to decide (it is the organism): ideas to choose from | Asked |
| 35 | Huge landscapes, but on the ground you hardly ever see them | Yours to decide how: ideas to choose from | Asked |
| 36 | A ringed planet in the sky looked wrong (smeared bands on one side) | From a moon near a big planet its rings reached past the eye: half behind it, the rest smeared. Kept within reach now, a wide ring round it | Done (check) |
| 37 | The edge of the world can still be seen through the water | Where the site's ground ends under shallow water, the land round it does not quite meet it: a line on the sea floor | Open |
| 38 | Canyon worlds: the bottoms of the big rocks can be seen | Mesas and stacks standing on sloping ground with their undersides showing | Open |
| 39 | Lava is not glowing all the way out | Far off it was faded to a dull red average; brighter | Open |
| 40 | The 2D fog (mist) looks bad | Mist as flat sprites: seen from above, white blobs | Open |
| 41 | Under water is green though the water is blue from above | The underwater colour from the water's own deep colour only, not the sky it shows from above | Open |
| 42 | The middle of the smoking mounds shows the ground | The vent's mouth is an open hole down to the ground | Open |
| 43 | The cliffs (ravines) should go down a lot further | Ravines at the edge too shallow, their floors in sight | Open |
| 44 | In vacuum, footsteps, doors and so on still play as normal | Keep them, but heard as through the suit: dull, near, quiet | Open |
| 45 | Footprints should face the way you walk | Laid facing the way the view looks, not the way the body goes | Open |
| 46 | Should a planet's rings be hazy too? | Yes: seen through the air, they should be hazed as the planet is | Open |
