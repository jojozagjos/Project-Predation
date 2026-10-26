# Credits and third-party content

Everything in this repository that somebody else made, and what is known about the terms it came
under. Anything whose terms are not confirmed is marked so, because "it was free to download" and
"it is free to ship in a game" are different statements and only one of them is safe to act on.

## Audio

### Footstep clips — `Assets/Audio/Footsteps/<surface>/`

Sixty-eight clips over ten surfaces (`Assets/Data/footsteps.json`), each surface in a folder of its own. Each
was made mono 48 kHz, its lead-in silence cut, capped at 0.6 s with a fade, and loudness-matched; dirt is
two clips mixed.

| Source | Licence | Used for |
|---|---|---|
| **Kenney, Impact Sounds** (kenney.nl/assets/impact-sounds) | CC0 (licence file in the download) | concrete, wood, snow, grass; carpet steps under dirt |
| **Ali_6868, Gravel Footsteps** (freesound.org pack 21608), as cut in congusbongus' "Footsteps on different surfaces" (opengameart.org) | CC0 | gravel |
| **Eelke, "fboots on aluminum ladder 01"** (freesound.org/people/Eelke/sounds/462598), as cut in congusbongus' "Footsteps on different surfaces" (opengameart.org) | **CC-BY 3.0: credit required** | metal |
| **Fantozzi, Footsteps (Grass/Sand & Stone)** (opengameart.org) | CC0 | stone; the grit in dirt |
| **felix.blume, "Footsteps wearing sneakers walking on sand gravel pathway in the desert"** (freesound.org/people/felix.blume/sounds/705745), its preview, cut into single steps (high-passed to take out the road hum) | CC0 | sand |
| **Corsica_S, 42 Snow and Gravel Footsteps**, extracted by Iwan Gabovitch (opengameart.org) | CC0, with the recordist's permission (its readme) | snow (snow-covered gravel), ice (gravel and ice) |
| **TinyWorlds, Different steps on wood, stone, leaves, gravel and mud** (opengameart.org) | CC0 | one stone and one wood clip |

**The metal clips are CC-BY 3.0**: the game must credit them where players can see it -- "Footsteps on metal: Eelke
(freesound.org), adapted by congusbongus (opengameart.org), CC-BY 3.0" -- in an in-game credits screen or a credits
file shipped with it. There is no credits screen yet; this needs doing before any release.

These replace the JDSherbert Footstep Foley pack, whose terms were never confirmed. The synthesised
footstep is still in `Assets/Data/Sounds/Player.json` as `step`, for when no clips load.

### The player out of breath -- `Assets/Audio/Player/out_of_breath/out_of_breath.wav`

**ceberation, "Heavy Breathing"** (freesound.org/people/ceberation/sounds/235519), made mono and brought up to the
other sounds' level. **CC-BY 3.0: credit required** -- "Heavy breathing: ceberation (freesound.org), CC-BY 3.0" -- in the
in-game credits with the metal footsteps. It replaces the synthesised placeholder.

### The player hurt -- `Assets/Audio/Player/hurt/`

**HaelDB, "Male Grunt/Yelling sounds"** (opengameart.org/content/male-gruntyelling-sounds), dual-licensed **CC0** and OGA-BY
3.0 -- used under CC0 (no credit required, credited anyway). One grunt (`3grunt1`), its silence cut, made mono 48 kHz, a
little muffled as heard in a helmet, faded at its ends and loudness-matched to the sounds round it. It replaces the
synthesised placeholders and their recipe.

### Weather -- `Assets/Audio/Weather/`

All **CC0** (public domain dedication: no credit required, credited anyway), from freesound.org, taken from each sound's
high-quality preview; made mono 48 kHz, cut, levelled, and the loops crossfaded end into start.

