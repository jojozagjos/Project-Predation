# Hosting

Three ways to play together. Only the last one needs a machine of your own somewhere.

| | Same wifi | Over the internet | Over the internet, with a relay |
|---|---|---|---|
| What the host does | Host → On your network | Host → Over the internet | same button |
| What a friend does | picks the game from the list | pastes the address the host sends | picks the game from the list |
| What has to be true | nothing | the host's router lets people in (or Tailscale) | somebody runs a relay |
| Cost | nothing | nothing | a small server |

## Same wifi: nothing to set up

A host announces itself four times a second on UDP 27016, which every machine on the network
hears. Anybody who opens Play sees the game and clicks Join.

The only thing that stops this is something eating broadcast traffic: a few office and campus
networks do, and some routers have client isolation switched on. Then the host sends their address
from the host page instead and the friend pastes it into the join box.

Windows Firewall asks once, the first time the game opens a port. Saying no to either the private
or the public box is by far the most common reason a game nobody can find looks like it started
correctly.

## Over the internet: the host's router lets people in

Your router has one address on the internet and hands out private ones inside the house. A friend
out on the internet can only reach the router, and the router has to be told which PC in the house
the game is on. That is the whole problem, and only the **host** has it — a friend joining needs
nothing opened.

When somebody hosts "Over the internet" with no relay set up, the game asks the router to do that
itself (UPnP), then shows the host an address with a Copy button. The friend pastes it into the box
under the list on the Play screen. The address is in the pause menu too.

When the router says no, the screen says so and lists the ways round it:

- **Let a friend host instead.** Only the host needs an open door, so whoever has an ordinary home
  router with UPnP on should host.
- **Tailscale.** Both install it (free) and sign in, and share the host's machine with the friend.
  It makes the two PCs behave as if they were on one network, so the host picks "On your network"
  and the friend pastes the host's Tailscale address (it starts `100.`) with `:27015` on the end.
  Nothing is opened on any router, and the traffic goes PC to PC, so there is no server and no
  bandwidth bill.
- **Turn on UPnP** in the router's settings page, then host again.
- **Forward the port by hand:** UDP 27015 to the host PC's own address (the screen shows it). Then
  send friends your public address — search "what is my IP" — with `:27015` on the end.

Two things cannot be fixed from a router you are sitting behind: a network somebody else runs (a
flat, a dorm, an office), and an internet provider that shares one address between many homes
(CGNAT; the screen detects this and says so). Both are what Tailscale is for.

Testing tip: you usually cannot join your own public address from inside your own house, even when
it works for everybody else. Test with a friend, not with a second copy on your own PC.

## Over the internet, with a relay

Two machines behind different routers cannot reach each other. Both of them *can* reach a third
machine that is publicly addressable, and that is all a relay is: everybody connects outwards to
it, and it passes messages between them. Outbound always works, which is why this needs nothing
forwarded on anybody's router.

It has to live somewhere with a public address. That is the whole of the requirement — it does not
need to be fast, or have a disk, or run anything else.

```bash
PredationRelay.exe --port 27020 --budget-gb 200
```

Then every player puts that machine's address in Settings → Multiplayer → Relay server. Only one of you runs it. With a relay answering, "Over the internet" hosting goes through it by itself, which works through every kind of router and puts games in everybody's list.

The relay also answers the "what games are open" question, so the Over-the-internet tab of the
browser is filled in by the same machine. There is no second service, no database and no website.

### What it costs to run

Measured rather than estimated, by `Tests/RelayTests.cpp` — "What a relayed game actually costs to
carry" — so that the number here stays true when the protocol changes:

| | both directions | outbound only (what most providers bill) |
|---|---|---|
| A four-player game, no voice | **0.16 GB/hour** | ~0.08 GB/hour |
| The same with everyone talking | ~0.35 GB/hour | ~0.18 GB/hour |

So a hundred hours of four-player games is somewhere between 8 and 18 GB of outbound transfer.
Idle, it costs nothing at all: a relay with no lobbies open sends nothing.

That is small. It is worth knowing exactly how small, because the failure people actually hit is
not "the relay is too slow" — it is "the month's transfer allowance ran out, the provider
suspended the machine, and nothing works until the month rolls over".

### Not running out

The relay counts every byte it carries and can be told what its allowance is:

```bash
PredationRelay.exe --budget-gb 200
```

Past 95% of that it stops opening new lobbies and says so in its log. Games already running are
never cut off — ending somebody's evening to save a few pennies of transfer is the wrong trade, and
letting four players finish overshoots by a fraction of a gigabyte. Restart it when the month
rolls over.

Set it a little under whatever the provider actually allows, and it cannot be the thing that gets
the machine suspended.

### Where to put it

What it needs: a public address, an open UDP port, and a transfer allowance you are not going to
walk into. What it does not need: a fast machine, a disk, a domain name, TLS, or a database.

- **A free-tier virtual machine.** Oracle Cloud's always-free tier gives an ARM VM with a very
  large monthly egress allowance; Google Cloud and AWS have small free instances with smaller
  allowances. Any of them is far more than the numbers above need. Check the allowance, pass it to
  `--budget-gb`, and it cannot surprise you.
- **A cheap VPS.** Hetzner, OVH and similar are a few pounds a month with allowances measured in
  terabytes. At 0.08 GB/hour of egress, a terabyte is well over ten thousand hours of play.
- **A spare machine at home**, with UDP 27020 forwarded to it. No allowance to worry about beyond
  whatever your own connection has, and it is the cheapest option by a distance.

**Not a platform-as-a-service host.** Render, Heroku, Railway, Fly's HTTP tier and the rest are
built to run web services: they route HTTP, most of them do not pass UDP at all, and the free tiers
are capped low enough that a busy weekend exhausts one. Running out there is not a slow-down, it is
a suspension until the month ends. A plain virtual machine with a public IP is both cheaper and the
only shape that works.

## Choosing between them

If everybody is in the same building, use the network option. It is instant, it needs nothing, it
has lower latency than any relay can, and there is no machine anywhere that can be suspended.

Over the internet, try the host's router first, then Tailscale. A relay is for when a group wants games to just appear in a list without anybody sending an address, or when nobody in the group can open a door.
