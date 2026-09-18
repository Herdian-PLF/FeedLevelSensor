#!/usr/bin/env bash
# Builds and runs the wire-format checks on the host. No board required.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

g++ -std=c++17 -Wall -Wextra -Werror \
  -DBENCH_TIMING -DENDPOINT_ID=0x0007 \
  -I "$ROOT/include" -I "$ROOT/lib/protocol" -I "$ROOT/scripts/hoststub" \
  "$ROOT/scripts/protocol_selftest.cpp" "$ROOT/lib/protocol/protocol.cpp" \
  -o "$OUT/selftest"
"$OUT/selftest"
