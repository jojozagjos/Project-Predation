# Hosting

One button hosts, and a game is reachable every way at once. Nobody has to decide in advance which of
these their situation needs.

| | How a friend joins | What has to be true |
|---|---|---|
| **By code** | types the six-character code the host's lobby shows | a lobby server is set up (see SERVER.md) |
| **Same network** | picks the game from "On your network" | the network lets PCs see each other |
| **By address** | types the host's address into the join box | the host's router lets them in, or Tailscale |

## The lobby

Hosting goes to the lobby first, not straight into the world:

1. The host presses **Play → Host a game → Start**.
2. The lobby shows a **code** (with a Copy button) and who is here.
3. Friends press **Play**, type the code into the box at the top, and press **Join**. They see the
   same lobby, with "Waiting for the host to start".
4. The host presses **Start the game**, and everybody goes in together.

People can still join after the game has started: they go straight in. The code is in the pause menu
for exactly that.

**Show it in Public games** (on the host page) also puts the game in everybody's *Public games* list,
so strangers can join without a code. Off by default.

## By code: how it works

The lobby server introduces the two PCs, and then they connect **directly**. The server never carries
the game, which is why it is free to run (SERVER.md has the numbers and the setup).

The trick is called hole punching. A home router only lets in replies to things that went out. So once
the server has told each PC where the other is, both send to each other at the same moment. Each
router sees its own PC's message going out first, takes the other's arriving message for the reply,
and lets it in. The game tries every address the other PC might have — its router's outside address
and its addresses inside its own network — and uses whichever answers first, so two PCs in the same
house find each other directly too.

It works through nearly every home router, with nothing forwarded and nothing configured. What it
cannot get through is a router that changes its outside port for every destination (some phone
hotspots and office networks do). The game says so in words — "could not connect to the host
directly" — and the fixes are to let the other person host, or use Tailscale. The host's router is
also asked (by UPnP) to let people straight in, which covers a friend whose own router is the strict
one.

## Same network

A host announces itself four times a second on UDP 27016, out of every network adapter the PC has.
Anybody on the **Play** screen also asks "is anybody hosting?" once a second, and hosts answer straight
back. The answer matters: Windows Firewall throws away announcements nobody asked for on networks it
treats as *public*, but lets a reply to its own question through. Before this, a game could be
invisible to the PC across the room while working perfectly between two copies on one PC.

What can still stop it: networks built to keep devices apart — schools, offices, hotels, some mesh
wifi with "client isolation" on. Then the list stays empty, and a code works instead (it goes out
through the internet and back, which those networks allow).

Windows Firewall asks once, the first time the game opens a port. Say yes to both boxes.

## By address

The join box takes an address as well as a code: `192.168.1.20`, or `81.2.3.4:27015`. The host's lobby
shows its own addresses under *Other ways to join*.

- **On the same network**, the host's address inside it (the lobby lists it).
- **Over the internet**, the host's router has to let people in. The game asks it to (UPnP) and, if it
  agrees, shows the outside address. Otherwise: forward UDP 27015 to the host PC by hand, or use
  **Tailscale** — both install it (free) and sign in, and the friend types the host's Tailscale address
  (it starts `100.`). Tailscale makes the two PCs behave as if they were on one network, with nothing
  opened on any router and no server.

Testing tip: you usually cannot join your own outside address from inside your own house, even when it
works for everybody else. Test with a friend, or with the automated tests.

## Choosing

Use codes. They work from anywhere, for anybody, and need nothing from the players. On the same wifi
the game shows up in the list as well, and joining from the list is the same thing without typing.
Addresses and Tailscale are for the networks that block everything else.
