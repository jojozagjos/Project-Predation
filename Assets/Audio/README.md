# Audio

Almost every sound in the game is **synthesised at startup** from a recipe in
`Assets/Data/sounds.json`. That is why the repository ships with hardly any audio: the whole library
is a few hundred kilobytes of samples built in a few milliseconds, and it needs no licence, no
attribution and no download.

A recipe is a stand-in. The way it shows is repetition — the same gunshot, sample for sample, forty
times a magazine — and no recipe sounds like a real recording.

## Replacing one

Put `.wav` files in the folder named after the sound. That is the whole procedure:

    Assets/Audio/gunshot/carbine_01.wav
    Assets/Audio/gunshot/carbine_02.wav
    Assets/Audio/gunshot/carbine_03.wav

The game plays those instead of the recipe and picks a different one each time, so three recordings
of a gunshot stop a magazine sounding like a loop. An empty folder falls back to the recipe, so
deleting the files puts the old sound back.

Nothing has to be listed anywhere and nothing has to be rebuilt. The folder is the registration —
the point being that somebody who does not build the game can still change its audio.

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
