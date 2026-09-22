# The lobby server

The lobby server is what makes codes work. A host asks it for a code; a friend types the code in; the
server tells each of them where the other is; and then the two games connect **straight to each
other**. The game itself never goes through the server.

It runs free on **Cloudflare Workers**, set up from your GitHub in a couple of minutes, with no card,
no machine to look after, and nothing that can be switched off for using too much bandwidth.

## Why this and not a machine somewhere

| | Cloudflare Worker (this) | A virtual machine (Oracle, Google...) |
|---|---|---|
| Setup | sign in, press Deploy | make a machine, open ports, install, keep it updated |
| Card needed | no | yes |
| Bandwidth bill | none: Cloudflare does not charge for it | free allowance, then billed |
| Can be switched off | no; a daily request limit resets at midnight UTC | idle machines get reclaimed |
| Looked after by | Cloudflare | you |

The free plan allows 100,000 requests a day. A host in its lobby checks in every 2.5 seconds and every
5 seconds once playing, so a two-hour evening is about 1,500 requests: dozens of evenings a day before
the limit is anywhere near. If it were ever reached, codes would stop working until midnight UTC and
games already running would carry on, because they do not use the server.

## How it works

```
   Host's PC ──── "give me a code" ─────►  Lobby server  ◄──── "join ABC123" ──── Friend's PC
       │                                  (introduces)                              │
       │   ◄── "what do I look like from outside?" ──►  public STUN servers  ◄──►   │
       └──────────────── the game itself, directly, PC to PC ────────────────────────┘
```

1. Each game asks a free public **STUN** server (Cloudflare's and Google's) what its connection looks
   like from outside: its router's address and the port the router gave it.
2. The host tells the lobby server that, and gets a code.
3. A friend types the code. The server gives the friend the host's addresses, and gives the host the
   friend's on its next check-in.
4. Both games send to each other at once, which opens both routers (hole punching), and connect.

The server's code is in `Tools/LobbyWorker`. It keeps everything in memory and needs no database. If
Cloudflare ever restarts it, hosts find out on their next check-in and get their codes back.

## Setting it up

About five minutes, once.

1. **Make a Cloudflare account** at <https://dash.cloudflare.com/sign-up>. It is free and asks for no
   card. (Signing in with Google or GitHub works if you would rather not make a password.)

2. **Deploy it**, one of two ways:

   - **The button** (quickest). Open <https://github.com/jojozagjos/Project-Predation/tree/main/Tools/LobbyWorker>
     and press **Deploy to Cloudflare**. It asks to connect your GitHub, makes a small copy of the
     server in a new repository of yours, and deploys it. Changes made to the game's copy later do not
     reach that one; press the button again to pick them up.
   - **Straight from this repository** (updates itself). In the Cloudflare dashboard: **Workers &
     Pages → Create → Import a repository**, connect GitHub, pick **Project-Predation**, and press
     Deploy with the settings as they are: `wrangler.jsonc` at the top of the repository tells
     Cloudflare where the server is. From then on, every push redeploys it by itself.

3. **Copy its address.** When it finishes, Cloudflare shows the Worker's address, like
   `https://predation-lobby.yourname.workers.dev`. Opening it in a browser should show
   `{"ok":true,"lobbies":0}`.

4. **Give it to the game.** In the game: **Settings → Multiplayer → Lobby server**, paste the address.
   Everybody needs the same one, so the easiest thing is to have it built into the game as the
   default: it is `net.lobby_server` near the top of `Game/PredationGame.cpp`. Send it over and it goes
   in.

That is all. There is no port to open anywhere and nothing to keep running.

## Watching it

In the Cloudflare dashboard, open the Worker, then **Logs**: each lobby opening, each introduction and
each close is a line. **Metrics** shows requests per day against the free limit.

## When a friend cannot connect

The game says what went wrong in words. The usual ones:

- **"There is no game with that code."** Typo, or the host has closed the game.
- **"The lobby server is not answering."** The address in Settings is wrong, or there is no internet.
  Opening the address in a browser tells you which.
- **"Your router is strict"** (shown to a host) or **"could not connect to the host directly"** (shown
  to a friend). One of the routers makes a new outside port for every destination, which hole punching
  cannot get through. Some phone hotspots and office networks do this. Let the other person host, or
  both install Tailscale and join by address (see HOSTING.md).

## Testing on one PC

`node Tools/LobbyWorker/local-server.js` runs the same server locally (plain Node, nothing to install),
with a STUN responder standing in for the public ones. Then in each copy of the game's console:

```
lobby_use http://127.0.0.1:8787 127.0.0.1:3478
```

`lobby_use` is for that run only and is not saved. The server's own tests are
`node --test "Tools/LobbyWorker/test/*.test.js"`, and GitHub runs them on every change to the server.
The game's side is tested in `Tests/LobbyTests.cpp` with a stand-in server and real sockets.

## Later: a server of our own

If the game ever needs a dedicated server, this stays useful: introductions are a separate job from
running a game. The lobby logic is one small file (`src/lobbies.js`) that runs the same under Node, so
moving it onto any machine is a matter of wrapping it in `local-server.js`.
