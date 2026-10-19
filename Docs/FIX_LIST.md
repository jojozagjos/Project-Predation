# Fix list

Everything raised in play on 2026-10-19, in one place so nothing is lost. Each item: what you said, what is
behind it, and where it stands. Ticked when done and pushed; the commit is noted.

Status: **Open**, **Doing**, **Done** (commit), **Planned** (a bigger piece that belongs to a phase of
[MASTER_PLAN.md](MASTER_PLAN.md)).

---

## Feel and performance

| # | You said | What is behind it | Status |
|---|---|---|---|
| 1 | The game feels choppy though it says about 111 FPS | The view is smoothed between physics steps, so it is not that. Your settings have vsync off and no frame cap, in a window: frames reach the screen unevenly and tear, which reads as choppy at any rate (try vsync on). Long frames now name what took the time, in the log. Measured properly once your game is closed (two at once share the graphics card) | Doing |
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
| 11 | Teleported back in water; should be able to swim out a fair way | Deep water is the edge until swimming: swimming comes in (Phase 12 brought forward) | Open |
| 12 | Footsteps in water wrong; walking-in-water sounds bad | Ground footsteps play under the splash; water wants wading sounds by depth, no ground step | Open |
| 13 | No ocean or water sounds | The shore loop is a made sound and only at the shore; recorded surf, lapping, wind over water (needs your yes to download) | Open |
| 14 | Water should have waves | The surface is flat with moving normals; real swell needs the mesh moved (and things floating on it) | Open |
| 15 | Underwater: a camera effect, sounds muffled; seeing clearly under the water and the edge of the map is wrong | No underwater view yet: fog and colour under the surface, light from above, sound muffled; the sea floor carried out past where it can be seen | Open |
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
