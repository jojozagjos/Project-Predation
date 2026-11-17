# Testing the creatures, giants and leviathans

Everything they can do, and how to see each one. Open the console (`~`) and type the commands shown. Use a **test site** or the
**creature lab**, never your own campaign:

```
campaign_new mytest 77        (a scratch campaign: your own is left alone)
test_site temperate flat none any 5   (a site of any kind: biome, ground, water, kind, seed)
lab                           (the creature lab: a flat, lit test ground with doors and rooms)
```

## The testing tools

| Command | What it does |
|---|---|
| `spawn_creature [seed] [ahead [m [turn°]]] [tiny\|small\|usual\|big] [four\|six\|two\|crawler] [predator\|territorial\|timid\|curious] [colour] [pack N] [asleep\|hungry\|tired]` | Makes a creature. `spawn_creature 11 ahead 10` puts one 10 m in front of you. |
| `spawn_creature giant [seed] ahead 150` / `spawn_creature leviathan [seed] ahead 40` | Makes a giant (on land) or a leviathan (needs water: `test_site oceanic flat seabed any 5`). |
| `creature_do <what> [n] [seconds]` | **Makes the creature nearest you do something now, at you**: hunt, attack, stalk, ambush, flank, lure, retreat, search, observe, avoid, warn, playdead, investigate, feed, roam, sleep. For 25 s unless you give seconds. Tab completes the words. |
| `giant_do <mode>` | **Makes the giant do something now, at you**: roam, notice, approach, hunt, strike, linger. |
| `creature_mind` | What the watched creature is doing, why, and its last decisions. |
| `ai_brain` | The brain inspector on screen: what it sees, hears, remembers, every option and its score. |
| `giant_mind` | What the giant is doing, where its head is, how many feet are on roofs. |
| `creature_hurt [amount]` / `giant_hurt [amount]` | As if you shot it (all its health kills it). |
| `grab_me` | The nearest creature takes hold of you. |
| `creature_clear` | Removes every creature. |
| `giant_view [side\|front\|quarter\|above\|below] [m]` | Puts the free camera by a giant's head (`fly` to get back). |
| `view_head [degrees] [m]` | The free camera at the watched creature's head. |
| `day_hour <0-24>` | Sets the time of day (night is when they are worst). |
| `site_weather <none\|rain\|snow\|dust\|ash> [0-1]` | Weather (storms drive creatures indoors). |
| `site_fog <0-1\|world>` | How foggy the site is, over its weather (fog comes with snow, damp mornings and fog worlds). |
| `torch` | Your flashlight on/off. |
| `fly` | Free camera on/off. |

## Creatures

**Senses and knowing you're there**
- Sees you by how lit you are, how big (crouch to be smaller), how you move, and how long it has looked (a glimpse is not a sighting). Test: crouch still in the dark vs. stand in light.
- Hears gunshots, footsteps, doors, things knocked over, voices. Test: shoot near one that can't see you.
- Smells / follows a trail of where you went.
- Shot from out of its sight, it knows where the shot came from and comes for you. Test: `creature_hurt 20` from behind.
- Doors and walls block its sight and its head (it can't look or poke through a door's port).
- Remembers you: who hurt it, where you were last, whether you watch it.

