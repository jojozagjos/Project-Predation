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
everyone at low bandwidth. The anatomy seed goes with every update, so a client that joins late or
misses a packet still builds the same body (ADR-064).

Built in Milestone 8: the `Creatures` message carries, per creature, an id, the seed, position,
facing, speed, strike wind-up, health, whether it is alive, whether it is lying down (dead or playing
it, drawn the same) and how low it is crouching, at the world-state rate. Posture and
contact targets arrive with the procedural anatomy.

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

**Status**: built through Milestone 8. Two machines connect over UDP, the host simulates everyone, clients
predict their own movement and interpolate everyone else, and shots, voice, spectating, host migration and
the creature all cross the wire. Protocol version 7: the roster says whether the host has started the
game (a guest waits in the lobby until it has), and a death carries how many seconds until the player
is back, so their screen can count it down. Version 6 added the "quiet" bit on world events, set on the
catch-up a joining player is sent, so what already happened is applied without being heard.

Finding each other over the internet is the lobby server's job (`Engine/Net/LobbyProtocol.h`,
`Engine/Net/LobbyDirectory.h`, `Engine/Net/LobbyClient.h`; ADR-067): it hands out codes and introduces
two machines, which then punch through their routers to each other and play directly. Its messages
travel through the game's own socket, by the transport's side door (`Transport::SendUnframed` and
`TakeUnframed`), because a hole punched through a router only opens for the socket that punched it.

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
three ticks of input in under 32 bytes and goes out at 60 Hz. A creature costs 137 bits in the `Creatures`
message, also at 30 Hz: one creature is about half a kilobyte a second to each client, and the most there
can be, eight, is 140 bytes a packet.

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

Reconnecting a player who dropped, per-player weapon state on the host, and a way through for the few
routers hole punching cannot pass (a relay fallback on the lobby server would be it; see ADR-067).

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

## When the host leaves

The game does not end. The host publishes the roster whenever it changes: who is playing and the
address each was seen from. When the host goes, everyone waits a moment (`net.migration_seconds`),
the lowest surviving player number starts hosting on the port everyone was already using, and the
rest connect to it. Nobody has to agree on the choice, because everybody is working from the same
list.

The world survives it because every machine already holds a complete copy; the successor's copy
simply becomes the authoritative one.

On one network this works. Through a router the addresses are the holes that router punched, which
stay open only as long as they stay open, so migration over the internet is best effort until there
is NAT traversal.

## Playing on the same network

The host binds every interface, so it is listening on all of the machine's addresses at once. What
it cannot do is know which one to hand out, so the title screen lists them and a click copies one.
Loopback is left out because it is the address that always works and never helps.

Two things go wrong, and neither of them is the game:

**The wrong address.** Told to hand out "your IP", the obvious thing to look up is the public
address. That belongs to the router, not to the machine, and nothing on the same network can reach
the host through it. The right one starts `192.168.`, `10.` or `172.` and is in the list on the
title screen.

**The firewall.** Windows asks once, the first time the game listens, and the prompt is easy to
dismiss or to miss entirely. It has to be allowed on private networks. If it was refused, this adds
the rule back, run from an administrator command prompt with the path to the executable:

```bash
netsh advfirewall firewall add rule name="Project Predation" dir=in action=allow protocol=UDP localport=7777 program="C:\path\to\ProjectPredation.exe"
```

Playing across the internet is a different problem: see HOSTING.md for codes and the lobby server,
and below for the router being asked to let people straight in.

## Playing across the internet

A home connection has one public address and the router has no idea which machine behind it a
stranger's packet is for, so hosting reaches the local network and nothing else until somebody tells
the router otherwise. Setting that up by hand is port forwarding. The game asks for it instead: when
you open a game it looks for a router on the network, asks it to forward the port it is listening on,
and asks what the connection's address looks like from outside. That address appears in the menu
marked "from anywhere", and it is the one to give somebody who is not in the house.

It is best effort and says which. A router with UPnP switched off, a network with two routers between
the machine and the internet, and a connection where the provider does the translating are all real,
and none can be fixed from inside the game; the menu says so and the local addresses are still there.
Forwarding UDP 27015 to the host machine by hand does the same job.

The mapping is taken down when hosting stops. A forwarded port pointing at a machine that is no
longer listening is worse than no port, and routers keep mappings for a long time.

What is checked in `Tests/NetTests.cpp` is the text handling: reading an address apart, and finding
the one service in a router's description that forwards ports, which is never the first one in the
document. Discovery and the requests themselves need a router in the room.
