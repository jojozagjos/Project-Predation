# Open exploration rework: the design

The user's design for reworking the game, given 2026-10-04, kept here word for word as the reference. Decisions
that follow it are in [DECISIONS.md](DECISIONS.md) from ADR-124; the user's answers to the first questions it raised:

- **Why the crew is out there:** an independent, CIRRA-contracted exploration and recovery crew operating their own
  ship: taking jobs, investigating unknown locations, recovering valuable material, and gradually pushing farther
  into less-charted space. Not CIRRA employees obeying every order.
- **CIRRA's missions** stay, as **optional contracts** (recovery, survey, investigation, distress response, research
  requests, salvage) that pay credits, components and information and sometimes open new areas or leads. The crew
  can ignore them and fly anywhere.

---

I want to rework Project Predation around a broader exploration/progression structure.

Do not treat this as a minor feature addition. This changes the overall game loop and progression.

The game should still support horror, mystery, procedural generation, ship travel, multiplayer, cinematic transitions, and exploration, but the structure should move away from:

“CIRRA assigns a mission → go there → complete objective → repeat”

and toward a more open exploration-driven campaign where players choose where to go, build up their ship, investigate locations, discover story information, and gradually uncover a larger mystery.

Do NOT focus on creatures in this implementation prompt. Leave the existing creature systems alone unless something here directly requires integration with them.

==================================================
1. HIGH-LEVEL CAMPAIGN STRUCTURE
==================================================

The broad campaign flow should become:

Tutorial / Prologue
↓
Start a new campaign
↓
Begin with a basic, cramped ship
↓
Explore nearby planets and locations
↓
Investigate points of interest
↓
Complete optional contracts / recover useful things / make discoveries
↓
Earn credits and obtain important components
↓
Upgrade and physically expand the ship
↓
Travel becomes faster and exploration becomes easier
↓
Reach more distant and unusual locations
↓
Build a shared record of discoveries
↓
Gradually uncover a larger story mystery
↓
Figure out how to reach the final destination
↓
Endgame sequence
↓
Credits
↓
Main Story Complete
↓
Free Play

The player should have substantial freedom after the tutorial.

The game should encourage exploration rather than constantly telling players exactly where they must go.

==================================================
2. TUTORIAL / PROLOGUE
==================================================

The tutorial should be treated as its own linear prologue.

It should teach the player how the game works, but it should not feel like:

“Press W to move.”

“Now interact with this.”

“Now do this.”

Instead, mechanics should be taught naturally through a short, controlled story sequence.

The tutorial should:

- introduce the world
- establish the visual tone
- introduce CIRRA / the Company
- teach essential controls
- teach basic interaction
- teach important ship systems
- teach the map/navigation concept
- teach basic planetary travel
- establish the darker undertones of the setting
- feel like part of the game world rather than a detached training room

The tutorial can be more linear than the main campaign.

After the tutorial is complete, the player can return to the main menu and begin the actual campaign.

Do not overbuild the tutorial story yet. The important thing is that the tutorial framework supports story-driven teaching.

==================================================
3. CAMPAIGN START
==================================================

The player should begin the real campaign docked at a station in orbit of the settled home world (changed from a
planet-based hangar / shipyard, ADR-129). The station is where the shipyard's work will be done: selling, upgrades, refits.

The starting ship should be:

- functional
- small
- somewhat cramped
- visually basic
- clearly upgradeable

The player and their crew are starting with a ship that works, but it is far from impressive.

Over time, that same ship should become increasingly capable and physically larger.

==================================================
4. ONE PERSISTENT SHIP
==================================================

Do NOT make progression about constantly replacing the ship with completely different ship classes.

The player should primarily keep the same ship throughout the campaign.

The ship should physically evolve.

Upgrades should be visible where appropriate.

Examples:

- upgraded travel systems change exterior engine assemblies
- new ship sections are physically added
- storage expansion creates more usable space
- new modules alter exterior silhouette
- new internal areas become accessible
- upgraded equipment visibly changes consoles or systems

The ship should become a visual record of progression.

The player should eventually be able to look at their ship and immediately see how far they have come.

==================================================
5. SHIP SIZE PROGRESSION
==================================================

Ship expansion is one of the major progression systems.

The ship starts cramped.

