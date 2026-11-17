# The creatures, giants and leviathans: everything they do now, and what you want

This lists everything the AI does today, in plain words, with the numbers that drive it. Under each part are **my notes** (what I think is weak) and **questions** for you.

Answer any way you like, for example "Q4: B", "Q9: fine as is", or "Q12: something else: …". Anything you don't answer, I'll leave as it is.

How to see each of these in the game is in [CREATURE_TESTING.md](CREATURE_TESTING.md).

---

## Part 1: Where they are and how many

**How it works now**

- Each site rolls whether anything lives there:
  - near home, a little under half of sites have something;
  - out at the far reaches, up to 85%;
  - a site with a contract job, almost always (85 to 97%).
- If something is there, it **lives** there (3 in 4) or is **passing through** (1 in 4).
- **One group per site, of one kind:**

  | Size | How many |
  |---|---|
  | Tiny | 3 to 4 |
  | Small | 2 to 3 |
  | Usual | 1 (sometimes 2) |
  | Big | 1 (never inside buildings) |

  Big ones are rarer near home: 1 in 12 there, 1 in 5 far out.
- **Where it lives:**
  - a cave's deepest chamber;
  - a dark room far from a building's way in;
  - out on the land.
  - You find it there **asleep**, **feeding** or **prowling**. A sleeper stays down 4 to 12 minutes unless something wakes it.
- Near home, the group is sometimes **curious** or **territorial** instead of a predator.
- **Giants:**
  - never in the home system;
  - on land, 6 to 20% of sites, more the further out;
  - water worlds, 3 in 4 sites have a leviathan.
  - Where there's a giant, there's nothing else.
- Killed creatures don't come back (`ai.return_seconds` is 0).

**My notes**

- One group per site keeps it sparse, which is what you asked for.
- Two smaller groups of different kinds would make sites less predictable, but would also make creatures less rare.

**Questions**

- **Q1. How many per site?**
  - A) As now (one group, often none).
  - B) Sometimes two groups of different kinds far out.
  - C) Fewer still.
- **Q2. Should more come once you've killed what was there?**
  - A) Never (as now).
  - B) Rarely: one more after a long time, far out only.
  - C) Yes, something always comes back eventually.
- **Q3. Giants and other creatures together?**
  - A) Never (as now).
  - B) Allowed far out: a strider and a small group at one site.

---

## Part 2: How they sense you

**How it works now**

- **Sight:**
  - up to **26 m**, in a cone 130° wide;
  - weaker at the edges and in the dark (at 22% in full dark);
  - weaker when you're small: crouching is 45%, lying down even less;
  - stronger when you're lit; a torch that's on makes you very visible.
  - It needs about **0.6 s of looking** before it's sure; a glimpse only makes it suspicious.
  - Doors, walls and windows' frames block sight.
- **It senses you without seeing:**
  - within **1.6 m** always;
  - within **4 m** it feels someone's there.
- **Hearing:**
  - gunshots, footsteps (running loud, crouching quiet), doors, things knocked over, and your **voice** (proximity voice chat);
  - sound through walls counts half.
- **A trail:** it can follow where you went, from about a minute ago up to 30 seconds ago.
- **Shot from out of its sight:** it knows where the shot came from.
- **Memory:**
  - who hurt it;
  - where it last knew you were;
  - whether you're watching it, which it uses to wait for you to look away.

**My notes**

- 26 m is short outdoors. A creature in an open field won't see you at 30 m in daylight.
- That's fine indoors, but outside it can feel blind.

**Questions**

- **Q4. Sight range?**
  - A) Keep 26 m everywhere.
  - B) Longer outdoors in daylight (about 60 m), 26 m indoors and at night.
  - C) Longer everywhere.
- **Q5. Does your voice chat attract them?** (Right now, talking near one is a noise it hears.)
  - A) Yes, keep it.
  - B) Only loud talking or shouting.
  - C) No.
- **Q6. Should your torch be as big a giveaway as it is?** (On, you're seen from its whole range even in the dark.)
  - A) Yes.
  - B) A bit less.
  - C) More: the beam itself should draw them from far.

---

## Part 3: What they do (their behaviours)

Every creature picks what to do every moment by scoring all of these, from:
- its nature (temperament plus two habits of its own);
- its needs (hunger, tiredness, hurt);
- what it knows about you.

You can force any of them with `creature_do`.