| Recording | Used for |
|---|---|
| **director89, "Rain-Atmo.WAV"** (freesound.org/people/director89/sounds/265627) | `rain` |
| **Rubaoliva, "Heavy_Rain_Loop.wav"** (freesound.org/people/Rubaoliva/sounds/624645) | `rain_heavy` |
| **kyles, "rain light on tin roof soft.flac"** (freesound.org/people/kyles/sounds/451152) | `rain_roof` |
| **conleec, "AMB_Int_Rain_Auto_001.wav"** (freesound.org/people/conleec/sounds/149244) | `rain_cabin` |
| **dhallcomposer, "Looping Gentle Wind Ambience on an Open Desert Plain.wav"** (freesound.org/people/dhallcomposer/sounds/697217) | `wind` |
| **keirofinch, "Strong Wind Recorded with Soundfield Microphone Manchester 2"** (freesound.org/people/keirofinch/sounds/376534) | `gale` |
| **craigsmith, "G56-22-Sandstorm.wav"** (freesound.org/people/craigsmith/sounds/438877) | `sandstorm` |
| **cobratronik, "Wind__Artic__Cold.wav"** (freesound.org/people/cobratronik/sounds/117136) | `blizzard` |
| **klangfabrik, "winter wind 01c.aiff"** (freesound.org/people/klangfabrik/sounds/117513) | `blizzard` |
| **Fission9, "Huge Thunderclap"** (freesound.org/people/Fission9/sounds/576041) | `thunder_near`, and low-passed for `thunder` far off |
| **BlueDelta, "Heavy Thunder Strike - no Rain - QUADRO.wav"** (freesound.org/people/BlueDelta/sounds/446753) | `thunder_near`, and low-passed for `thunder` far off |
| **lennyboy, "Thunder.ogg"** (freesound.org/people/lennyboy/sounds/244053) | `thunder` |
| **Dave Welsh, "Thunder Clap OWB KY 441x16.wav"** (freesound.org/people/Dave%20Welsh/sounds/194364) | `thunder` |

## Code

Dependencies are declared in `vcpkg.json` and their licences are the ones vcpkg records. All of them
are permissive. The one worth naming is **Opus** (BSD three-clause), which compresses voice: it is
the codec every voice application uses and there is no obligation beyond keeping the notice.

**libjuice** (MPL-2.0) was here and is gone. It did hole punching; the game now does its own, simpler
kind through the lobby server (ADR-067), so the only copyleft obligation in the project went with it.

## Everything else

Models, textures and all other assets in this repository are original to the project.

### Water -- `Assets/Audio/Ambience/shore/`, `Assets/Audio/World/splash/`, `Assets/Audio/Player/wade/`

From opengameart.org, made mono 48 kHz and levelled. The shore loop is the eight waves end to end, crossfaded, its end
crossfaded into its start. They replace the synthesised placeholders (whose designs are taken out of
`Assets/Data/Sounds`, so a bake does not bring them back).

| Recording | Licence | Used for |
|---|---|---|
| **jasinski, "Beach Ocean Waves"** (freesound.org/people/jasinski/sounds/18363), cut by qubodup (opengameart.org/node/11174) | CC0 | `Ambience/shore` |
| **transitking, "Water Waves"** (freesound.org/people/transitking/sounds/11505), cut by qubodup (opengameart.org/content/water-waves) | CC0 | `Ambience/shore` |
| **Peludo, "Water Splash and sand footsteps"** (opengameart.org/content/water-splash-and-sand-footsteps), splash1 and splash2 | CC0 (the author asks that games reference their itch.io page; the page gives no address -- find it before a release) | `World/splash` |
| **Blender Foundation, "Water Step Splashes (Yo Frankie!)"**, submitted by Lamoot (opengameart.org/content/water-step-splashes-yo-frankie) | **CC-BY 3.0: credit required** | `Player/wade` |

**The wading steps are CC-BY 3.0**: credit them with the others that need it -- "Wading steps: Blender Foundation (Yo
Frankie!), via opengameart.org, CC-BY 3.0".
