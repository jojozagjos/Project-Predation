# What is left

Ordered by the phase plan, with what is actually built as of Milestone 7. This is deliberately a
list of gaps rather than a list of achievements: it is for deciding what to do next.

## Player controller and embodiment

Built: walking, running, sprinting, crouching, prone with a supine roll, jumping, leaning, stairs
and slopes, fall damage, a full procedural body with IK, weapon holding, items held in the hand,
and ragdolls on death.

Still missing:

- **Mantling and vaulting.** The test map has ledges from 0.3 to 1.8 m and nothing can climb them.
  This is the largest hole in the movement set and it changes level design, so it should come first.
- **Stamina.** Sprinting is currently free. In a game about being hunted, running out is the point.
- **Injury.** Health is a number that does nothing until it reaches zero. Limping, a shaking aim and
  a slower stance change are what make damage frightening rather than administrative.
- **Melee or shoving.** There is no answer to something being close except shooting it.
- **Audio.** There is no audio system at all. Footsteps, breathing and weapon sound are most of what
  makes a horror game work, and creature hearing later needs the same loudness values.
- **A flashlight.** Owed since the first plan. It needs a spot light in the forward shader, and it
  matters twice: atmosphere now, and creature vision later.

## Multiplayer

Built: UDP with its own reliability, host authority, client prediction with replay, interpolated
remote players, doors, lockers, pickups, ammunition crates, loose objects, shots, friendly fire,
damage and death.

Still missing:

- **Lag compensation.** The host tests hits against where players are now, not where the shooter saw
  them. At 100 ms that is a metre of error on a running target. The hit test is already in one
  place, which is what this needs.
- **Death and respawn as a flow.** Dying leaves a body and nothing else happens. There is no
  spectating a teammate, which the brief asks for, and no round structure to respawn into.
- **Nameplates.** You cannot tell who anybody is.
- **Voice.** Proximity voice over the same transport, which the creature's hearing later reads as a
  stimulus.
- **Weapon state per player.** The host passes on what a client says it is holding but does not
  simulate their ammunition, so a client is trusted about its own magazine.
- **Reconnecting.** A dropped player is gone for the round.
- **Matchmaking and NAT traversal.** Joining means typing an address and forwarding a port.

## Not started

Creatures, AI, procedural anatomy, capture and the lair, and the first real map. These are phases 7
onwards and should stay that way: the brief puts movement and embodiment first, and the reason is
that a creature is only frightening if the body it is chasing feels like a body.
