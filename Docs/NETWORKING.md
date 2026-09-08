# Networking

**Status**: design. Implementation begins in Milestone 7 (multiplayer prototype). The player controller from
Milestone 3 onward is written to fit this model: fixed tick, input structs, no rendering dependencies.

## Topology

Host-authoritative listen server for 1 to 4 players. The host runs the full simulation and owns creature AI,
creature state, damage, important physics interactions, and mission state. Clients predict only their own
movement and interpolate everything else. Later, a small internet service handles matchmaking, lobby
discovery, and NAT traversal signaling; gameplay traffic stays peer to host.

## Timing

- Simulation: fixed 60 Hz tick (`app.fixed_hz`).
- Send rate: 20 to 30 Hz.
- Rendering: interpolates between the two most recent snapshots for remote entities.

## Transport

Interface with five operations: connect, disconnect, send reliable, send unreliable, poll. First implementation
over ENet (reliable and unreliable channels over UDP). Second implementation over GameNetworkingSockets or
Steam networking when encryption and NAT traversal are needed. The transport layer includes latency, jitter,
and packet loss simulation from day one so prediction is tested under bad conditions.

## Replication

Replicated components produce delta snapshots against the last acknowledged state per client. Reliable events
carry discrete actions: fire, interact, chat, damage, mission changes. With four players in one facility, no
interest management is needed initially; every relevant entity replicates to every client.

Packets use an in-house bit writer. Packet structures are covered by unit tests.

## Player movement

Clients send sequenced input structs (buttons, look angles, tick). The host runs the same movement code and
returns authoritative state with the last processed input sequence. The client compares, and on mismatch
rewinds to the host state and replays unacknowledged inputs. Correction smoothing hides small errors.

## Weapons

The client fires immediately for feel (muzzle flash, sound, recoil) and sends a fire event with tick and aim
direction. The host validates ammo, fire rate, and plausible direction, rewinds targets by a bounded
interpolation delay (about 200 ms max) for hit tests, and applies damage. Clients never report damage.

## Creature

AI runs on the host only. Clients receive position, orientation, gait parameters, posture, and contact targets
at the send rate, then run the same procedural animation locally. A creature on the ceiling looks identical to
everyone at low bandwidth. The anatomy seed is sent once at spawn so clients build the same body.

## Capture and grabs

The host attaches the captured player to the creature. Client prediction is suspended while captured or
restrained; the client follows host state directly.

## Voice

Opus frames on an unreliable channel through the host to players within proximity range. The host also emits a
voice sound stimulus for creature hearing with loudness derived from microphone activity. No audio data reaches
the AI. Optional player-voice mimicry is opt-in per player, kept in memory for the session only, never written
to disk.

## Spectating

Dead clients receive the spectated teammate's already-replicated camera state and a HUD summary. Free camera
is not offered because it would reveal creature positions.

## Security posture

Host validation of ammo, fire rate, interaction distance, and movement plausibility. No anti-cheat
infrastructure initially, but no client is trusted for critical events.

## Not initially

Late join, host migration, interest management, dedicated servers.

## Debug

Network overlay (RTT, loss, bandwidth per channel), replication log, and the simulated-conditions controls.
