#!/usr/bin/env bash
# Usage: Scripts/Linux/test.sh [preset]      default preset: linux-release
# Runs the unit tests. They need no window and no sound card, so they run anywhere the code builds.
set -euo pipefail
PRESET="${1:-linux-release}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ctest --preset "$PRESET"
