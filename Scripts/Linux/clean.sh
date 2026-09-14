#!/usr/bin/env bash
# Usage: Scripts/Linux/clean.sh [all]
# Removes the intermediate trees vcpkg keeps after it has finished building a library. Nothing here
# needs downloading again. "all" also removes every preset's compiled output, so the next build is a
# full one; it leaves build/vcpkg_installed alone, so that build is a compile and not a download.
set -euo pipefail

# Git Bash, MSYS2 and Cygwin will happily start this on Windows and then fail somewhere
# deep inside CMake, where the real problem is not visible. Say it here instead.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "[clean] This is the Linux script. On Windows run Scripts/Windows/clean.cmd instead." >&2
        exit 1
        ;;
esac
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build"

echo "[clean] vcpkg intermediates"
# The shared tree the presets point at, and any per-preset trees left over from before they shared
# one. Each of those was a full copy of exactly the same libraries.
for dir in "$BUILD/vcpkg_installed/vcpkg/blds" "$BUILD/vcpkg_installed/vcpkg/pkgs"; do
    if [[ -d "$dir" ]]; then
        echo "  $dir"
        rm -rf "$dir"
    fi
done
for preset in "$BUILD"/*/; do
    if [[ -d "$preset/vcpkg_installed" ]]; then
        echo "  $preset/vcpkg_installed  (superseded by the shared one)"
        rm -rf "$preset/vcpkg_installed"
    fi
done
for dir in "${VCPKG_ROOT:-$HOME/vcpkg}/buildtrees" "${VCPKG_ROOT:-$HOME/vcpkg}/packages"; do
    if [[ -d "$dir" ]]; then
        echo "  $dir"
        rm -rf "$dir"
    fi
done

if [[ "${1:-}" == "all" ]]; then
    echo "[clean] compiled output"
    # Every preset folder under build, which is to say everything except the shared dependency tree
    # Anything packaged is in dist, untouched.
    for preset in "$BUILD"/*/; do
        name="$(basename "$preset")"
        if [[ "$name" != "vcpkg_installed" ]]; then
            echo "  $preset"
            rm -rf "$preset"
        fi
    done
fi
echo "[clean] done"
