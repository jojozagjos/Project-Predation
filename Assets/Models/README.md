# Models

One folder per kind of thing, and a model is found by its file name wherever it sits. Nothing
outside this folder spells out a path: `weapons.json` says `"model": "m5_carbine"`, and moving that
file into a different folder tomorrow changes nothing anywhere else.

```
Weapons/        the guns
Vehicles/       the shuttle, the crawler, the carrier and the hangar doors
Props/          anything else that is not held
Source/         the art these were imported from, in the same shape as above
Textures/       images the models name, shared across all of them
```

A model says what it is with `"kind"` (`weapon`, `vehicle` or `prop`), which is what the editor offers it: grips,
a magazine, reloads and someone to hold it for a weapon; none of that for the rest. One that does not say goes by its
folder. Vehicles face **-Z** with the origin on the ground under their middle.

Adding a new one is dropping a file in. Two models cannot share a name, even in different folders;
the game says which two and which it picked rather than quietly using whichever it found first.

`Source/` is never read by the game. It is the `.glb` or `.obj` a model was imported from, kept so
the import can be repeated when the importer improves or when somebody wants to re-cut the original.

## Importing

Drag a `.glb` or `.obj` onto the game window with the model editor open, or from the console:

```
model_import <file> <name>
```

It lands in `Weapons/` (or whichever folder the editor is showing), and the source file is copied
into `Source/` beside it so the import can be repeated later.

The game expects a weapon's barrel to run down **+Z** and its up to be **+Y**. Most exporters do
not agree; the editor's `Orientation` controls turn a model onto that frame once and bake it in,
rather than every piece of code that aims a gun having to know which way this particular one faces.

## Sockets

A socket is the contract between a model and the hands that hold it. A weapon needs four:

| socket    | what it means                                                |
|-----------|--------------------------------------------------------------|
| `grip`    | where the trigger hand goes, and which way the weapon is held |
| `support` | where the other hand goes                                     |
| `muzzle`  | where shots leave                                             |
| `sight`   | what lines up with the eye when the sights come up            |

Two more are optional because each has a real answer when it is absent: `carry` (where the weapon
rides when it is not up, defaults to the grip) and `magazine` (where a magazine seats, absent when
there is no magazine part).

A model missing any of the four loads with an error naming the model and the socket. It does not
fall back to a guess: a guess puts the gun somewhere plausible and costs an afternoon.
