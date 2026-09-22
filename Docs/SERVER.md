# The lobby server

The lobby server is what makes codes work. A host asks it for a code; a friend types the code in; the
server tells each of them where the other is; and then the two games connect **straight to each
other**. The game itself never goes through the server.

That last part is why it is cheap to run. The old relay passed every byte of every game through
itself, which is exactly the kind of thing that runs out of bandwidth and gets a server switched off
for the month. The lobby server only makes introductions:

| | Traffic |
|---|---|
| An open lobby | about 60 bytes every 2 seconds |
| Somebody joining | a handful of messages, once |
| A full evening of four-player games | well under a megabyte |

Oracle Cloud's free tier includes 10 TB of outgoing traffic a month. You will never get near it.

## What is running where

```
   Host's PC  ──── "give me a code" ────►  Lobby server  ◄──── "join ABC123" ────  Friend's PC
       │                                   (introduces)                               │
       └──────────────── the game itself, directly, PC to PC ─────────────────────────┘
```

The server is one small program (`Tools/LobbyServer/main.cpp` plus four files in `Engine/Net`). It
keeps everything in memory, has no database, and needs no maintenance. If it restarts, hosts ask
again by themselves and get their old codes back.

## Setting it up on Oracle Cloud, free

About twenty minutes, once. You need an email address and a card. Oracle checks the card is real but
does not charge anything for the free machines.

### 1. Make the account

1. Go to <https://www.oracle.com/cloud/free/> and press **Start for free**.
2. Pick a **home region** near where you and your friends live. It cannot be changed later.
3. Finish signing up. It can take a few minutes for the account to be ready.

### 2. Make the machine

1. In the Oracle Cloud console, open the menu (top left) → **Compute** → **Instances** →
   **Create instance**.
2. Name it anything, for example `predation-lobby`.
3. **Image**: press *Change image* and pick **Ubuntu** (the newest version listed).
4. **Shape**: press *Change shape*. Either of these is free and is far more than the server needs:
   - **Ampere** → `VM.Standard.A1.Flex` with 1 OCPU and 6 GB of memory, or
   - **AMD** → `VM.Standard.E2.1.Micro`.

   Both are marked *Always Free-eligible*. If Oracle says it is **out of capacity** for one, try the
   other, or try again later.
5. **Networking**: leave it creating a new network, and make sure **Assign a public IPv4 address** is
   on.
6. **SSH keys**: choose **Generate a key pair for me** and press **Save private key**. Keep that file
   somewhere safe: it is how you get into the machine.
7. Press **Create**. After a minute it says *Running*. Note the **Public IP address** on its page.

### 3. Open the door in Oracle's network

Oracle blocks everything except SSH until you say otherwise.

1. On the instance's page, click the **subnet** link (under *Primary VNIC*).
2. Open its **Security Lists**, then the **Default Security List**.
3. Press **Add Ingress Rules** and fill in:
   - **Source CIDR**: `0.0.0.0/0`
   - **IP Protocol**: `UDP`
   - **Destination Port Range**: `27020`
4. Press **Add Ingress Rules**.

### 4. Log in and install the server

On your PC, open **PowerShell** and type (with your key file's real path and the machine's IP):

```powershell
ssh -i C:\Users\you\Downloads\ssh-key.key ubuntu@123.45.67.89
```

If it says the key's permissions are too open, run this once and try again:

```powershell
icacls C:\Users\you\Downloads\ssh-key.key /inheritance:r /grant:r "$($env:USERNAME):R"
```

Once you are in (the prompt changes to `ubuntu@...`), paste this and press Enter:

```bash
curl -fsSL https://raw.githubusercontent.com/jojozagjos/Project-Predation/main/Tools/LobbyServer/setup-linux.sh | bash
```

It installs a compiler, downloads just the server's source, builds it, sets it up to start whenever
the machine starts, and opens port 27020 in the machine's own firewall. At the end it prints the
server's address.

### 5. Stop Oracle switching it off

Oracle stops free machines that look idle for a week, and a lobby server is idle nearly all the time.
To prevent that, upgrade the account to **Pay As You Go** (console → your profile → *Upgrade and
Manage Payment*). Free resources stay free: you are only charged for going past the Always Free
limits, and this server comes nowhere near them. If you would rather not, the machine is *stopped*,
not deleted, and pressing **Start** on its page brings it back.

### 6. Point the game at it

In the game: **Settings → Multiplayer → Lobby server**, and type the machine's public IP. Everybody
who plays needs the same address there. Once it is working, the address can be built into the game as
the default so nobody has to type it: it is the `net.lobby_server` setting near the top of
`Game/PredationGame.cpp`.

## Looking after it

| To | Run on the server |
|---|---|
| Watch what it is doing | `sudo journalctl -u predation-lobby -f` |
| See whether it is running | `sudo systemctl status predation-lobby` |
| Restart it | `sudo systemctl restart predation-lobby` |
| Update it to the newest code | run the same `curl ... | bash` line again |

The log shows each lobby opening and closing and each introduction, with the addresses involved, and
an hourly summary.

## When a friend cannot connect

The game says what went wrong in words. The usual ones:

- **"There is no game with that code."** Typo, or the host has closed the game.
- **"The lobby server is not answering."** The server is down, the address in Settings is wrong, or
  port 27020 is not open (step 3 above). `sudo systemctl status predation-lobby` on the server tells
  you which.
- **"Found the game, but could not connect to the host directly."** Both routers were introduced, but
  one of them will not let the connection through. This happens with some phone hotspots and office
  networks. Let the other person host, or both install Tailscale and join by address (see
  HOSTING.md).

## Testing without a server

With developer tools on, the console command `lobby_server_local` runs a lobby server inside that copy
of the game, and `lobby_use 127.0.0.1` points another copy at it. Neither is saved, so testing leaves
the settings as they were. The automated tests (`PredationTests "[lobby]"`) do the same with a real
server and two real games on this PC.

## Running it somewhere else

Any Linux machine with a public address works; `setup-linux.sh` is written for Ubuntu and Debian. The
server can also be built on Windows (`cmake --preset windows-release -DPRED_BUILD_LOBBY_SERVER=ON`) and
run on a PC with UDP 27020 forwarded to it, but a machine that is always on is the point of it.