Over time, the player should be able to:

- add sections
- expand rooms
- add new rooms
- increase usable space
- increase storage
- add specialized modules

The ship should still maintain coherent architecture.

Do not make upgrades look like random boxes glued onto the hull.

Interior and exterior changes should correspond where reasonable.

==================================================
6. SHIP CUSTOMIZATION
==================================================

Allow visual customization of the ship.

At minimum:

- primary color
- secondary color
- accent color

For example:

- orange + black
- yellow + blue
- white + red

The colors should affect logical exterior ship materials.

Additional cosmetic systems can be added later.

Potential later additions:

- markings
- decals
- hull identifiers
- lighting styles

Do not prioritize cosmetics over functional progression, but establish the system cleanly.

==================================================
7. SHIP UPGRADE CATEGORIES
==================================================

Build the upgrade system modularly.

Initial useful categories include:

TRAVEL SYSTEM
- reduces travel time
- progressively makes long journeys less tedious
- high-end upgrades can eventually make travel effectively near-instant after the departure / arrival cinematics

SHIP SIZE
- physically expands the ship
- creates new rooms and usable space

STORAGE
- increases the amount of equipment / recovered items that can be kept

SENSORS
- reveals more information about planets before arrival
- improves detection of points of interest
- improves environmental / location knowledge

NAVIGATION
- improves destination information
- helps identify obscure or distant locations
- improves route planning

RESEARCH / ANALYSIS
- helps identify discoveries
- may provide more useful information about recovered data, materials, or unknown phenomena

CREW / QUALITY-OF-LIFE SYSTEMS
- can improve usability, comfort, or support features
- exact systems can be added later

UTILITY MODULES
- reserved for future mission-specific or exploration-specific systems

COSMETICS
- colors and visual customization

Do not hard-code the upgrade architecture around only these categories.

New upgrade categories should be easy to add later.

==================================================
8. TRAVEL
==================================================

Travel should remain physical.

Players should stay aboard the ship while traveling.

They should be able to:

- walk around
- talk
- use voice chat
- interact with ship systems
- inspect equipment
- open the system map
- change destination if needed

Do not treat travel as only a loading screen.

Travel should take time.

A farther destination should generally take longer than a nearby one.

Travel-system upgrades should reduce travel time.

Eventually, very advanced upgrades can make travel so fast that the practical sequence becomes:

departure cinematic
↓
short transition
↓
arrival cinematic

Do not call this “time travel.”

This is simply faster ship travel / advanced propulsion.

==================================================
9. CHANGE DESTINATION DURING TRAVEL
==================================================

Players should not be permanently locked to a destination after selecting it.

While traveling, players should be able to:

- open the map
- choose another destination
- redirect the ship
- cancel or modify the current route

The autopilot should recalculate the course.

This should work in multiplayer.

==================================================
10. DO NOT USE FUEL AS A MAJOR SYSTEM
==================================================

Do NOT build a complicated fuel system right now.

Avoid:

- constant refueling chores
- fuel grinding
- getting permanently stranded
- fuel micromanagement

The main travel progression should come from:

- travel speed
- ship capability
- navigation
- player choice

If a soft-range concept is useful later, it can be implemented as travel practicality rather than hard fuel gating.

Do not make fuel a central gameplay system unless explicitly requested later.

==================================================
11. SYSTEM / GALAXY MAP
==================================================

Create a 3D navigational map inspired by the feeling of looking at an actual solar system rather than a flat mission-select screen.

The player should be able to:

- open the map
- rotate around the system
- zoom
- inspect planets
- inspect moons
- see their ship’s current position
- select a destination
- initiate autopilot travel

The map should feel like an in-world navigation system.

It should NOT feel like:

“LEVEL SELECT”

==================================================
12. MULTIPLAYER MAP SYNCHRONIZATION
==================================================

The system map should work well in multiplayer.

Players should be able to see shared navigation context.

Where practical, allow players to see:

- what another player is pointing at
- another player’s cursor / selector
- the currently highlighted destination
- the ship’s shared route

This should make conversations like:

“Look at that planet over there.”

actually useful.

The host remains authoritative for final travel decisions unless the existing multiplayer architecture already handles this differently.

==================================================
13. SEEDED PLANETS
==================================================

Planets should continue to use deterministic procedural seeds.

