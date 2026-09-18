#!/usr/bin/env python3
"""Reports which firmware is on each attached board, by resetting it and reading the boot banner.

Both DevKit V1 boards enumerate as CP2102 with the serial number left unprogrammed
(SER=0001), so nothing in /dev distinguishes them and PlatformIO's auto-detect
cannot either. The banner is the only reliable answer.

Args:
    ports: serial devices to probe; defaults to every /dev/ttyUSB*.
    --seconds: how long to listen (default 6).
    --no-reset: listen without resetting, to watch a sleeping board wake on its own.
"""

import argparse
import glob
import os
import shutil
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover - re-exec path
    # pyserial ships inside PlatformIO's own interpreter, not the system one.
    # Find it via the `pio` launcher's shebang and re-exec there, so this script
    # runs as ./scripts/identify_boards.py without the caller knowing any of that.
    if os.environ.get("_IDENTIFY_BOARDS_REEXEC"):
        sys.exit("pyserial not available; try: pip install pyserial")
    launcher = shutil.which("pio") or shutil.which("platformio")
    interpreter = None
    if launcher:
        with open(launcher, "rb") as handle:
            first = handle.readline()
        if first.startswith(b"#!"):
            candidate = first[2:].strip().decode()
            if os.path.exists(candidate):
                interpreter = candidate
    if not interpreter:
        sys.exit("pyserial not available and PlatformIO's interpreter was not found; "
                 "try: pip install pyserial")
    os.environ["_IDENTIFY_BOARDS_REEXEC"] = "1"
    os.execv(interpreter, [interpreter, os.path.abspath(__file__)] + sys.argv[1:])

BANNERS = (
    ("SILOMETER bench gateway", "bench_gateway"),
    ("silometer endpoint fw", "endpoint"),
    ("SILOMETER probe", "probe"),
    ("SILOMETER - TMF8829 measurement readout", "tof_read (hardware_verification)"),
    ("SILOMETER LoRa link check", "lora_link (hardware_verification)"),
)


# A board in deep sleep leaves U0TXD undriven and the bridge re-delivers the last
# buffered line thousands of times. Collapsing runs keeps that from burying the
# one boot in the noise.
def collapse(text: str) -> list:
    out = []
    for line in text.splitlines():
        line = line.rstrip()
        if not line.strip():
            continue
        if out and out[-1][0] == line:
            out[-1][1] += 1
        else:
            out.append([line, 1])
    return out


def probe(port: str, seconds: float, reset: bool) -> None:
    print(f"\n=== {port}")
    try:
        link = serial.Serial(port, 115200, timeout=0.2, exclusive=True)
    except serial.SerialException as exc:
        print(f"  cannot open: {exc}")
        print("  a running monitor holds the port; stop it first")
        return

    with link:
        if reset:
            # The DevKit's auto-reset circuit drives EN from RTS and IO0 from DTR.
            # DTR low throughout keeps IO0 high, so the chip resets into the
            # application rather than the ROM bootloader.
            link.dtr = False
            link.rts = True
            time.sleep(0.1)
            link.rts = False
        link.reset_input_buffer()

        deadline = time.time() + seconds
        text = ""
        while time.time() < deadline:
            chunk = link.read(256)
            if chunk:
                text += chunk.decode("utf-8", "replace")

    for line, count in collapse(text):
        suffix = f"   (x{count})" if count > 1 else ""
        print(f"  | {line}{suffix}")

    for needle, name in BANNERS:
        if needle in text:
            print(f"  -> {name}")
            return
    print("  -> unrecognised (no banner seen; board may be asleep or unflashed)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ports", nargs="*", help="serial devices; default every /dev/ttyUSB*")
    parser.add_argument("--seconds", type=float, default=6.0, help="listen window")
    parser.add_argument("--no-reset", action="store_true", help="do not reset before listening")
    args = parser.parse_args()

    ports = args.ports or sorted(glob.glob("/dev/ttyUSB*"))
    if not ports:
        print("no /dev/ttyUSB* found - is usbipd attached?", file=sys.stderr)
        return 1
    for port in ports:
        probe(port, args.seconds, reset=not args.no_reset)
    return 0


if __name__ == "__main__":
    sys.exit(main())
