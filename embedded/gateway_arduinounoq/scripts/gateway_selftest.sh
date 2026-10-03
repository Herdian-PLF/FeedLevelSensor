#!/usr/bin/env bash
# Builds and runs the gateway session checks on the host. No board required.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

g++ -std=c++17 -Wall -Wextra -Werror \
  -I "$ROOT/sketch/src" -I "$ROOT/sketch/src/protocol" \
  "$ROOT/test/gateway_selftest.cpp" "$ROOT/sketch/src/session.cpp" \
  "$ROOT/sketch/src/protocol/protocol.cpp" \
  -o "$OUT/selftest"
"$OUT/selftest"
