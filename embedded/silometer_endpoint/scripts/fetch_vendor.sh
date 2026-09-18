#!/usr/bin/env bash
# Fetches the ams-OSRAM TMF8829 Arduino driver into vendor/ (gitignored).
# Only the core driver and the firmware image are kept: the shim is ours
# (lib/tmf8829_shim/) because the vendor one targets the Arduino Uno.
#
# The commit is pinned. Product firmware cannot ride on a moving branch: the
# sensor image ships inside this repo and a silent upstream change would be
# indistinguishable from a bug in our own code.
#
# Usage: fetch_vendor.sh [--ref <commit-ish>]
set -euo pipefail

REPO="https://github.com/ams-OSRAM/tmf8829_driver_arduino.git"
REF="61567895ff0d960f705c39ef9137c0bbb2d5d3d2"  # HEAD of main, verified 2026-09-14

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ref) REF="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

DEST="$(cd "$(dirname "$0")/.." && pwd)/vendor/tmf8829"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

git clone --quiet "$REPO" "$TMP/driver"
git -C "$TMP/driver" checkout --quiet "$REF"
mkdir -p "$DEST"
for f in tmf8829.c tmf8829.h tmf8829_image.c tmf8829_image.h; do
  cp "$TMP/driver/tmf8829/$f" "$DEST/$f"
done
cp "$TMP/driver/LICENSES-MIT.TXT" "$DEST/"
echo "$REF" > "$DEST/.pinned-ref"
echo "vendor driver at $REF in $DEST"