A planet’s seed should determine its broad persistent identity.

This may include:

- appearance
- size
- atmosphere
- colors
- cloud patterns
- oceans
- continents
- ice regions
- storms
- rings
- moons
- biome distribution
- terrain characteristics
- other major traits

If the exact same planet seed is revisited, it should appear to be the same planet.

Do not regenerate it as an unrelated planet every visit.

==================================================
14. LARGE UNIVERSE / PROCEDURAL DISCOVERY
==================================================

The game should feel extremely large and potentially expandable.

Do not load or generate every possible planet at once.

Use deterministic procedural generation.

Generate systems / planets when needed.

The universe can feel effectively enormous without keeping every world active in memory.

Focus on:

- deterministic seeds
- lazy generation
- streaming
- lightweight persistence
- efficient indexing

This needs to remain practical for a solo-developed custom C++ engine.

Do not design the architecture assuming a giant cloud infrastructure or dedicated persistent MMO server.

==================================================
15. PLANET INFORMATION
==================================================

When the player highlights a planet, show only the information that is currently known.

Known information might include:

- planet name / designation
- broad biome
- atmosphere
- known conditions
- colonization status
- known points of interest
- known hazards
- existing CIRRA / human records
- discovered landing regions

Unknown information should remain unknown.

Sensors and exploration can improve this information.

==================================================
16. PLANET SCALE
==================================================

Do NOT create fully explorable seamless planets.

The full planet should exist visually from space.

Gameplay occurs in generated landing regions.

Each planet can have multiple landing regions.

A landing region should be large enough to feel exploratory, but not so large that the player spends most of the game walking through nothing.

The game should primarily support exploration on foot.

Do not rely on vehicles as a required core system.

Vehicles may be considered later, but they are not necessary for the initial design.

A good rule:

The environment should contain enough meaningful density that walking between major points of interest does not become tedious.

==================================================
17. LANDING REGIONS
==================================================

A planet can contain:

- known landing regions
- discovered regions
- story locations
- generated exploration regions
- colonized service locations

The player should not necessarily see every region immediately.

Sensors, exploration, signals, records, and discoveries can reveal additional regions.

Once a region has been visited, preserve enough state that revisiting it feels like returning to the same location.

Use deterministic regeneration plus state deltas rather than saving unnecessarily huge amounts of raw generated geometry.

==================================================
18. PLANET VARIETY
==================================================

I want a large amount of planetary variety.

Do not build every planet as a completely separate handcrafted generator.

Instead, use combinatorial generation.

A planet can be built from:

BASE BIOME
+
ATMOSPHERE
+
TERRAIN
+
WEATHER
+
CIVILIZATION STATE
+
SPECIAL TRAITS

Possible biome foundations include:

- frozen
- desert
- volcanic
- oceanic
- temperate
- jungle
- rocky
- barren
- toxic
- storm-dominated
- unusual alien biomes

Example:

Frozen
+
thin atmosphere
+
mountainous terrain
+
heavy snow
+
abandoned research presence

or:

Oceanic
+
dense atmosphere
+
violent storms
+
scattered islands
+
submerged research locations

Oceanic planets should eventually support underwater exploration in appropriate regions.

This does not need to become Subnautica in scope, but underwater locations should be possible.

==================================================
19. WEATHER AND ENVIRONMENTAL LIFE
==================================================

Planets should feel alive.

Weather should not always be static.

Depending on the biome:

- snow can become heavier
- storms can develop
- fog can roll in
- wind intensity can change
- ash can increase
- rain can begin or stop
- ocean conditions can change

Lighting should correspond to actual planetary conditions.

Do not randomly select:

“dark”
“bright”
“hazy”

without considering where the landing region is relative to the planet’s light source and current environment.

==================================================
20. WHAT PLAYERS DO ON PLANETS
==================================================

The game should NOT revolve entirely around grinding raw resources.

Planets should offer multiple reasons to explore.

Possible activities include:

- investigate points of interest
- investigate distress signals
- explore abandoned facilities
- explore active facilities
- investigate unusual natural phenomena
- recover salvage
- find rare upgrade components
- complete optional contracts
- recover research data
- survey locations
- scan environments
- investigate wrecks
- discover lore
- discover navigation information
- discover story clues
- find new landing regions
- visit colonized service locations
- encounter unusual hazards
- investigate unexplained signals
- complete environmental objectives

