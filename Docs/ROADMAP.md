# What is left

Ordered by the phase plan, with what is actually built as of Milestone 8. This is deliberately a
list of gaps rather than a list of achievements: it is for deciding what to do next.

## Player controller and embodiment

Built: walking, running, sprinting with stamina, crouching, prone, mantling, injury, jumping, leaning, stairs
and slopes, fall damage, a full procedural body with IK, weapon holding, items held in the hand,
ragdolls on death, a torch, and sound: recorded footsteps per surface, synthesised weapon and world
sounds, and positional mixing.

Still missing:

- **Melee or shoving.** There is no answer to something being close except shooting it, which now
  matters: the creature is close more often than anything else in the game.

## Multiplayer

Built: UDP with its own reliability, host authority, client prediction with replay, interpolated
remote players, lag compensation, host migration, doors, lockers, pickups, ammunition crates, loose
objects, shots, friendly fire, damage, death, spectating, proximity voice, a LAN game browser, an
optional relay, UPnP port opening, and the creature (see below).

Still missing:

- **A round structure.** Death and respawn work and you spectate a teammate while dead, but there
  is nothing to respawn into: no extraction, no objective, no end.
- **Nameplates.** You cannot tell who anybody is.
- **Weapon state per player.** The host passes on what a client says it is holding but does not
  simulate their ammunition, so a client is trusted about its own magazine. Related: when a client
  fires, the host resolves the round with the host's own equipped weapon's range and damage.
- **Reconnecting.** A player who drops is gone for the round. Host migration covers the host
  leaving, but not anybody else coming back.

## The creature (Milestones 8 and 9 built)

Built: a navigation mesh from the level (Recast/Detour); a creature made from a seed; sight with a
field of view, occlusion, light and a glimpse-is-not-a-sighting exposure; hearing of footsteps,
landings, gunshots, impacts, doors, lockers, pickups, drops and voice; memory of each player;
utility scoring over Roam, Investigate, Hunt, Attack and Retreat; strikes that hurt and kill;
rounds that hurt it; the brain inspector and overlays; and replication, so everybody in a game sees
the host's creature. See [AI.md](AI.md).

Milestone 9: stalking from cover with peeking, playing dead, a real death that stops the brain,
arrival out of sight, searching, checking lockers and learning that shut ones hide people,
curiosity, a memory of where players go, several creatures at once keeping apart, and a test that
the same seed makes the same decisions.

Still missing, roughly in order:

- **Its body (Milestone 10, next).** A placeholder of boxes. The procedural anatomy generated from the seed,
  and replication already carries the seed for it.
- **Procedural animation and traversal (Milestone 11).** A generated gait for whatever body the seed
  makes -- legs placed by IK on the real ground, not swung on a timer -- climbing, ceilings, and vents
  it can hide and move in if its body fits (see AI.md, "Vents and other hiding places").
- **Its sound.** It makes none: no footfalls, no breathing, no call. For a creature that is found
  by listening, this is the largest gap it has.
- **Doors.** It walks through closed ones: the navigation mesh is built from the fixed level, and a
  door is not part of it.
- **Capture and the lair.**
- **Hunting as a pack.** Several creatures already run at once and keep apart; sharing what one has
  seen with the others -- a call, a converging hunt -- is not built. Each already knows where the others
  are, which is where it would start.

## Not started

Procedural anatomy, capture and the lair, and the first real map. These are the phases after this
one and should stay in that order: a creature's behaviour is worth tuning once it has the body it
will actually have.
