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

`sounds.json` and the synthesiser are still in the engine, reached only by the `sound_bake` console
command. Run it and every recipe is written back out as `<name>/<name>_1.wav`, overwriting what is
there. That is the only thing that still reads those recipes; the game itself never does.

## What goes where

| Folder      | When it plays                                          |
|-------------|--------------------------------------------------------|
| `gunshot`   | A round leaves the barrel                              |
| `dry_fire`  | The trigger is pulled on an empty weapon               |
| `reload_out`| The magazine leaves the weapon                         |
| `reload_in` | The fresh magazine seats                               |
| `step_hard` | A footstep, where the surface has no clips of its own  |
| `land`      | Landing from a fall, louder the harder the landing     |
| `door`      | A door swings                                          |
| `locker`    | Somebody gets into or out of a locker                  |
| `pickup`    | An item is taken                                       |
| `drop`      | An item is put down                                    |
| `hurt`      | The player takes damage                                |
| `death`     | The player dies                                        |

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