Not every planet needs every activity.

Different planets should generate different combinations.

==================================================
21. POINTS OF INTEREST
==================================================

Points of interest should be one of the main reasons exploration feels compelling.

Some POIs can be known before landing.

Some should only appear once players explore.

Examples:

- facility
- wreck
- distress beacon
- unusual terrain formation
- abandoned camp
- scientific installation
- crashed transport
- cave / underground entrance
- communications tower
- research site
- unusual signal source
- hidden structure
- natural anomaly
- story-related location

Do not make every POI a combat arena.

Some should simply be:

- interesting
- unsettling
- useful
- beautiful
- strange
- story relevant

==================================================
22. RESOURCE GATHERING
==================================================

Some resource collection is fine.

Do NOT make resource grinding the primary game.

Avoid systems where the player must gather enormous quantities of generic materials such as:

800 iron
400 copper
700 carbon

just to progress.

Instead, exploration should naturally provide useful materials and components.

Resources should support the exploration loop rather than replace it.

==================================================
23. ECONOMY
==================================================

Use a relatively understandable economy.

The initial structure can use:

CREDITS
+
SPECIAL COMPONENTS / RECOVERED ITEMS

Credits can come from:

- contracts
- selling salvage
- discoveries
- survey/research work
- recovered valuables
- optional objectives

Important upgrades can sometimes require:

credits
+
a specific unusual component

Example:

Advanced Drive Upgrade
Cost: 12,000 credits
Requires: High-Capacity Field Assembly

This creates a reason to explore without creating excessive grind.

==================================================
24. COLONIZED WORLDS / SERVICE HUBS
==================================================

Some locations should be established human-controlled service locations.

Do NOT require large simulated cities with hundreds of NPCs.

These locations primarily function as service hubs.

Players can use them to:

- purchase upgrades
- change ship customization
- access stores
- restock basic supplies
- sell salvage
- access contracts
- use company / navigation services

They can still have:

- cinematic approaches
- visual atmosphere
- hangars
- shipyards
- limited NPC presence

But do not expand them into massive open-world cities unless explicitly requested later.

==================================================
25. SHARED SHIP LOG
==================================================

Add a shared campaign log.

The purpose is to let the game remember important discoveries so the player does not need to write everything down manually.

The log should be shared across the campaign / crew.

It can track things such as:

- discovered planets
- discovered regions
- important locations
- unusual signals
- story clues
- company information
- unexplained phenomena
- known relationships between discoveries
- important records
- potential leads

The log should update automatically when appropriate.

Players should also be able to:

- inspect entries
- mark important entries
- potentially pin destinations or leads

Do NOT directly copy the presentation of Outer Wilds.

The functionality can serve a similar purpose, but the design and presentation should be original and fit CIRRA / Project Predation.

==================================================
26. STORY DISCOVERY
==================================================

The larger story should emerge naturally through exploration.

Do NOT immediately tell the player:

“Here is the main mystery.”

Early in the campaign:

- the player explores
- earns money
- improves the ship
- learns the universe
- takes contracts
- investigates places

Gradually, unrelated discoveries begin pointing toward something larger.

This should happen slowly.

Eventually the crew realizes that there is a destination / phenomenon / location that CIRRA and others have information about but have never fully understood or reached.

The player’s crew eventually becomes the group that successfully pieces the information together.

The PLAYER should solve the larger mystery.

Do not have CIRRA simply send:

“Congratulations, here are the coordinates.”

==================================================
27. CIRRA / THE COMPANY
==================================================

Keep CIRRA as a major organization, but change its role from commanding every individual mission.

CIRRA can be:

- a massive private organization
- government contractor
- infrastructure provider
- research organization
- recovery organization
- exploration organization
- hazardous-response organization

CIRRA can offer:

- contracts
- research requests
- salvage assignments
- survey requests
- investigation work

But the player controls where the ship travels.

Do not make CIRRA the central villain.

CIRRA can still have:

- unethical projects
- secrecy
- questionable priorities
- cover-ups
- expendable attitudes toward crews

But CIRRA should be one human organization inside a universe much larger than itself.

The final cosmic mystery should NOT ultimately be about CIRRA.

