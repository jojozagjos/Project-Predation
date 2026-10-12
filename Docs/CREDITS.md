# Credits and third-party content

Everything in this repository that somebody else made, and what is known about the terms it came
under. Anything whose terms are not confirmed is marked so, because "it was free to download" and
"it is free to ship in a game" are different statements and only one of them is safe to act on.

## Audio

### Footstep clips — `Assets/Audio/Footsteps/`

Sixty-four clips over eleven surfaces (`Assets/Data/footsteps.json`), all **CC0** (public domain: no
conditions, no attribution required; credited here anyway). Each was made mono 48 kHz, its lead-in
silence cut, capped at 0.6 s with a fade, and loudness-matched; metal, dirt and mud are two clips mixed.

| Source | Licence | Used for |
|---|---|---|
| **Kenney, Impact Sounds** (kenney.nl/assets/impact-sounds) | CC0 (licence file in the download) | concrete, wood, snow, grass; carpet steps under dirt; grass steps under mud; plate impacts in metal |
| **Fantozzi, Footsteps (Grass/Sand & Stone)** (opengameart.org) | CC0 | stone, sand; the grit in dirt |
| **Corsica_S, 42 Snow and Gravel Footsteps**, extracted by Iwan Gabovitch (opengameart.org) | CC0, with the recordist's permission (its readme) | gravel (snow-covered gravel), ice (gravel and ice) |
| **TinyWorlds, Different steps on wood, stone, leaves, gravel and mud** (opengameart.org) | CC0 | one stone and one wood clip |
| **25 CC0 mud sfx** (opengameart.org) | CC0 | the squelch in mud |

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
