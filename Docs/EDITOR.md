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
- **Clips.** Named animations with keyframes for the parts and the hands. A clip called `reload` is
  the reload: the hands, the magazine and the weapon all move as it says, and it lasts as long as it
  is.

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

1. Make a clip and name it, or start from a template (below).
2. Set the duration.
3. Move the playhead, pose the parts, and press **Key all parts**.
4. Repeat at each moment that matters.
5. Play it back, and scrub to check the in-between.

The model in front of you is the animation as it will play, and with **Follow the timeline** ticked in
the *Hold it* panel the first-person view plays it too, at the playhead: scrub the timeline and the
hands move in front of you.

### Hands

A reload is animated by hand, hands included. Add a track called **hand_left** or **hand_right** from
*Animate part*. While a clip with a hand track plays, that hand does exactly what its keys say and
nothing written into the game second-guesses it.

- A hand's key is how far it has moved from the socket it rests on (`support` for the left, `grip`
  for the right), in the weapon's frame, and how the wrist is turned. Zero is the hand on the gun, so
  start and end a clip there.
- The hands are drawn in the editor view while the clip moves them -- blue for the left, orange for the
  right -- with a line back to the socket they rest on.
- Select a track in the timeline and the move handles move its key at the playhead, so a hand or a part
  can be posed by dragging it to where it should be.

### Who holds a part

Every key on a part says what is carrying it from that moment: **the weapon**, **the left hand** or
**the right hand** (*Held by*, under the key). Held by a hand, the part goes wherever that hand goes.
Changing it keeps the part exactly where it is on screen, so the usual pattern is:

1. Key the hand onto the magazine.
2. At that same moment, key the magazine and set it to **Held by: the left hand**.
3. Move the hand; the magazine comes with it.
4. When the hand has put it back where it rests, key it **Held by: the weapon**.

*Part is there* hides a part from a key on -- a magazine dropped into a pouch, a fresh one out of it.

### Templates

**Reload**, **Reload from empty** and **Double magazine** make a whole clip that already moves: the
hand to the magazine, the magazine out and into a pouch, a fresh one in, the weapon tilting to meet
it. They measure the model -- where the support hand rests, where the magazine is and how tall -- so
they fit whatever gun is open. They need a part called `magazine`.

- **Reload from empty** ends with the hand hitting the bolt catch, and moves a part called `bolt` from
  locked back to home if there is one.
- **Double magazine** is two magazines taped side by side, one upside down: out, roll the pair over,
  in. It adds `magazine_2` beside the magazine if the model has none. The pair is the same shape the
  other way up, so the reload ends with the model looking exactly as it started.

Each replaces the clip of the same name, and Undo brings the old one back.

## Clips the game plays

Name a clip one of these and the game plays it at the right moment. Its progress comes from the
simulation's own timers, so a clip plays at exactly the speed the weapon works at.

| Clip | When it plays |
| --- | --- |
| `reload` | while the magazine is being changed |
| `reload_empty` | the same, when the magazine ran dry; `reload` plays if there is none |
| `equip` | while the weapon is being brought up |
| `unequip` | while it is put away; `equip` backwards if there is none |
| `fire` | after each shot, on top of the recoil |

A reload lasts exactly as long as its clip: the clip's duration *is* the reload time, so the moment the
magazine goes in on screen and the moment the gun can fire again are the same moment. (With no clip,
`reload_seconds` and `reload_empty_seconds` in `weapons.json` say how long.)

A track named `root` moves the whole weapon rather than one part, in the frame it is carried in: a
reload's tilt towards the magazine, the jolt as the bolt goes home, an equip.

A model with no clip of a given name falls back to movement written in code, so a weapon works before
anything has been authored for it.

## Not yet

Turning with a handle in the viewport rather than by typing numbers; a dropped magazine that falls
to the floor as a real object rather than disappearing into a pouch; and mirroring a clip from one
hand to the other.
