#!/usr/bin/env python3
"""Logs two boards' consoles onto one timeline, so a session can be read end to end.

Args:
    --gw / --ep: serial devices for the gateway and the endpoint.
    --seconds: how long to listen.
    --reset: pulse each board's reset before listening, for a clean start.
    --gw-keys / --keys-at: send console keys to the gateway fixture partway through,
        to force a failure path (d=drop fragment, b=busy, r=reject, i=push interval).
"""

import argparse
import os
import shutil
import sys
import threading
import time

try:
    import serial
except ImportError:  # pragma: no cover - re-exec path
    if os.environ.get("_DUAL_MONITOR_REEXEC"):
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
        sys.exit("pyserial not available and PlatformIO's interpreter was not found")
    os.environ["_DUAL_MONITOR_REEXEC"] = "1"
    os.execv(interpreter, [interpreter, os.path.abspath(__file__)] + sys.argv[1:])

lock = threading.Lock()
start = time.time()


def pump(port: str, tag: str, seconds: float, reset: bool, keys: str = "",
         keys_at: float = 0.0) -> None:
    try:
        link = serial.Serial(port, 115200, timeout=0.1, exclusive=True)
    except serial.SerialException as exc:
        with lock:
            print(f"{tag}: cannot open {port}: {exc}")
        return

    with link:
        if reset:
            link.dtr = False
            link.rts = True
            time.sleep(0.1)
            link.rts = False
        link.reset_input_buffer()

        pending = ""
        last = ""
        repeats = 0
        sent_keys = not keys
        deadline = time.time() + seconds
        while time.time() < deadline:
            if not sent_keys and (time.time() - start) >= keys_at:
                link.write(keys.encode())
                link.flush()
                sent_keys = True
                with lock:
                    print(f"  {time.time() - start:8.3f}  {tag} < sent key(s) {keys!r}")
            chunk = link.read(256)
            if not chunk:
                continue
            pending += chunk.decode("utf-8", "replace")
            while "\n" in pending:
                line, pending = pending.split("\n", 1)
                line = line.rstrip()
                if not line.strip():
                    continue
                # A sleeping board's undriven TX makes the bridge repeat the last
                # line; collapse those so a real event is never buried.
                if line == last:
                    repeats += 1
                    continue
                with lock:
                    if repeats:
                        print(f"  {' ':>8}  {tag} | ... previous line x{repeats}")
                        repeats = 0
                    print(f"  {time.time() - start:8.3f}  {tag} | {line}")
                last = line


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gw", default="/dev/ttyUSB0")
    parser.add_argument("--ep", default="/dev/ttyUSB1")
    parser.add_argument("--seconds", type=float, default=45.0)
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--gw-keys", default="", help="console keys to send to the gateway")
    parser.add_argument("--keys-at", type=float, default=2.0, help="when to send them, seconds")
    args = parser.parse_args()

    print(f"  {'t (s)':>8}  who | line")
    threads = [
        threading.Thread(target=pump, args=(args.gw, "GW", args.seconds, args.reset,
                                            args.gw_keys, args.keys_at)),
        threading.Thread(target=pump, args=(args.ep, "EP", args.seconds, args.reset)),
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return 0


if __name__ == "__main__":
    sys.exit(main())
