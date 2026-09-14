#!/usr/bin/env bash
# Usage: Scripts/Linux/clean.sh [all]
# Removes the intermediate trees vcpkg keeps after it has finished building a library. Nothing here
# needs downloading again. "all" also removes the compiled output, so the next build is a full one.
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

echo "[clean] vcpkg intermediates"
for preset in linux-debug linux-release linux-relwithdebinfo; do
    for dir in "$ROOT/build/$preset/vcpkg_installed/vcpkg/blds" "$ROOT/build/$preset/vcpkg_installed/vcpkg/pkgs"; do
        if [[ -d "$dir" ]]; then
            echo "  $dir"
            rm -rf "$dir"
        fi
    done
done
for dir in "${VCPKG_ROOT:-$HOME/vcpkg}/buildtrees" "${VCPKG_ROOT:-$HOME/vcpkg}/packages"; do
    if [[ -d "$dir" ]]; then
        echo "  $dir"
        rm -rf "$dir"
    fi
done

if [[ "${1:-}" == "all" ]]; then
    echo "[clean] compiled output"
    for preset in linux-debug linux-release linux-relwithdebinfo; do
        for dir in bin Engine Game Tools Tests; do
            if [[ -d "$ROOT/build/$preset/$dir" ]]; then
                echo "  $ROOT/build/$preset/$dir"
                rm -rf "$ROOT/build/$preset/$dir"
            fi
        done
    done
fi
echo "[clean] done"
