# Audio

Every sound in the game is a `.wav` file in this folder. Nothing is generated at run time.

Sounds are sorted by what makes them, one folder per category and one folder per sound inside it:

    Assets/Audio/
      Weapons/     shots, magazines, casings, impacts
      Player/      steps, landings, breath, pain
      World/       doors, lockers, crates, things picked up and dropped
      Items/       using what is in your hand: a medical kit, a battery, a keycard, a flare
      Nest/        a nest growing, its heart, cocoons
      Creature/    everything the creature does with its feet, lungs and throat
      Ambience/    the building itself: loops for each kind of place, and things heard far off
      UI/          the menus
      Footsteps/   recorded steps, one set per surface (listed in Assets/Data/footsteps.json)

The game knows a sound by its category and name together -- `Creature/growl` is
`Assets/Audio/Creature/growl/` -- so two categories can each have a `hurt` and never be confused.

The files here are **placeholders**, rendered from the designs in `Assets/Data/Sounds` (see below). They
are a stand-in until there are recordings, and every one of them can be replaced without building
anything.

## Replacing one

Put `.wav` files in the sound's folder. That is the whole procedure:

    Assets/Audio/Weapons/carbine_shot/carbine_01.wav
    Assets/Audio/Weapons/carbine_shot/carbine_02.wav
    Assets/Audio/Weapons/carbine_shot/carbine_03.wav

Delete the placeholders or leave them -- the game plays **every** wav in the folder and picks a
different one each time, which is the whole of how a sound stops repeating. An empty folder means that
sound is silent, and the log says which folder was empty.

A new sound is a new folder. Nothing has to be listed anywhere and nothing has to be rebuilt: the
folder is the registration, so somebody who does not build the game can still change its audio.

## Regenerating the placeholders

Each placeholder is designed in `Assets/Data/Sounds/<Category>.json` -- one file for each folder here, `Weapons.json` for `Weapons/` and so on, each sound under its own name -- as a few layers -- noise, a tone, a
voice, a click -- each with its own envelope, moving pitch, moving filter and repeats, summed and given
a room. `sound_bake` in the console renders every one into `Category/sound/sound_<take>.wav`, three or
four takes each; `sound_bake Creature/growl` does one. Only files named that way are overwritten, so a
recording dropped in under any other name survives a bake. See `Engine/Audio/SoundDesign.h` for every
field.

## What goes where

**Weapons**

| Folder | When it plays |
|---|---|
| `sidearm_shot`, `carbine_shot` | A round leaves that weapon: `<weapon key>_shot`, from `weapons.json` |
| `shot` | A round leaves a weapon with no folder of its own |
| `dry_fire` | The trigger pulled with nothing in the magazine and nothing to reload |
| `mag_out`, `mag_in`, `slide` | Through a reload, at the moments `reload_sounds` in `weapons.json` names |
| `draw`, `holster` | A weapon brought up, or put away |
| `casing` | The brass hitting the floor, a moment after each shot |
| `impact_hard`, `impact_flesh` | Where a round lands: on something solid, or in something alive |

**Player**

| Folder | When it plays |
|---|---|
| `step` | A footstep, where the surface has no recordings in `Footsteps` |
| `jump`, `land` | Leaving the ground on purpose, and hitting it again from a height |
| `hurt`, `death` | Anybody taking damage, and dying: in your own head when it is you |
| `out_of_breath` | After running until there is nothing left |
| `struggle` | Fighting a creature's grip |
| `torch_on`, `torch_off` | The torch switched |

**World**

| Folder | When it plays |
|---|---|
| `door`, `door_locked` | A door swings; a locked one is tried |
| `door_bash`, `door_slam` | A creature throwing itself at a locked door, and the door giving way |
| `locker` | Somebody gets into or out of a locker |
| `pickup`, `drop` | An item taken, or put down |
| `crate_open`, `ammo_take`, `crate_close` | An ammunition crate opened and drawn from, and its lid falling shut |

**Items**

| Folder | When it plays |
|---|---|
| `medkit_open`, `medkit_apply` | A medical kit opened, and put to use |
| `battery_out`, `battery_in` | The old cell out of the torch, and the new one in |
| `keycard_swipe`, `keycard_accept` | A keycard swiped, and a locked door accepting it |
| `flare_strike`, `flare_throw` | A flare struck, and thrown |
| `flare_burn` | A thrown flare burning where it lies, looping until it is spent |
| `inspect` | A sample container turned over in the hands |

**Nest**

| Folder | When it plays |
|---|---|
| `grow` | A nest taking hold somewhere |
| `heartbeat` | The nest's heart, beating, from where it hangs |
| `heart_hurt`, `heart_burst` | The heart shot, and the heart destroyed |
| `cocoon_wrap`, `cocoon_cut` | Somebody wrapped up at a nest; somebody cut free |

**Creature**

| Folder | When it plays |
|---|---|
| `step_light`, `step_heavy` | Each foot as it comes down, heavy for anything over 170 kg |
| `climb` | Claws on a wall or a ceiling |
| `breath` | Every few seconds while alive and not playing dead; quicker after running |
| `growl` | Now and then while it means somebody harm |
| `chitter` | Now and then while it is looking into something, or wandering |
| `call` | Rearing up to call the others |
| `warning` | Rearing up to warn somebody off its ground |
| `swipe`, `bite`, `lunge`, `grab` | Each blow, as it starts |
| `hurt`, `death` | Wounded, and killed |

A small creature is played higher and a large one lower, from the same files.

**Ambience**

The loops are faded in and out by where you are, measured a few times a second:

| Folder | Heard |
|---|---|
| `room_tone`, `vent` | Indoors: the hum of the place and the air in its ducts |
| `wind` | Outside, and a breath of it indoors |
| `vent_close` | Inside a vent or the crawlspace, where the fan is right there |
| `nest` | Near a nest, before you see it |
| `buzz` | The nearest failing or flickering lamp, from where it hangs, stuttering with its light |
| `groan`, `drip`, `distant_bang` | Now and then, somewhere out of sight: a building's noises indoors, only something falling far off outside |

`audio.ambience` sets how loud all of it is.

**UI**

| Folder | When it plays |
|---|---|
| `hover`, `click`, `back`, `confirm` | The front end |

## Format

Mono or stereo `.wav`, 8, 16, 24 or 32 bit PCM or 32 bit float, any sample rate -- it is resampled on
load and downmixed to mono, because the mixer positions a sound in the world and a stereo file has
already decided something that is not its business.

Trailing silence is trimmed automatically. Packs usually carry some, and it delays every sound queued
behind it by however long it is.

## Loudness

Recordings are best left with their natural dynamics rather than normalised hard. The mixer leaves
headroom and has a limiter on its output, which turns the whole mix down for a moment when something
very loud happens -- it never bends the waveform -- so a gunshot can be a gunshot without the footsteps
having to be shouted.

## Licensing

Anything added here has to be cleared for use and recorded in `Docs/CREDITS.md`. A sound with no licence
file is not cleared, whatever the page it came from said.