| Behaviour | What it does now |
|---|---|
| **Roam** | Wanders; prowls where people have been; explores; goes indoors out of storms. |
| **Investigate** | Goes to look at a noise or something half-seen. |
| **Observe** (curious ones) | Follows you openly at a distance, watching, walking where you walked. Doesn't attack unless provoked or hungry. |
| **Stalk** | Shadows you out of your sight, cover to cover. Waits 6 to 24 s for you to look away or be alone, and goes round to where you aren't looking. Lamps near it flicker. |
| **Hunt** | Comes straight for you. In the open under your eyes it comes on a slant, side to side; close in it moves sideways, facing you. Shot at indoors, a climber goes up the wall, across the ceiling, and drops on you. |
| **Attack** | Swipe (quick), bite (worse), lunge (from a few metres, knocks you flat), grab. In a pack, one holds your attention with a roar while another comes round behind. |
| **Drag** | Has hold of you and carries you away from the others. Jump to struggle free, or someone shoots it off. |
| **Ambush** | Waits 15 to 50 s somewhere you'll have to pass: beside a door (out of its window's line), at the mouth of a crawlspace it can't fit, or up on a ceiling. |
| **Flank** | Goes round to where shooting came from, out of sight, listens, then goes in. |
| **Lure** (mimics) | Hides near you and says something back in the recorded voice of a friend of yours (at most every 50 s). Only with players who allow it and have spoken. |
| **Retreat** | Hit-and-run, or badly hurt and backing off. Cornered while running, it turns on you. |
| **Search** | Lost you: goes through where you could be, opening lockers. |
| **Avoid** (timid ones) | Keeps away from you. |
| **Warn** (territorial ones) | On its ground (16 m from where it started), stands and hisses. Comes at you if you keep coming. |
| **Play dead** | Badly hurt: lies as if dead until you come close or turn your back. |
| **Feed** | Eats a body, head down, not watching. Patient ones wait near it afterwards. |
| **Sleep** | Turns round, lies down curled, breathes slowly. Noise or people wake it. Hurt ones go somewhere sheltered first. |

**The four natures:**
- **predator**: hunts you;
- **territorial**: warns, then fights on its ground;
- **timid**: avoids you, fights only cornered;
- **curious**: watches and follows.

**Habits** (two each, from its seed):
- goes about on the ceiling;
- knocks on walls while it stalks;
- shrieks to bring the others;
- extra-patient watcher;
- one blow and away;
- drawn to lights;
- afraid of lights;
- plays dead readily;
- paces;
- clicks in the dark;
- silent;
- waits by a body.

**My notes**

- These all work and are tested, but some are hard to ever see in a normal game, because a single creature that's a predator mostly stalks, hunts and attacks.
- Observe, warn and avoid only happen with the gentler natures, which are rarer far out.

**Questions**

- **Q7. Is anything here you don't want?** Name any to remove or make rarer (for example lure, play dead, search lockers, the ceiling drop).
- **Q8. Is anything missing you want them to do?** Some ideas:
  - A) Drag a body away to eat it somewhere else.
  - B) Break lights or cut the power on purpose.
  - C) Hide in lockers or vents themselves.
  - D) Follow you back to the shuttle.
  - E) Nothing more.
- **Q9. How much should the gentle natures show?**
  - A) As now (near home gentler, far out mostly predators).
  - B) More gentle ones everywhere, so meeting one isn't always a fight.
  - C) Predators only.
- **Q10. Stalking time:** it waits 6 to 24 s for an opening before trying something else.
  - A) Fine.
  - B) Longer: more slow tension before it comes.
  - C) Shorter: more action.

---

## Part 4: Fighting

**How it works now**

- **Its blows:**
  - damage comes from its body (bigger hits harder);
  - from behind or while holding you, blows land harder (×1.15 to ×1.5);
  - a lunge knocks you flat;
  - a grab takes you, and you're dragged off.
- **Getting free:**
  - jump repeatedly to struggle;
  - or a friend does enough damage (6% of its health) to make it let go.
- **Hurting it:**
  - where you hit matters (the body knows head/limbs);
  - when it's had enough it goes down for a moment;
  - badly hurt, it retreats or plays dead.
- **Doors:**
  - it pulls them open and bashes locked ones;
  - it can't open an airlock, so it gives up on it, longer each time, and waits beside it.
- **The pacing ("director"):**
  - pressure builds while creatures are close to you or busy with you, and jumps when a blow lands;
  - at full pressure the near ones back off and give you room for 35 to 60 s;
  - after 60 s of quiet, an idle creature is put on your trail (every 45 to 75 s).

**My notes**

- The director is what stops it being either constant attacks or nothing at all.
- The numbers are guesses that haven't been tuned against real play.

**Questions**

- **Q11. How deadly should one creature be?**
  - A) As now.
  - B) Deadlier: a usual-sized one should kill a careless player in 3 or 4 blows.
  - C) Less: more chances.
- **Q12. Grabs:**
  - A) As now (dragged off, struggle free).
  - B) A grab should be a kill unless a friend helps (very scary in solo).
  - C) Rarer.
- **Q13. The director:**
  - A) Keep it.
  - B) Less mercy: they shouldn't back off.
  - C) More quiet stretches between encounters.
- **Q14. Should creatures fight each other?** (Earlier you said organisms can fight. Right now they only keep apart and get angry when kin are hurt; nothing fights.)
  - A) Yes: different kinds fight when they meet; you can use it.
  - B) Only over a body or territory.
  - C) No.

