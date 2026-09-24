# Items

Everything that can be picked up is in `Assets/Data/items.json`. The two weapons are items too, and are
described in [WEAPONS.md](WEAPONS.md); this is about the rest.

Every item is laid out on both equipment benches -- the test map's and the creature lab's -- in the
order `items.json` gives them, as many of each as its `bench_count` says. An item added to the file is
on both benches without anything else changing.

## Using one

With an item in your hand rather than a weapon, **fire** uses it. A use takes as long as `items.json`
says, and the hand moves the whole time; everybody else sees the same movement on your body. What the
use actually does is decided by the host, like everything else that changes the world.

| Item | What using it does |
|---|---|
| **Medical Kit** | Patches up 45 health: on whoever is right in front of you and close enough to reach (2.2 m) if they are hurt, or else on you if you are. With nobody hurt, nothing happens -- no animation, and the kit is kept. Used up. |
| **Battery Cell** | A fresh cell in your torch. Used up. |
| **Access Keycard** | Opens the locked door you are looking at, if you are close enough -- and does nothing at all, not even the swipe, when you are not looking at one -- and unlocks it for good. Not used up. You do not have to hold it: pressing **use** on a locked door with a keycard anywhere in your bag does the same, and the door says **Unlock** instead of **Locked** when you have one. |
| **Signal Flare** | Strikes it. It burns red in your hand for a minute, held up in front of you. Fire again throws it; it lands, rolls, and burns on where it lies, lighting the room, until it is spent. Put away still burning, it is dropped at your feet and burns there. Used up when it leaves your hand. |
| **Sample Container** | Carried, for now. The thing the mission will be about. |

## The torch

The torch runs on a cell that lasts eight minutes on (`game.torch_minutes`). In its last fifth it
dims, and right at the end it stutters. Flat, it goes out and will not come back on until a battery
goes in. A bar above the stamina bar shows the cell while the torch is on or the cell is under half.
The cell is this player's own and is not sent anywhere; the torch being on or off is.

## Flares and the creatures

A burning flare lights anybody near it with a clear line to it, exactly as a lamp does, and somebody
carrying a lit one is as visible as somebody with a torch: a flare is a way to see and a way to be seen.
One landing is heard -- the same noise as a round striking something -- and anything that hears it may
come to look.

## Writing a use

In `items.json`:

```json
"use": {
  "kind": "heal",
  "seconds": 2.4,
  "amount": 45,
  "consumed": true,
  "start_sound": "Items/medkit_open",
  "done_sound": "Items/medkit_apply",
  "motion": [
    [0, 0, 0, 0, 0, 0, 0],
    [0.4, -0.18, 0.21, -0.07, 45, 0, -25],
    [1, 0, 0, 0, 0, 0, 0]
  ]
}
```

`kind` is one of `heal`, `recharge`, `unlock`, `flare` and `inspect`. `amount` is health for a heal,
the share of a cell for a recharge, and seconds of burning for a flare. Each key of `motion` is
`[t, right, up, forward, turn x, turn y, turn z]`: how far through the use (0 to 1), where the hand has
the item then relative to where it is carried -- metres right, up and forward of the view -- and a turn
in degrees. Between keys it is eased. A flare also has `second_seconds`, `second_motion` and
`second_sound`, for the throw.

The easiest way to shape a motion is by eye, in the editor: **Hold it** holds any item, and its **Use**
section plays the use, scrubs through it, edits each key, adds a key wherever the scrubber stands, and
writes it back with **Write to items.json**.

## On the wire

A client tells the host when it starts using something, stops, finishes, or throws a flare
(`ItemUse`). The host checks the client actually carries one (its tally of what each client picked
up), does what the use does, takes one off the tally if it was used up, and tells everybody
(`ItemUsed`, `FlareThrown`, `DoorUnlocked`) so their copies of that player's hand move and the sounds
are heard. Health goes back through the same player state everything else does. A newcomer is told
which doors have been unlocked, which flares are burning where and for how long, and who is holding a
lit one.
