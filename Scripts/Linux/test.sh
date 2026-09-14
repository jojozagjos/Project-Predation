#!/usr/bin/env bash
# Usage: Scripts/Linux/test.sh [preset]      default preset: linux-release
# Runs the unit tests. They need no window and no sound card, so they run anywhere the code builds.
set -euo pipefail

# Git Bash, MSYS2 and Cygwin will happily start this on Windows and then fail somewhere
# deep inside CMake, where the real problem is not visible. Say it here instead.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "[test] This is the Linux script. On Windows run Scripts/Windows/test.cmd instead." >&2
        exit 1
        ;;
esac
PRESET="${1:-linux-release}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ctest --preset "$PRESET"
