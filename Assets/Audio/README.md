# Audio

Every sound in the game is a `.wav` file in this folder. Nothing is generated at run time.

The files here now are **placeholders**, and they sound like it. They were baked out of the old
synthesiser — the one that used to build the whole library from a recipe in `Assets/Data/sounds.json`
at startup — so the game sounds exactly as it did, with the difference that every sound is now a
thing you can open, listen to, and drop a better one on top of.

## Replacing one

Put `.wav` files in the folder named after the sound. That is the whole procedure:

    Assets/Audio/gunshot/carbine_01.wav
    Assets/Audio/gunshot/carbine_02.wav
    Assets/Audio/gunshot/carbine_03.wav

Delete the placeholder or leave it — the game plays **every** wav in the folder and picks a
different one each time, which is the whole of how a sound stops repeating. Three recordings of a
gunshot stop a magazine sounding like a loop. An empty folder means that sound is silent, and the
log says which folder was empty.

Any rate, any bit depth, any channel count: 8, 16, 24 and 32 bit PCM and 32 bit float are all read,
and anything with more than one channel is averaged down to mono, because the mixer decides which
ear a sound belongs in from where it is in the world. Trailing silence is trimmed automatically.

Nothing has to be listed anywhere and nothing has to be rebuilt. The folder is the registration —
the point being that somebody who does not build the game can still change its audio.

## Regenerating the placeholders

The placeholders are designed in `Assets/Data/sound_design.json`: each sound is a few layers -- noise,
a tone, a voice, a click -- each with its own envelope, pitch that moves, filter that moves, and
repeats, summed and given a room. `sound_bake` in the console renders every one into
`<name>/<name>_<take>.wav`, three or four takes each so nothing repeats; `sound_bake <name>` does one.
Only files named that way are overwritten, so a recording dropped in under any other name survives a
bake. The game never reads the design, only the files. See `Engine/Audio/SoundDesign.h` for every field.

## What goes where

Every folder is a sound, by its name. The game plays a different file from the folder each time.

**Weapons**

| Folder | When it plays |
|---|---|
| `gunshot_sidearm`, `gunshot_carbine` | A round leaves that weapon: `gunshot_<weapon key>` from `weapons.json` |
| `gunshot` | A round leaves a weapon with no folder of its own |
| `dry_fire` | The trigger pulled with nothing in the magazine and nothing to reload |
| `reload_out`, `reload_in`, `slide_rack` | Through a reload, at the moments `reload_sounds` in `weapons.json` names |
| `weapon_draw`, `weapon_holster` | A weapon brought up, or put away |
| `shell_casing` | The brass hitting the floor, a moment after each shot |
| `impact_hard`, `impact_flesh` | Where a round lands: on something solid, or in something alive |

**Players**

| Folder | When it plays |
|---|---|
| `step_hard` | A footstep, where the surface has no clips of its own |
| `jump`, `land` | Leaving the ground on purpose, and hitting it again from a height |
| `hurt`, `death` | Anybody taking damage, and dying: in your own head when it is you |
| `breath_exhausted` | Out of breath after running until there is nothing left |
| `struggle` | Fighting a creature's grip |
| `torch_on`, `torch_off` | The torch switched |

**The world**

| Folder | When it plays |
|---|---|
| `door`, `door_locked` | A door swings; a locked one is tried |
| `door_bash`, `door_slam` | A creature throwing itself at a locked door, and the door giving way |
| `locker` | Somebody gets into or out of a locker |
| `pickup`, `drop` | An item taken, or put down |
| `crate_open`, `ammo_take`, `crate_close` | An ammunition crate opened and drawn from, and its lid falling shut |
| `nest_build`, `cocoon_wrap`, `cocoon_cut` | A nest finished; somebody wrapped up at one; somebody cut free |

**The creature**

| Folder | When it plays |
|---|---|
| `creature_step_light`, `creature_step_heavy` | Each foot as it comes down, heavy for anything over 170 kg |
| `creature_breath` | Every few seconds while alive and not playing dead; quicker after running |
| `creature_growl` | Now and then while it means somebody harm |
| `creature_chitter` | Now and then while it is looking into something, or wandering |
| `creature_call` | Rearing up to call the others |
| `creature_display` | Rearing up to warn somebody off its ground |
| `creature_swipe`, `creature_bite`, `creature_lunge`, `creature_grab` | Each blow, as it starts |
| `creature_hurt`, `creature_death` | Wounded, and killed |

A small creature is played higher and a large one lower, from the same files.

**The building**

| Folder | When it plays |
|---|---|
| `amb_room_tone`, `amb_vent` | Always, looped, under everything (`audio.ambience` sets how loud) |
| `amb_groan`, `amb_distant_bang`, `amb_drip` | Every twenty to fifty seconds, somewhere out of sight |
| `amb_electric` | The nearest failing or flickering lamp, from where it hangs, stuttering with its light |

**Menus**

| Folder | When it plays |
|---|---|
| `ui_hover`, `ui_click`, `ui_back`, `ui_confirm` | The front end |

Footsteps are the exception and already work this way: `Assets/Audio/Footsteps/` holds a set per
surface, listed in `Assets/Data/footsteps.json`, because which surface is underfoot decides which
set is used.

## Format

Mono or stereo `.wav`, 16 or 24 bit PCM or 32 bit float, any sample rate — it is resampled on load
and downmixed to mono, because the mixer positions a sound in the world and a stereo file has
already decided something that is not its business.

Trailing silence is trimmed automatically. Packs usually carry some, and it delays every sound
queued behind it by however long it is.

## Licensing

Anything added here has to be cleared for use and recorded in `Docs/CREDITS.md`. A sound with no
licence file is not cleared, whatever the page it came from said.
