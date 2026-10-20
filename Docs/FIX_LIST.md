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
| 10 | Water splash particles look bad | Soft drops now, drawn as the weather is: thrown up and falling back streaked, with a quick puff of white where the water broke, gone as they fall back into it; from wading steps (harder the deeper) and from each stroke of a swimmer at the surface, for everybody | Done |
| 11 | Teleported back in water; should be able to swim out a fair way | Swimming: past about 1.35 m deep you float with your eye just over the surface and swim (shift faster, on stamina); jump rises, crouch dives; pushing into a bank you climb out onto it. Out as far as anywhere else at a site (the comms range). Acid keeps its wading limit. Still to come: a swimming pose for the body others see | Done (pose to come) |
| 12 | Footsteps in water wrong; walking-in-water sounds bad | In water the ground's step fades with depth and a recorded wading step plays, louder the deeper; past the knees a recorded splash as well. No prints left under water | Done |
| 13 | No ocean or water sounds | The made-up shore loop is replaced by recorded waves (eight, crossfaded into a loop); the splash by recordings. CC0 and one CC-BY set, all in Docs/CREDITS.md | Done |
| 14 | Water should have waves | The surface moves with real travelling waves now, sized by the wind and weather, and swimmers ride them (see 61) | Done |
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
| 23 | Light flickering through a door, appearing and vanishing | The same cause as 28, fixed there: a failing lamp beyond the door gave its shadow away while dark and lit through the shut door until it was drawn again. The light let through a doorway itself follows the door's angle each frame. Say if you still see it | Done |
| 24 | Opening a door, the shadow shakes as the lighting updates | Not found yet: the lamps near a moving door are redrawn up to eight a frame and their shadows filtered smoothly, from before this was raised. Needs seeing it happen -- which door, which lamp (a screenshot or two in a row would help) | Open |

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
| 29 | Footprints still look bad from the side | No cards any more: the ground itself is drawn with the prints near the eye pressed into it (their slopes bend the way it faces, so the sun, lamps and snow shade them as the ground; the floor of each darker) -- nothing to see edge-on | Done |
| 30 | Snow worlds: the frozen falls clip into things and look bad | Laid on a straight line from the cliff's lip to its foot, the fall went into the rock where the face bulged and stood off it where it fell back. Each column now follows the face as it is at each height | Done |
| 31 | The organism's signs had missing faces | Each ring of a branch was turned square to it afresh at each step, and flipped a quarter turn where a root dipped steeply: the length between twisted flat into a blade, faces inside out. The rings are now carried along the branch. And nothing scattered lies across them any more (rocks were laid on their roots) | Done |
| 32 | A lag spike choosing where to land on the map | Choosing builds the whole site there and then. It now takes half as long (a jungle 1.4 s, was 3.0; a desert 0.6, was 1.3): putting meshes together copied all of each one over at every piece added. Still a held frame; to hide or spread the rest next | Doing |
| 33 | On a moon with no air, wind should not be heard | Right. Out on a world without air there is now no wind and the ears are in vacuum; inside its buildings and the shuttle, which have air, sound carries but no wind is heard through the walls | Done |
| 34 | Should the organism's signs move or breathe; more kinds of them? | You left it to me: each formation now swells a little about its foot with the shared rhythm and settles again -- slow, a percent and a half at most, only seen by somebody watching. More kinds of sign later. Not twitching at players | Done (more kinds later) |
| 35 | Huge landscapes, but on the ground you hardly ever see them | Your picks: a much larger roam area, objectives out in the land, and the walkable area not a square -- other shapes and ways through (a pass, a canyon). Ideas of mine to add when it is built | Planned |
| 36 | A ringed planet in the sky looked wrong (smeared bands on one side) | From a moon near a big planet its rings reached past the eye: half behind it, the rest smeared. Kept within reach now, a wide ring round it | Done (check) |
| 37 | The edge of the world can still be seen through the water | The sea floor falls away steeply past its shelf, and the water let the floor show by its depth alone: from a low look the shelf's edge was a straight line across the sea. The water now hides its floor by how far the look goes through it, as real water does; only the shallows near you show their floor | Done |
| 38 | Canyon worlds: the bottoms of the big rocks can be seen | A butte, spire or balanced rock started 12 m below its foot, on ground that fell away much further round it. Each is now carried down below the lowest ground round it | Done |
| 39 | Lava is not glowing all the way out | Far off it was faded to a dull red; brighter now, still in patches as its plates lie | Done |
| 40 | The 2D fog (mist) looks bad | Removed (flat sprites, blobs from above) until it can be done properly | Done |
| 41 | Under water is green though the water is blue from above | The murk now has the sky's colour in it as well as the water's own, as the surface does from above | Done |
| 42 | The middle of the smoking mounds shows the ground | Each now has a dark throat down from its mouth | Done |
| 43 | The cliffs (ravines) should go down a lot further | 40 to 65 m deep now (were 16 to 26) | Done |
| 44 | In vacuum, footsteps, doors and so on still play as normal | Kept, but heard as through the suit: dull, quieter, and only from a few metres (what you touch or stand on); nothing from further, no room echo. The radio, voices, the intercom, the interface and music are in the helmet and stay clear | Done |
| 45 | Footprints should face the way you walk | Laid facing the way the body faces (see 53) | Done |
| 46 | Should a planet's rings be hazy too? | Yes, through air: now hazed and let through as the planet is | Done |
| 47 | The organism's signs still have faces that do not render right | Where a thick branch left the knot its flare narrowed faster than its rings were apart, and its bends were tighter than it was thick: it folded into a fin. The flare now fades over a few of its own thicknesses and thick branches bend no tighter than they are wide | Done |
| 48 | Swimming: its movement and animations; crouching or crawling where the water is over your head should swim | Crouching or lying down where the water is about your eye now swims (and standing up where it is shallow walks again). A swimmer lies forward in the water, its eye just over it, and is drawn so -- arms reaching and pulling, legs drawing up and pushing -- for everybody (it travels with the snapshot). Floats steadily at the surface (it bobbed through it before), and crouch-toggle no longer makes it dive. Proper strokes and kick still to make, in place of the crawl's | Done (its own animation since: see 57) |
| 49 | Rain (and other weather) is drawn under water | No weather, smoke or blown snow is drawn under water now, and from above nothing falls on past the surface | Done |
| 50 | Shadows under water are as sharp as in air (and seen from any distance) | Under water the sun now dies away with depth (little of it or its shadows a few metres down); and the soft contact shadows round things (ambient occlusion), laid on after the fog, showed through the murk at any distance: they fade into the fog now | Done |

