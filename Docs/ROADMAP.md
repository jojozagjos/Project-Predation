# What is left

Ordered by the phase plan, with what is actually built as of Milestone 7. This is deliberately a
list of gaps rather than a list of achievements: it is for deciding what to do next.

## Player controller and embodiment

Built: walking, running, sprinting with stamina, crouching, prone, mantling, injury, jumping, leaning, stairs
and slopes, fall damage, a full procedural body with IK, weapon holding, items held in the hand,
and ragdolls on death.

Still missing:

- **Melee or shoving.** There is no answer to something being close except shooting it.
- **Audio.** There is no audio system at all. Footsteps, breathing and weapon sound are most of what
  makes a horror game work, and creature hearing later needs the same loudness values.
- **A flashlight.** Owed since the first plan. It needs a spot light in the forward shader, and it
  matters twice: atmosphere now, and creature vision later.

## Multiplayer

Built: UDP with its own reliability, host authority, client prediction with replay, interpolated
remote players, lag compensation, host migration, doors, lockers, pickups, ammunition crates, loose objects, shots,
friendly fire, damage and death.

Still missing:

- **A round structure.** Death and respawn work and you spectate a teammate while dead, but there
  is nothing to respawn into: no extraction, no objective, no end.
- **Nameplates.** You cannot tell who anybody is.
- **Voice.** Proximity voice over the same transport, which the creature's hearing later reads as a
  stimulus.
- **Weapon state per player.** The host passes on what a client says it is holding but does not
  simulate their ammunition, so a client is trusted about its own magazine.
- **Reconnecting.** A player who drops is gone for the round. Host migration covers the host
  leaving, but not anybody else coming back.
- **Matchmaking and NAT traversal.** Joining means typing an address and forwarding a port.

## Not started

Creatures, AI, procedural anatomy, capture and the lair, and the first real map. These are phases 7
onwards and should stay that way: the brief puts movement and embodiment first, and the reason is
that a creature is only frightening if the body it is chasing feels like a body.