==================================================
28. HORROR DESIGN
==================================================

Project Predation should remain a horror game.

However, do NOT make every planet or location scary.

If every expedition becomes:

dark building
+
monster
+
chase

the horror will become predictable.

Instead, vary the emotional tone.

Some locations can be:

- safe
- beautiful
- abandoned
- eerie
- unsettling
- dangerous
- horrifying
- mysterious

The player should not always know what type of experience they are about to have.

A distress signal should NOT automatically mean:

“monster encounter.”

It could mean:

- equipment failure
- survivors
- no survivors
- an accident
- environmental disaster
- misleading information
- abandoned infrastructure
- something genuinely terrifying
- a story discovery

Use uncertainty as part of the horror.

==================================================
29. CREW WIPE / DEATH PENALTY
==================================================

A full squad wipe should matter, but should not erase major campaign progression.

The fiction is:

The crew died.

The ship and recoverable campaign infrastructure eventually return / are recovered.

A new crew takes over the same vessel and operational history.

Always preserve:

- ship
- ship upgrades
- ship customization
- discovered planets
- shared log
- story progression
- major permanent unlocks
- campaign knowledge

Potentially lose:

- equipment physically carried by the dead crew
- items gathered during that expedition
- unrecovered salvage
- temporary expedition items

A modest financial or recovery penalty may also exist.

Do NOT make the player lose dozens of hours of permanent progression.

The exact penalty can be tuned later.

==================================================
30. SAVING AND LOADING
==================================================

Create a proper campaign save/load system.

The host owns the campaign save.

The game should support:

- manual saves where appropriate
- autosaves
- campaign loading
- players leaving and returning later
- multiplayer rejoining the same campaign
- preserving meaningful campaign progression

The save system should preserve everything necessary to resume the campaign correctly.

Do not hard-code an overly narrow save schema.

The save architecture should be extensible as new systems are added.

==================================================
31. SAVE SIZE / PROCEDURAL STATE
==================================================

Keep save files efficient.

Do NOT save huge generated worlds if they can be reconstructed from deterministic seeds.

Prefer saving:

- universe seed
- system seed
- planet seed
- region seed
- discovered state
- changed state
- important persistent objects
- story state
- upgrades
- player / campaign progress

Then regenerate deterministic content when the save is loaded.

Store state deltas only where something meaningful has changed.

Example:

Do NOT serialize an entire large procedural facility if it can be recreated from:

facility seed
+
doors changed
+
items removed
+
major events completed

This is important for storage and performance.

==================================================
32. AUTOSAVING
==================================================

Autosave at sensible points such as:

- returning from an expedition
- completing an important objective
- purchasing a major upgrade
- reaching a major story milestone
- arriving at a safe location
- other meaningful transitions

Avoid excessive constant disk writes.

Saving should be reliable and asynchronous where practical so it does not cause noticeable gameplay stalls.

==================================================
33. MULTIPLAYER
==================================================

Continue using the existing host-authoritative peer-to-peer architecture.

Do not unnecessarily replace working networking systems.

The host is authoritative for:

- campaign state
- travel state
- world state
- ship state
- procedural generation seeds
- important progression

Shared systems should synchronize correctly.

This includes:

- system map
- travel
- ship upgrades
- ship appearance
- discovered planets
- shared log
- story state
- generated locations
- important persistent changes

The game must remain fully playable in single player.

Do not design any core mechanic that requires multiple players.

==================================================
34. PERFORMANCE / OPTIMIZATION
==================================================

This is a custom C++ engine and the game must remain practical to run on normal gaming hardware.

I want the game to look visually impressive.

But do not achieve that by creating systems that unnecessarily destroy performance.

Be careful with:

- procedural generation cost
- world streaming
- memory usage
- network bandwidth
- replication frequency
- save size
- shader complexity
- physics cost
- AI cost
- draw calls
- unnecessary active objects

Use:

- deterministic generation
- object pooling where useful
- LODs
- culling
- streaming
- asynchronous generation
- sensible update frequencies
- efficient replication
- profiling

The host should not need a huge server-class computer to run a four-player session.

==================================================
35. CINEMATIC CONTINUITY
==================================================

Keep the cinematic transition systems already planned.

The player should still physically experience:

