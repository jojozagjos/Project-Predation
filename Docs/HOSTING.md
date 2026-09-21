# Hosting

Two ways to play together, and only one of them needs a machine somewhere.

| | On your network | Over the internet |
|---|---|---|
| Who it is for | same house, same wifi, a LAN party | anybody, anywhere |
| What you have to run | nothing | a relay |
| What it costs | nothing | a machine and its transfer allowance |
| What a player does | picks your game out of a list | picks your game out of a list, or types a code |

## On your network: nothing to set up

A host announces itself four times a second on UDP 27016, which every machine on the network
hears. Anybody who opens the browser sees the game and clicks Join. There is no code, no address,
no router setting and no server of any kind.

The only thing that stops this working is something eating broadcast traffic: a few office and
campus networks do, and some routers have client isolation switched on. The host page has the
machine's address folded away under "If your game does not appear in their list" for exactly that
case — the other player types it into the join box and everything else is the same.

Windows Firewall asks once, the first time the game opens a port. Saying no to either the private
or the public box is by far the most common reason a game nobody can find looks like it started
correctly.

## Over the internet: the relay

Two machines behind different routers cannot reach each other. Both of them *can* reach a third
machine that is publicly addressable, and that is all a relay is: everybody connects outwards to
it, and it passes messages between them. Outbound always works, which is why this needs nothing
forwarded on anybody's router.

It has to live somewhere with a public address. That is the whole of the requirement — it does not
need to be fast, or have a disk, or run anything else.

```bash
PredationRelay.exe --port 27020 --budget-gb 200
```

Then every player sets `net.relay_host` to that machine's address. Only one of you runs it.

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

Use the relay when people are not in the same building. That is the only reason it exists.
