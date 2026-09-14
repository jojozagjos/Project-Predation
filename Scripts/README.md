# Scripts

Every helper the project has, split by the platform it runs on. Nothing here is required to build
the game — they are the shortest way to do the things you do over and over.

```
Scripts/Windows/    .cmd files, for Windows
Scripts/Linux/      .sh files, for Linux
```

The two sides take the same arguments and print the same things, so instructions written for one
read the same on the other.

## Windows

| Script | What it does |
|---|---|
| `Play.cmd` | Builds if anything changed, then starts the game. The one to double-click. |
| `build.cmd [preset]` | Configures if needed, then builds. Default preset `windows-debug`. |
| `test.cmd [preset]` | Runs the unit tests through CTest. |
| `run.cmd [preset] [args]` | Starts an already built game. |
| `smoke.cmd [preset]` | Headless 60-frame run. Requires a screenshot out the other side and fails on any logged error. |
| `package.cmd [preset]` | Builds the shipping preset, lays out a folder and zips it. Default preset `windows-shipping`. |
| `clean.cmd [all]` | Clears vcpkg working trees. `all` also deletes every preset's compiled output, keeping the shared dependency tree. |
| `linux-build.cmd [preset]` | Builds and tests the Linux version in a Docker container. Needs Docker Desktop running. |
| `vsenv.cmd` | Finds MSVC through `vswhere` and sets up the x64 environment. The others call it; you do not. |

## Linux

| Script | What it does |
|---|---|
| `play.sh` | Builds if anything changed, then starts the game. |
| `build.sh [preset]` | Configures if needed, then builds. Default preset `linux-release`. |
| `test.sh [preset]` | Runs the unit tests through CTest. |
| `run.sh [preset] [args]` | Starts an already built game. |
| `clean.sh [all]` | Clears vcpkg working trees. `all` also deletes every preset's compiled output, keeping the shared dependency tree. |

There is no `package.sh` yet. The Linux port is prepared but has not been compiled; see
[../Docs/LINUX.md](../Docs/LINUX.md) for what is known to be left.

## The window-holding preamble

Every `.cmd` here starts with the same eleven lines. They are worth explaining once rather than
eleven times.

When Explorer runs a double-clicked script it starts `cmd /c`, and `/c` closes the window the
instant the script finishes. Everything the script printed goes with it, so a run that worked and
a run that failed look exactly the same from the outside: a flash, then nothing. The preamble
notices that case and runs the script again under a shell that waits for a key.

It tells the two cases apart by looking at `%cmdcmdline%`, the command line of the shell the script
is running under. A double-clicked script is named on it; a script typed at a prompt that was
already open is not, because that shell was started for the person, not for this file. The match is
on the file name rather than the full path on purpose: Explorer does not always hand over the same
spelling of a path that the script sees for itself, and a missed pause is a window that vanishes.

It calls `find.exe` by its full path under `%SystemRoot%`. If you have Git or MSYS on your PATH,
the bare name `find` can resolve to the Unix `find` instead, which does not understand `/i` and
fails — and a failed test here means the window closes without pausing, which is exactly the
problem this code exists to prevent.

`PRED_NO_PAUSE=1` turns the behaviour off, which is what you want from a build server. Pausing when
it was not needed costs nothing — with no console to read a key from, `pause` returns at once.

## Line endings

`.gitattributes` checks the `.cmd` files out with CRLF line endings. This is not cosmetic: `cmd.exe`
seeks through a batch file by byte offset, and `goto` in a file with bare LF endings can land in the
wrong place. If you edit these with a tool that rewrites line endings, check them afterwards.

## Two builds

`windows-debug`, `windows-relwithdebinfo` and `windows-release` are developer builds: the model
editor is on the title screen and its commands are in the console. `windows-shipping` is what other
people get — same code, `PRED_DEV_TOOLS=OFF`, no way in to any of that.

They are separate folders under `build/` on purpose. The developer tools are a cached CMake
variable, so building a shipping exe inside a developer folder used to leave that folder without
its tools, silently, until somebody noticed a missing button.

To tell them apart at a glance: the developer build says "developer build" beside the byline on the
title screen, and both say which they are in the first line of the log.

## Where the dependencies live

All presets share one vcpkg tree at `build/vcpkg_installed`, set by `VCPKG_INSTALLED_DIR` in the
base presets. vcpkg's default is one tree per build folder, which on this project was 3.1 GB each
and identical every time. Sharing is safe because a triplet's tree already holds both the debug and
the release libraries.
