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

---

# Implementation notes

**Status**: Milestone 6 built. Two machines connect over UDP, the host simulates everyone, clients predict
their own movement and interpolate everyone else. Creature replication, voice and spectating are still design.

## What exists

| Piece | Where |
| --- | --- |
| Bit-level packet reading and writing | `Engine/Net/BitStream.h` |
| Transport interface, five operations | `Engine/Net/Transport.h` |
| In-process transport for tests | `Engine/Net/LoopbackTransport.cpp` |
| Real transport with its own reliability | `Engine/Net/UdpTransport.cpp` |
| The wire format | `Game/Net/Protocol.h` |
| Prediction history and reconciliation | `Game/Net/Prediction.h` |
| Host and client | `Game/Net/NetSession.h` |

## Console commands

    net_host [port]                      open a game, default port 27015
    net_join [address] [port]            join one, default 127.0.0.1:27015
    net_leave                            back to single player
    net_sim <latency ms> <jitter ms> <loss %>   pretend the connection is worse than it is
    net_status                           print the session state

The Network panel in the debug overlay (F3) shows packet and byte counters, how many corrections prediction
has needed, how far off the last one was, and sliders for the simulated conditions.

## Bandwidth

A four-player snapshot is 85 bytes: 152 bits per player plus a 71-bit header. At the 30 Hz send rate that is
2.6 kB/s to each client, so a host with three of them spends under 8 kB/s upstream. An input packet carries
three ticks of input in under 32 bytes and goes out at 60 Hz.

Positions are quantised to about a millimetre over a kilometre, angles to a twentieth of a degree, velocity to
three centimetres a second. A value outside a field's range has no encoding at all, so a modified client
cannot claim to be ten kilometres away.

## Input redundancy rather than resends

Every input packet repeats the last three ticks. An input is only useful for a few milliseconds, so a resend
would arrive after the host had already run past that tick. Repeating instead means a quarter of packets can
go missing with almost no effect, which the tests exercise directly.

## Simulated conditions

Latency, jitter, loss and duplication are simulated by both transports, including the real one. Prediction is
only exercised by a bad link, so the tests run at a round trip a real player would have rather than on a
perfect connection where nothing has time to disagree.

## Not yet

Creature replication, weapon events over the wire, voice, spectating, late join, host migration, and NAT
traversal. Weapons already produce host-authoritative `FireEvent`s (ADR-014); they are not yet sent.

## Getting into a game

The game opens at a title screen with the world already built behind it, so starting is instant and
there is only ever one world for hosting, joining and leaving to share.

- **Play on your own** goes straight in with no session.
- **Open a game** binds the port and puts you in the world. Other players join on your address.
- **Join** takes an address and a port, shows a joining state while the handshake runs, and enters
  the world when the host answers. Being turned away for a full game or a version mismatch is shown
  there rather than dropping you into an empty world.

The address and port are remembered in the archived config (`net.last_address`, `net.last_port`), so
rejoining the same game does not mean typing it again. Escape frees the cursor, and Escape again
with the cursor already free returns to the title screen.

The console commands still do the same things, and hosting from the console also enters the world.
