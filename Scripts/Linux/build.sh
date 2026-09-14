#!/usr/bin/env bash
# Usage: Scripts/Linux/build.sh [preset]      default preset: linux-release
# Configures (if needed) and builds the given CMake preset.
#
# The Windows scripts have a block at the top that holds the console window open when the file is
# double-clicked. Nothing here needs one: a desktop that runs a shell script from a file manager
# either keeps the terminal open itself or was started from one that is already open.
set -euo pipefail

# Git Bash, MSYS2 and Cygwin will happily start this on Windows and then fail somewhere
# deep inside CMake, where the real problem is not visible. Say it here instead.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "[build] This is the Linux script. On Windows run Scripts/Windows/build.cmd instead." >&2
        exit 1
        ;;
esac

PRESET="${1:-linux-release}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ -z "${VCPKG_ROOT:-}" ]]; then
    for candidate in "$HOME/vcpkg" "/opt/vcpkg" "/usr/local/vcpkg"; do
        if [[ -x "$candidate/vcpkg" ]]; then
            export VCPKG_ROOT="$candidate"
            break
        fi
    done
fi
if [[ -z "${VCPKG_ROOT:-}" ]]; then
    echo "[build] VCPKG_ROOT is not set and vcpkg was not found in the usual places."
    echo "[build] Clone it and bootstrap it, then point VCPKG_ROOT at it:"
    echo "[build]   git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh"
    exit 1
fi

cd "$ROOT"
cmake --preset "$PRESET"
cmake --build --preset "$PRESET"
