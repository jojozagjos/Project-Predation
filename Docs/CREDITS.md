# Credits and third-party content

Everything in this repository that somebody else made, and what is known about the terms it came
under. Anything whose terms are not confirmed is marked so, because "it was free to download" and
"it is free to ship in a game" are different statements and only one of them is safe to act on.

## Audio

### Footstep clips — `Assets/Audio/Footsteps/<surface>/`

Seventy-six clips over eleven surfaces (`Assets/Data/footsteps.json`), each surface in a folder of its own. Each
was made mono 48 kHz, its lead-in silence cut, capped at 0.6 s with a fade, and loudness-matched; dirt and mud are
two clips mixed.

| Source | Licence | Used for |
|---|---|---|
| **Kenney, Impact Sounds** (kenney.nl/assets/impact-sounds) | CC0 (licence file in the download) | concrete, wood, snow, grass; carpet steps under dirt; grass steps under mud |
| **Ali_6868, Gravel Footsteps** (freesound.org pack 21608), as cut in congusbongus' "Footsteps on different surfaces" (opengameart.org) | CC0 | gravel |
| **Eelke, "fboots on aluminum ladder 01"** (freesound.org/people/Eelke/sounds/462598), as cut in congusbongus' "Footsteps on different surfaces" (opengameart.org) | **CC-BY 3.0: credit required** | metal |
| **Fantozzi, Footsteps (Grass/Sand & Stone)** (opengameart.org) | CC0 | stone; the grit in dirt |
| **felix.blume, "Footsteps wearing sneakers walking on sand gravel pathway in the desert"** (freesound.org/people/felix.blume/sounds/705745), its preview, cut into single steps (high-passed to take out the road hum) | CC0 | sand |
| **Corsica_S, 42 Snow and Gravel Footsteps**, extracted by Iwan Gabovitch (opengameart.org) | CC0, with the recordist's permission (its readme) | snow (snow-covered gravel), ice (gravel and ice) |
| **TinyWorlds, Different steps on wood, stone, leaves, gravel and mud** (opengameart.org) | CC0 | one stone and one wood clip |
| **25 CC0 mud sfx** (opengameart.org) | CC0 | the squelch in mud |
| **100 CC0 SFX #2** (opengameart.org) | CC0 | wet steps in mud |

**The metal clips are CC-BY 3.0**: the game must credit them where players can see it -- "Footsteps on metal: Eelke
(freesound.org), adapted by congusbongus (opengameart.org), CC-BY 3.0" -- in an in-game credits screen or a credits
file shipped with it. There is no credits screen yet; this needs doing before any release.

These replace the JDSherbert Footstep Foley pack, whose terms were never confirmed. The synthesised
footstep is still in `Assets/Data/Sounds/Player.json` as `step`, for when no clips load.

## Code

Dependencies are declared in `vcpkg.json` and their licences are the ones vcpkg records. All of them
are permissive. The one worth naming is **Opus** (BSD three-clause), which compresses voice: it is
the codec every voice application uses and there is no obligation beyond keeping the notice.

**libjuice** (MPL-2.0) was here and is gone. It did hole punching; the game now does its own, simpler
kind through the lobby server (ADR-067), so the only copyleft obligation in the project went with it.

## Everything else

Models, textures and all other assets in this repository are original to the project.
