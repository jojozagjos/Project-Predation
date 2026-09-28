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

## Grouping and joining parts

A downloaded model often arrives in many pieces. A magazine might be a body, a see-through window and
a base plate. Animate only the body and the other pieces stay in the gun. Two buttons under the part
list deal with this:

- **Group...** ticks the parts that move with the selected one. Grouped parts are listed under it,
  indented. You can set the same thing one part at a time with **Moves with** in the part inspector.
  Grouping never moves anything by itself: each part stays where it was placed. From then on, whatever
  moves the group's main part carries the others along, including a hand holding it. Hiding the main
  part hides the whole group. A part with its own hand key follows that hand instead.
- **Join...** merges the ticked parts into the selected one for good, as a single mesh. Use it for
  pieces that never come apart. The joined part keeps its own colour and texture. Undo splits them
  again.

Renaming a part keeps its group and its animation tracks attached to the new name. Deleting a part
releases anything grouped under it.

The carbine's magazine window (`Object_48_polycarbonate`) is grouped with `magazine`, so it now
leaves with the magazine during a reload.

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

## Items in the hand

The *Hold it* panel holds anything, not only the model open in the editor: choose an item from **In
the hands** and the body holds it the way the game will. **Offset** and **Turn** place it in the hand.

An item that can be used has a **Use** section: **Play** runs the use through at its real speed, the
scrubber beside it stands the hand anywhere in it, and each key -- how far through, where the hand has
the item, and how it is turned -- can be dragged, jumped to with **go**, or removed with **x**. **Add a
key where the scrubber is** puts a new one at the scrubber, starting from wherever the hand already is
there. A flare has a second half, the throw, behind **The second half (thrown)**. **Write to
items.json** saves the placement and the motions; see [ITEMS.md](ITEMS.md) for what the numbers mean.

## Turning and mirroring

Around whatever is selected -- a part, a socket, or a key under the playhead -- are three rings as well
as the three move handles, one round each axis in the same colours, out beyond the handles so the two
never get in each other's way. Drag round a ring and the selection turns about that axis, in five-degree
steps while snapping is on. A key held in a hand is turned in that hand's frame, as it is moved. Each
turn is one undo.

**Mirror hands**, beside **Key all parts**, swaps the hands in the open clip and reflects them across
the weapon: what the left hand did the right does, and every part a hand held goes to the other hand.
One undo takes it back.

## Not yet

A dropped magazine that falls to the floor as a real object rather than disappearing into a pouch.

# Cinematic editor

For the sequences between the parts of a deployment: the shuttle coming down, the crawler driving to the
building, the title card, the doors opening. What a cinematic is, and why it is written against anchors rather
than coordinates, is in DECISIONS.md (ADR-103); this is how to edit one. Development builds only.

## Opening it

Be somewhere first -- `facility 1` puts you at a snow site -- then type `cine_edit surface_insertion` (or just
`cine_edit` to edit whichever is playing, or to be asked which). `cine_edit` again closes it; so does Escape,
which asks twice if there are changes not saved.

Unlike the model editor it does not open a scene of its own: the cinematic plays **here**, at this site, with the
real shuttle, the real crawler, the real fog and lamps. The picture is shown top left, scaled down but framed
exactly as the game frames it, title card and bars included. **H** hides the editor to see it full size.

## The timeline

Along the bottom, a row for everything the cinematic has: the shots (which camera, from when; a wedge where one
blends into the next), each camera (its keys, and a band where it is the one in the picture), each actor (its keys,
the paths it drives as bars, its clips as flags), the sounds, the markers, the words, the particles, the lights,
and the shake, fade, bars, fog and light, each with its value drawn along it.

- **Click** a key or flag to select it; **drag** it along to move it. It snaps to the step chosen in the toolbar
  and to the playhead; **Shift** while dragging lets it go anywhere. **Alt**-drag leaves a copy behind.
- **Double-click** an empty place on a row to add a key or event there. A new key takes the value that is already
  there, so nothing moves until it is changed.
- **Right-click** for duplicate, copy, paste values, ease, delete, and deleting a whole track.
- Drag in the ruler to scrub. **Ctrl+wheel** zooms, the wheel over the ruler zooms too, **middle-drag** pans,
  **F** fits it all in.

## Looking round

**C** switches between the cinematic's own cameras and a free one; holding the **right button** over the picture
takes hold of the view and looks round from where the cinematic's camera was. WASD flies, Q and E go down and up,
Shift is faster, Alt slower. In the free camera every camera's path is drawn, the selected one white with its keys
and the edges of what it sees.

To place a camera: fly to where it should be, select its key, and press **Set from the view** -- the key is
measured from whatever that camera is anchored to, so it lands in the same place relative to the pad, the crawler
or the building at every site. **View from it** goes the other way. **K** adds a key on the selected row at the
playhead; with the free camera, a camera key is what the view sees.

## The inspector

Down the right: whatever is selected, every value of it, and for keys the ease into it -- a named one, or a
curve whose two handles are dragged by hand (dragging one turns a named ease into a curve starting from it). With
nothing selected it shows the cinematic itself: its length, its cameras and actors, and what this site supplies
to measure from, with anything the cinematic names that is not supplied here in orange.

## Keys

| Key | Does |
|---|---|
| Space | play and pause |
| Left / Right | a frame back or on (Shift: a second) |
| Home / End | the start / the end |
| , and . | the key before / after, on any row |
| K | key at the playhead |
| Delete | delete |
| Ctrl+D | duplicate at the playhead |
| Ctrl+C / Ctrl+V | copy / paste values (onto the selected one, or as a new one at the playhead) |
| Ctrl+Z / Ctrl+Y | undo / redo |
| Ctrl+S | save |
| C | free camera / the cinematic's |
| F | fit the timeline |
| H | hide the editor |

## Saving

**Save** writes Assets/Cinematics/<name>.json, the file the game plays, in the same layout it was read in, so a
change shows as the lines it changed. **New / save as** saves a copy under another name or starts an empty one.
**Revert** goes back to the file, and can itself be undone. Nothing a mission generates is ever written into
these files.

While editing, markers that would send everybody somewhere (`go_to_ship`) or hold the players do nothing; the
intercom's lines (`say`) are still heard.
