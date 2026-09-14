#!/usr/bin/env bash
# Usage: Scripts/Linux/run.sh [preset] [game args...]
# Plays whichever build is actually there, preferring release.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

PRESET=""
if [[ $# -gt 0 && -d "$ROOT/build/$1" ]]; then
    PRESET="$1"
    shift
fi
if [[ -z "$PRESET" ]]; then
    for candidate in linux-release linux-debug; do
        if [[ -x "$ROOT/build/$candidate/bin/ProjectPredation" ]]; then
            PRESET="$candidate"
            break
        fi
    done
fi
PRESET="${PRESET:-linux-release}"

EXE="${PRED_BUILD_DIR:-$ROOT/build/$PRESET}/bin/ProjectPredation"
if [[ ! -x "$EXE" ]]; then
    echo "[run] Nothing built yet: $EXE"
    echo "[run] Build it with Scripts/Linux/build.sh $PRESET, or run Scripts/Linux/play.sh which builds and then runs."
    exit 1
fi
echo "[run] Playing the $PRESET build."
exec "$EXE" "$@"
