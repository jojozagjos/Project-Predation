#!/usr/bin/env bash
# The one thing to run. Builds the game if it needs building, then plays it.
set -euo pipefail

# Git Bash, MSYS2 and Cygwin will happily start this on Windows and then fail somewhere
# deep inside CMake, where the real problem is not visible. Say it here instead.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "[play] This is the Linux script. On Windows run Scripts/Windows/Play.cmd instead." >&2
        exit 1
        ;;
esac
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PRESET="${1:-linux-release}"
EXE="$ROOT/build/$PRESET/bin/ProjectPredation"

if [[ -x "$EXE" ]]; then
    echo "[play] Building anything that has changed..."
else
    echo "[play] First build. This one takes a while; every one after it is seconds."
fi

"$(dirname "${BASH_SOURCE[0]}")/build.sh" "$PRESET"

if [[ ! -x "$EXE" ]]; then
    echo "[play] The build said it worked but there is no game at $EXE"
    exit 1
fi

echo
echo "[play] Starting."
exec "$EXE"
