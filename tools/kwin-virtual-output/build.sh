#!/usr/bin/env bash
# Build sunshine-ds-virtual-output against this Sunshine tree's KWin protocol.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
XML="$ROOT/third-party/plasma-wayland-protocols/src/protocols/zkde-screencast-unstable-v1.xml"
OUT="${1:-$ROOT/build/sunshine-ds-virtual-output}"
GEN="$(mktemp -d)"
trap 'rm -rf "$GEN"' EXIT
wayland-scanner client-header "$XML" "$GEN/zkde-screencast-unstable-v1.h"
wayland-scanner private-code "$XML" "$GEN/zkde-screencast-unstable-v1.c"
mkdir -p "$(dirname "$OUT")"
gcc -O2 -Wall -Wextra -o "$OUT" \
  "$(dirname "${BASH_SOURCE[0]}")/kwin-virtual-output.c" \
  "$GEN/zkde-screencast-unstable-v1.c" \
  -I"$GEN" -lwayland-client -lm
echo "Built $OUT"