departure
↓
travel
↓
arrival
↓
landing / docking
↓
exploration
↓
extraction
↓
return

Do not replace this with generic loading screens just because the player now chooses destinations.

When a player selects a planet:

- route is confirmed
- ship departs current location
- departure cinematic can play
- travel begins
- player remains aboard
- destination is approached
- arrival cinematic plays
- landing / docking transition occurs
- player enters the generated region

Use the existing cinematic editor / timeline architecture where appropriate.

==================================================
36. FINAL DESTINATION / ENDGAME
==================================================

Do NOT fully lock the ending lore yet.

The exact final destination is still being designed.

However, build the story architecture so the campaign can eventually lead to a unique final destination that:

- is difficult to identify / solve
- requires accumulated discoveries
- is reached by the player’s crew first
- is visually unlike normal space
- supports major custom shader / rendering effects
- feels beautiful and disturbing
- becomes a physically explorable endgame sequence
- reveals something much larger than normal human concerns
- is NOT ultimately about CIRRA

One current possible direction is:

The final region may turn out to be something living on an incomprehensibly large scale.

The player may initially interpret it as:

- terrain
- environment
- cosmic structure

and only gradually realize:

the environment itself is alive.

This is NOT final canon.

Treat this as a placeholder direction.

Do not build irreversible story assumptions around it yet.

==================================================
37. ENDGAME PRESENTATION
==================================================

Whatever the final destination becomes, reserve the ability to:

- use custom rendering rules
- use unusual shaders
- alter starfield presentation
- distort light
- use large-scale volumetrics
- change environmental rendering
- support reactive materials
- support unique endgame visual systems

The endgame should be able to visually look unlike anything else in the campaign.

Do not assume the entire universe must use identical rendering behavior.

==================================================
38. MAIN STORY COMPLETION / FREE PLAY
==================================================

When the main story is eventually completed:

final sequence
↓
credits
↓
return to campaign
↓
clear notification:

MAIN STORY COMPLETE

FREE PLAY UNLOCKED

Then allow the player to continue using the same campaign.

Preserve:

- ship
- upgrades
- exploration progress
- discovered worlds
- inventory / progression as appropriate
- shared log

The world should remain playable after the story.

Do NOT force a New Game+ just to continue exploring.

==================================================
39. EXTENSIBILITY
==================================================

This is extremely important.

Do not hard-code the game around the systems currently listed.

The architecture should make it possible to add later:

- new planet biomes
- new planet modifiers
- new weather
- new POIs
- new contracts
- new exploration activities
- new ship modules
- new ship upgrades
- new room types
- new story systems
- new horror encounters
- new landing region types
- new economic items
- new research systems
- new cinematic transitions
- new endgame content

Keep systems data-driven where practical.

==================================================
40. DEVELOPMENT PRIORITY
==================================================

Do not attempt to build every possible future feature at once.

Focus first on creating a strong vertical slice of the new loop:

1. start campaign
2. basic persistent ship
3. 3D system map
4. seeded planets
5. select destination
6. physical travel
7. landing region generation
8. several meaningful POI types
9. exploration / salvage / discovery
10. return to ship
11. basic economy
12. ship upgrade
13. save/load
14. multiplayer synchronization
15. shared log foundation

Once that works well, expand:

- biome variety
- additional POIs
- deeper progression
- contracts
- story discoveries
- advanced ship expansion
- additional horror systems
- endgame systems

==================================================
41. OVERALL DESIGN TARGET
==================================================

The new version of Project Predation should feel like:

“I have a ship.”

“I can look at the system and decide where I want to go.”

“That planet looks interesting.”

“We’re actually flying there.”

“We landed somewhere we’ve never seen before.”

“There’s something over there. Let’s investigate it.”

“We found something useful / strange / terrifying.”

“We made it back.”

“Our ship is getting better.”

“Now we can reach places that used to be impractical.”

“We keep finding information that seems connected.”

“There is something much larger going on.”

“We think we finally know where to go.”

The campaign should be driven by:

EXPLORATION
+
CURIOSITY
+
SHIP PROGRESSION
+
DISCOVERY
+
HORROR
+
MYSTERY

Do not reduce the experience to either:

resource grinding

or:

repetitive mission selection.

The player should keep thinking:

“What’s out there?”
