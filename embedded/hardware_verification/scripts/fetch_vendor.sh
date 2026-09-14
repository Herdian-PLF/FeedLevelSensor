#!/usr/bin/env bash
# Fetches the ams-OSRAM TMF8829 Arduino driver into vendor/ (gitignored).
# Only the core driver and the firmware image are kept: the shim is ours
# (src/tof_read/tmf8829_shim.h) because the vendor one targets the Arduino Uno.
set -euo pipefail

REPO="https://github.com/ams-OSRAM/tmf8829_driver_arduino.git"
DEST="$(cd "$(dirname "$0")/.." && pwd)/vendor/tmf8829"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

git clone --depth 1 --quiet "$REPO" "$TMP/driver"
mkdir -p "$DEST"
for f in tmf8829.c tmf8829.h tmf8829_image.c tmf8829_image.h; do
  cp "$TMP/driver/tmf8829/$f" "$DEST/$f"
done
cp "$TMP/driver/LICENSES-MIT.TXT" "$DEST/"
echo "vendor driver in $DEST"