---

## Part 5: Learning over a campaign

**How it works now**

- Shot in encounter after encounter, a creature gets careful: it comes round the side, from hiding.
- Lit up and then shot twice, it learns to fear your light.
- Each **kind** remembers which tactics worked on you (got hits in, didn't get shot) and leans toward those.

**Questions**

- **Q15. Should learning carry between kinds?**
  - A) No, each kind on its own (as now).
  - B) Yes, a little: all creatures slowly get smarter as the campaign goes on.
- **Q16. Should you be told?** For example a log entry when a kind has learned something.
  - A) No.
  - B) Yes, in the ship's log, functional text only.

---

## Part 6: How they move and look alive

**How it works now**

- Walks, runs, climbs walls and ceilings, squeezes through crawlspaces, jumps gaps, swims.
- Some stand up on their back legs now and then, mostly when they see you.
- Feet step one at a time with the body's weight sinking onto them.
- It leans onto the side its feet are on.

**Questions**

- **Q17. Anything about how they move that still looks wrong to you?** (Climbing, standing and mimic polish are on the to-do list already.)

---

## Part 7: Giants (striders)

**How it works now**

- **Senses:** sees you from **300 m**.
- **Freeze:** watched from more than 40 m, it stands dead still with its eyes dimmed. Once you've shot it, it stops freezing for 90 s and comes at once.
- **Approach:**
  - walks to you over buildings lower than its belly, feet on the roofs, body rising over them, and round taller ones;
  - near you, it circles sideways, eyes on you.
- **Hunt:**
  - over you, circling slowly, one strand reaching out and feeling about;
  - it bends right down to look at you, or in at windows.
- **Strike:**
  - with strands: one is drawn back, then whipped down onto you, and it takes you;
  - without strands: it raises a foot and stamps.
- **Hold:**
  - lifts you to its eyes and turns you over;
  - then it **eats** you (dead) if it's hungry, angry, or has smashed you before;
  - otherwise it **smashes** you into the ground (45 damage) and comes again.
  - Struggle or have someone shoot it to get free.
- **Linger:**
  - you went inside: it waits outside where you went in, taps at the door with a strand or stamps beside it;
  - it gives up after **45 s**.
- **Dead:** its legs give and it comes down, on a roof if it's over one.
- Footsteps shake the ground; giant-sized signs lie along its way in.

**My notes**

- Giving up after 45 s is short for something that size. On the other hand, a giant that never leaves would trap you indoors.

**Questions**

- **Q18. The freeze when watched from far** (like a statue until you look away):
  - A) Keep.
  - B) Remove.
  - C) Only at night.
- **Q19. Can a giant be killed with your weapons?** (It has thousands of health; it can.)
  - A) Yes, as now.
  - B) Only driven off: it leaves when hurt enough, never dies.
  - C) Only with something special (later).
- **Q20. Linger time:**
  - A) 45 s.
  - B) Longer (2 to 3 minutes), then it wanders off and comes back later.
  - C) It stays near the site until you leave.
- **Q21. Should it reach into buildings** (a strand through a window or door to feel for you)?
  - A) Yes.
  - B) No, buildings are safe from it.
- **Q22. How a hold ends:**
  - A) As now (eat or smash).
  - B) Always eaten unless freed.
  - C) Thrown far instead of smashed.

---

## Part 8: Leviathans (in the water)

**How it works now**

- **Kinds:** seven (sea serpent, eel, armoured, frilled, barbed, paddled sea reptile, sea dragon), serpent bodies only.
- **Senses:** swimmers within **80 m**, only in the water.
- **Hunt:**
  - goes dark, swims off into the murk out of sight, round to behind where you're looking;
  - then comes in from behind and a little below.
- **Strike:**
  - jaws wide, a rush at where you'll be, a bite in passing;
  - it never grabs (as you asked);
  - missed, it goes off and round again.
- **Its rush is heard** (a roar through the water) and so is its bite.
- **Its body is solid:** it pushes you aside, feels ahead for rocks and buildings and goes round, and gives up a rush if something's in the way.
- **Surfaces** for a while now and then (about every 6 minutes, rarely); **calls** in the water.
- **Dead:** floats up (an armoured one sinks).

**Questions**

- **Q23. Out of the water**, on the floating pad or in the moon pool, should it ever come for you?
  - A) Never (as now).
  - B) It can rise and snap at someone at the edge of the pad.
- **Q24. How often should it strike?** Right now it can take a minute or two to circle round before it comes.
  - A) Fine.
  - B) More often.
  - C) Rarer, more dread.
- **Q25. Should it react to light** (your torch underwater)?
  - A) Yes, it draws it.
  - B) Yes, it scares it off.
  - C) No.

---

## Part 9: Anything else

- **Q26.** Anything else you want them to do, or stop doing, before we call the AI finished and move on to the next phase?
