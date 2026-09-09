# Weapons

**Status**: built. Milestone 6 delivers the firearm: aiming, firing, recoil, spread, reload, limited
ammunition and hit resolution. Projectile weapons, weapon-specific animation clips and creature damage
follow later.

## Shape

Three pieces, deliberately separated:

- **`WeaponDatabase`** loads `Assets/Data/weapons.json`. Nothing in a definition changes at runtime.
- **`WeaponSim`** is the simulation. `Step` takes a definition, an input and a state, and returns any
  `FireEvent`s. It touches no renderer, no physics and no scene, and it never applies damage.
- **`ResolveShot`** traces one event against the world and reports what it found.

The line between the second and third is the multiplayer authority boundary, and it is the reason the
split exists. See ADR-014.

## Why firing produces events rather than damage

A client has to fire the instant the trigger goes down, or the weapon feels broken over any latency. A
host has to be the only thing that decides what was hit, or a client can claim any kill it likes.

Both run `WeaponSim::Step`. The client raises its events for muzzle flash, recoil and ammunition, and
sends them up. The host runs identical code, validates rate of fire and ammunition against its own
copy of the state, and calls `ResolveShot`. Nothing on the client's side of that line ever changes
another player's or a creature's health.

Today the game is single player, so the same process does both. `PredationGame::ResolveShots` is
marked as the authority's step and is the one place a shot becomes an effect on the world.

## Why spread is hashed, not random

Two machines simulate the same shot. If the deviation came from a random number generator, its state
would have to be replicated and kept in step, and any divergence would put the client's prediction
and the host's hit test on different lines through the world.

`DeviateShot` hashes the shooter's shot number instead. Same shot, same deviation, everywhere, with
no state to synchronise and nothing to send. `WeaponState::shotCount` also gives the host a sequence
to check: gaps or repeats mean a client is claiming rounds it did not fire.

## State

`WeaponState` is the replicated unit. Given a state and a sequence of `WeaponInput`s the result is
reproducible, which is what prediction and reconciliation need.

| Field | Meaning |
| --- | --- |
| `rounds`, `reserve` | magazine and spare ammunition |
| `fireCooldown` | seconds until the next round may leave the barrel |
| `reloadRemaining` | above zero while the magazine is out |
| `aim` | 0 at the hip, 1 sighted; drives spread, movement speed and the hold |
| `bloom` | extra spread from sustained fire, decaying |
| `recoilPitch`, `recoilYaw` | degrees added to the view, decaying back to zero |
| `shotCount` | every round this weapon has fired; seeds the spread |

## Recoil

Recoil is an offset on top of where the player is aiming, not a change to it. It decays back to zero,
so a burst walks upward and then hands their aim back intact. The sideways kick alternates on the
shot number, so a long burst wanders instead of climbing in a straight line.

Rounds leave along the same line the camera looks down, from the eye rather than from the model's
muzzle. A round that starts at the visible barrel can be on the far side of a doorframe the player is
peering round, so they shoot the wall they can see past.

## Holding it

`PlayerBody::UpdateWeaponHold` places the weapon in the view's frame, low and to the side until the
sights come up, then on the eye line. Both hands are solved onto it with the same two-bone IK the
legs use. The support hand slides back along the barrel when the weapon is longer than the arm can
reach: a shorter hold reads as a hold, an over-extended IK chain reads as a broken arm.

Crawling wins over the hold once the body is more than half way to lying flat. You cannot pull
yourself along the floor with a rifle in both hands, and the reach-and-pull cycle is what sells the
crawl.

## Data

`Assets/Data/weapons.json`. `key` is the stable identity for data files and network messages; `item`
links a weapon to the inventory entry that carries it, so picking a gun up and selecting its slot is
all it takes to be holding it. Angles are degrees, times are seconds, ranges are metres.

## Debug

- `give_weapon <key>` puts one in the inventory and selects it.
- `weapon_state` prints ammunition, aim, current spread and the shot count.
- `fire [ticks]` holds the trigger, for exercising the weapon without a mouse.
- Tracers are drawn for every round: yellow where it struck something, white where it did not.
- The reticle's gap is the real cone, converted through the same projection the world is drawn with,
  so it says where rounds can actually go rather than always promising the centre of the screen.

## Not yet

Projectile travel time and drop, penetration, weapon-specific reload and fire animations, shell
ejection, muzzle flash, audio, damage falloff over range, and hit zones. Creature damage arrives with
the creature.
