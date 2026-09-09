# Model and animation editor

**Status**: built, and the loop from editor to game is closed. Weapons already load their models
from it. Items and creatures are the obvious next users.

## Opening it

Type `editor` in the console, or `editor <model name>` to open one straight away. Type `editor`
again to close it and go back to playing.

Right mouse to look, WASD to move, Space and Left Ctrl for up and down.

## Why it runs inside the game

It draws through the same renderer, the same shader and the same lighting the world uses, so what
is built here is what appears in the player's hands. There is no export step, no second definition
of what a model is, and nothing that can look right in a tool and wrong in the game.

## What a model is

`Assets/Models/<name>.json`. Three things:

- **Parts.** Each is a box, cylinder, sphere or an imported mesh, with a position, rotation, size
  and material. Parts are separate entities in the world rather than one baked mesh, which is what
  lets a reload take the magazine out.
- **Sockets.** Named points: `grip`, `support`, `muzzle`, `magazine`, `sight`. These are the
  contract between a model and whatever holds it. Moving the support socket moves the hand that
  holds the handguard; moving the sight socket changes where the weapon sits when the sights come
  up. A missing socket falls back to something derived from the weapon's footprint, so a
  half-finished model still works.
- **Clips.** Named animations with per-part keyframes. A clip called `reload` plays during a reload,
  driven by the simulation's own timer, so it works at whatever reload time the weapon has.

Offsets in a clip are relative to the part's rest pose. Editing the model does not invalidate a clip
that was authored against it.

## Getting started from what exists

The weapons already in the game were built from their numbers in C++. To edit one:

```
model_export carbine
editor carbine
```

That writes `Assets/Models/carbine.json` with every part and socket the built-in shape has, then
opens it. `weapons.json` already names both models, so saving from the editor and restarting shows
the change in your hands.

A weapon with no `model` field falls back to the built-in shape, so adding a weapon to
`weapons.json` still produces something usable before anyone has opened the editor.

## Importing a downloaded model

Paste a path to a `.obj` file into the import box. Sketchfab offers OBJ on most downloads.

Positions, normals and faces are read. Materials, textures and anything rigged are not, which suits
the placeholder look the game has now. A face with more than three corners is fanned into triangles,
and missing normals are generated from the winding.

An imported mesh becomes a part like any other, so it can be moved, scaled and animated beside
primitives. Its vertices are stored inside the model file rather than referenced by path, so a model
is one self-contained thing.

## Animating

1. Make a clip and name it. `reload` is the one the game plays today.
2. Set the duration.
3. Move the playhead, pose the parts, and press **Key all parts**.
4. Repeat at each moment that matters.
5. Play it back, and scrub to check the in-between.

The model in front of you is the animation as it will play. There is no separate preview mode.

## Clips the game plays

Name a clip one of these and the game plays it at the right moment. Its progress comes from the
simulation's own timers, so a clip works at whatever reload or draw time the weapon has.

| Clip | When it plays |
| --- | --- |
| `reload` | while the magazine is being changed |
| `equip` | while the weapon is being brought up |

A track named `root` moves the whole weapon rather than one part, in the frame it is carried in.
That is what an equip needs: every other track in a clip moves a part relative to the weapon, and
bringing a weapon up moves the weapon relative to the hands.

A model with no clip of a given name falls back to movement written in code, so a weapon works
before anything has been authored for it.

## Not yet

Rotating and scaling with a handle in the viewport rather than by typing numbers; undo; glTF
import; per-part parenting; and clips for holstering and firing.
