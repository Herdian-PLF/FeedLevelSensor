from arduino.app_utils import *

from collections import deque
import time

REPORT_INTERVAL_MS = 2000
UPLINK_PERIOD_S = 10

# The Bridge handler runs on its own thread; deque is the handoff to the uplink loop.
_pending = deque()
_interval_applied = False


def on_sample(endpoint_id: int, seq: int, distance_mm: int):
    _pending.append((time.time(), endpoint_id, seq, distance_mm))
    print(f"sample ep=0x{endpoint_id:04x} seq={seq} distance={distance_mm}mm", flush=True)


def _apply_report_interval():
    global _interval_applied
    try:
        applied = Bridge.call("set_report_interval", REPORT_INTERVAL_MS, timeout=2)
    except (TimeoutError, RuntimeError, ValueError) as err:
        print(f"MCU not answering set_report_interval yet: {err}", flush=True)
        return
    _interval_applied = True
    print(f"MCU reporting every {applied} ms", flush=True)


def _drain():
    batch = []
    while True:
        try:
            batch.append(_pending.popleft())
        except IndexError:
            return batch


def loop():
    if not _interval_applied:
        _apply_report_interval()

    time.sleep(UPLINK_PERIOD_S)

    batch = _drain()
    if not batch:
        print("uplink: nothing to send", flush=True)
        return
    span = batch[-1][0] - batch[0][0]
    print(f"uplink: {len(batch)} samples over {span:.1f}s (cloud push goes here)", flush=True)


Bridge.provide("on_sample", on_sample)
App.run(user_loop=loop)