## Raised 2026-10-20

| # | What you said | What is behind it / what is being done | Status |
|---|---|---|---|
| 51 | The loading screen jumps; make it smooth | It was drawn only between the steps of loading, so its dots stood still and its line jumped. Now the loading itself (each model, texture and sound, each part of the site) draws another frame of it when one is due, and the line eases on, creeping ahead while a step takes its time and never going back | Done |
| 52 | The snow particles on the ground look really bad | Taken to be the little stones scattered everywhere: on snow they were dark chips all over the white. On a frozen world there are a sixth as many now, sunk half into the snow (say if you meant something else) | Done |
| 53 | Walking backwards, the prints face the way you are going, not the way you face | Laid by the way the body faces now, for everybody | Done |
| 54 | The grass is still seen fading in | Each tuft came in through a scatter of pixels over a fixed share of the grass -- far off, tens of metres of walking -- so the far field was always half faded in. Each now comes in over a few percent of its distance (a metre or two near, a few metres far) | Done |
| 55 | At the beach the water never runs up onto the sand, though it sounds as if it does | The wash is drawn on the shore now: each wave a sheet of water running up the sand and draining back, quick up and slow back every nine seconds or so, some further than others, not all along the shore at once, foam along its edge and the sand wet behind it (the same for everybody: the campaign clock). Not along lava | Done |
| 56 | Crawling: the body lags behind when turning while a key is held; it should always be behind you | It eased round towards the line of travel, slowly, trailing every turn. Crawling now, it lies straight out behind the eye and turns as the view does (lying still, looking round still does not turn it) | Done |
| 57 | Swimming: its own animation -- treading water when still, strokes by direction, procedural; movement like Subnautica's; sliding along the bottom when holding still; up and down feel like a toggle | Its own procedural animation: upright treading water when still (hands sculling, feet circling), lying along the way it goes when swimming (flat ahead, head first going down, upright going up) with a crawl stroke and a flutter kick, upright and leaning going sideways, leaning back backing away. Movement as in Subnautica: swum the way you look, pitch and all; jump held rises, the crouch key held sinks (the key itself -- as a toggle it latched); under the surface you stay at the depth you leave off at; floating at the surface. Held still on the bottom it stops dead. What is in your hands is put away while swimming | Done |
| 58 | Lava hurts standing near it but not on it; it should look more real and glow round it more; the near and far lava look different; going into it should not teleport you out with text | It burnt anyone within a third of a metre of its level -- metres back from a gentle shore. Now only in it, and fast (a few seconds at the ankles, a moment deeper); no putting you back, no text. Its look: black glassy crust, a network of glowing cracks with the crust hot along them, open molten patches churning, a broken line of fire along the shore; its heat by distance from the shore, not by depth (a shallow pool was all crust, a deep lake all glow), and far off as dark on average as near. Its glow lights every stretch of shore over the walkable ground now (it was 16 lamps from one corner of the map) | Done |
| 59 | Moons should look like moons: craters, little on them | A moon with no air is now a crater field four times in five, whatever kind of world. Crater fields have many more craters, sized as real ones are (many small, a few great), as deep as fresh craters (a fifth of their width), rims falling away gently outside, and are smoothed less (nothing has worn them). On any airless world: a third as much lying about, and no arches, spires, buttes or balanced rocks (nothing carves them without weather) | Done |
| 60 | Plan for underground: caves, underground facilities and so on | Planned in MASTER_PLAN Phase 13: underground facility levels, then mines, then caves (their own mesh set into the ground). First part done: basements -- the main building has one or two more often than not, the others now and then; mostly plant rooms and storage, reached by the stairs, the ground left out under them | Doing |
| 61 | Whole water worlds; the water more real, with waves that can get very big and change with the weather | Planned in MASTER_PLAN Phase 12: waves by the weather (to storm seas), the water's look, then platform sites on water worlds, then underwater places. First part done: the sea's surface now moves with real travelling waves sized by the wind -- a gentle swell on the stillest day, a couple of metres in a strong wind, storm seas of several metres, foam breaking on high crests, calm in the shallows; swimmers ride them (the same waves on every machine). Water worlds still to come | Doing |
| 62 | Little flat smoke puffs near the water when it rains | The rain's big soft puffs of mist, flat cards, went through the water's surface and showed cut off along it. Kept clear of it now, and fainter the lower they were | Done |
| 63 | Puddles show under water | None under water now, nor on the shore it washes over | Done |
| 64 | Footprints showed inside the facility | With snow lying outside, a floor at the ground's height took prints as if snowed on. Only out under the sky now (not in a building or the shuttle) | Done |
| 65 | Footprints are drawn over your own legs and body | Pressed only into what lies out in the weather (the ground, a yard), never a body | Done |