**What it does** (`creature_do <word>` forces each)
- `roam`: wanders, prowls where people go, explores, goes in out of storms.
- `investigate`: goes to look at a noise or a glimpse.
- `observe` (curious ones): follows you at a distance, openly, watching, walking where you walked.
- `stalk`: shadows you from out of your sight, cover to cover, waiting for you to look away or be alone, going round to where you aren't looking. Lamps near it stutter.
- `hunt`: comes for you. Under your eyes it comes in on a slant, side to side, and close in it moves sidelong, facing you. Shot at indoors, a climber goes up the wall and comes across the ceiling, then drops on you.
- `attack`: swipe, bite, lunge (knocks you down), grab. Packs: one holds your attention with a roar while another comes round behind.
- `grab` and `drag` (`grab_me`): takes hold of you and carries you away from the others; struggle (jump) to get free, or have someone shoot it off.
- `ambush`: waits beside a door on the far side from you (out of the line of its port), at the mouth of a crawlspace it can't follow you into, or up on a ceiling. A door it can't open (an airlock) it gives up on, for longer each time, and waits beside instead.
- `flank`: goes round to where shooting came from, out of sight of it.
- `lure` (mimics): hidden near you, says something back in the voice of somebody you know.
- `retreat` / withdraw: hit-and-run, or hurt and backing off; cornered while running, it turns on you.
- `search`: goes through where you could have gone, lockers included (it opens them).
- `avoid` (timid ones): keeps away.
- `warn` (territorial ones): stands its ground and hisses at you on its ground.
- `playdead`: lies as if dead, badly hurt, until you come close or turn your back.
- `feed`: eats a body, head down and not watching; patient ones then wait near it for whoever comes.
- `sleep`: turns round on the spot, lies down curled, breathing slowly. Wakes to noises and people. Hurt ones go well away and tuck in somewhere sheltered first.

**Moving**
- Opens doors (pulls them; bashes locked ones), climbs walls and ceilings, squeezes through crawlspaces, jumps gaps, swims.
- Stands up on its back legs now and then (some), mostly when it sees you.

**Learning (over a campaign)**
- Shot in encounter after encounter, it gets careful (round the side, from hiding). Lit then shot twice, it learns to fear your light. Its kind learns which tactics work.

**Quirks** (some have one): goes about on the ceiling; taps on walls while it stalks; shrieks and brings the others; extra patient watcher; one blow and away; goes for lights; afraid of lights; plays dead readily; paces; clicks in the dark; silent; always waits by a body.

## Giants (striders)

- **Sees you from 300 m.** Watched from more than 40 m, it stands dead still, eyes dimmed (`giant_do notice` then look at it). Shot, it never freezes again for 90 s and comes at once.
- **approach**: comes to you, stepping over buildings lower than its belly (feet on roofs, body rising over them); finds a way round taller ones. Near you it comes round you sidelong, eyes on you.
- **hunt**: over you, circling slowly, sizing you up, one strand reaching out and feeling about toward you. Close, it bends right down on its legs to look at you, or in at the windows.
- **strike**: with strands: one is drawn back, then whipped down onto you and takes you (`giant_do strike`). Without strands: raises a foot and stamps on you.
- **hold**: lifts you to its eyes, turns you over; then eats you (hungry, angry, or it has smashed you once already) or swings you down and smashes you into the ground and comes again. Struggle (jump) or shoot it to get free.
- **linger**: you went indoors: it waits outside where you went in, taps at the way in with a strand or stamps beside it, and gives up after 45 s.
- **dead** (`giant_hurt`): legs give, it comes down once and lies still, on a roof if it's over one.
- Its footsteps shake the ground near you. Giant-sized signs lie along its way into the site.

## Leviathans

- Seven kinds: sea serpent, eel, armoured, frilled, barbed, paddled sea reptile, sea dragon. Heavy toothed heads, two eyes a side, a jaw that opens.
- **Senses swimmers** within 80 m (only in the water).
- **hunt**: lights out; swims off into the murk out of sight, goes round to behind where you're looking, comes in from behind and below (`giant_do hunt`).
- **strike**: jaws wide, a rush at where you'll be, a bite in passing (it never takes you). Dodged, it turns and comes again.
- Its body is solid: it pushes you out of its way. It feels ahead for rocks and structures and goes round them.
- Mouth: nearly shut going about, working as it hunts, wide on the rush, slack dead.
- Surfaces now and then for a while; calls in the water.
- **dead**: floats up along the surface (an armoured one sinks).

## Things worth trying
- Shoot a creature from out of its sight, then hide: does it come for the shooter, search, give up sensibly?
- Stand in an airlock: does it wait beside the door rather than at the port, and not loop back for ever?
- Watch a strider from far off, then shoot it.
- Go into a building with a strider after you: does it wait, tap, stamp, give up?
- Swim near a leviathan and watch where it goes: off, round, from behind.
