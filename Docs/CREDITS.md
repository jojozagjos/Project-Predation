# Credits and third-party content

Everything in this repository that somebody else made, and what is known about the terms it came
under. Anything whose terms are not confirmed is marked so, because "it was free to download" and
"it is free to ship in a game" are different statements and only one of them is safe to act on.

## Audio

### Footstep clips — `Assets/Audio/Footsteps/`

**JDSherbert - Footstep Foley SFX Pack (FREE)**, mono 48 kHz WAV. Ten clips are used: two each of
concrete, stone, metal, gravel and wood, renamed and otherwise unmodified.

**Licence: not yet confirmed.** The download contains no licence file, no readme and no terms of any
kind — only audio. The pack is distributed as free, which says what it costs and not what may be
done with it. Before any public release, get the terms in writing from wherever it was downloaded
and record them here, including whether attribution is required and in what form.

If the terms turn out not to allow it, this is a small thing to undo: the clips are named in
`Assets/Data/footsteps.json` and nothing else refers to them, and the synthesised footstep they
replaced is still in `Assets/Data/sounds.json` and still works.

## Code

Dependencies are declared in `vcpkg.json` and their licences are the ones vcpkg records. The one
worth naming here is **libjuice** (MPL-2.0), which does the NAT traversal: MPL-2.0 is file-level
copyleft, so linking it is fine and modifying its own source would require publishing those changes.
We do not modify it.

## Everything else

Models, textures and all other assets in this repository are original to the project.
