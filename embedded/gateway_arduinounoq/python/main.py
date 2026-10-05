"""MPU half of the silo gateway: it takes decoded readings from the MCU over the
Bridge, keeps the most complete copy of each transmission window, and pushes
batches upstream. The radio, the framing and the session live on the MCU."""

from arduino.app_utils import *

import threading
import time

UPLINK_PERIOD_S = 30
GRID_SIDE = 8
ZONE_COUNT = GRID_SIDE * GRID_SIDE

# A window the MCU could not complete is still worth sending; a reading older
# than this has been superseded by the endpoint's next window anyway.
STALE_READING_S = 1800

_lock = threading.Lock()

# Keyed by (endpoint, seq): the MCU forwards a window again whenever a retry
# round gains a fragment, so the last copy of a key is the most complete one.
_readings = {}
_telemetry = {}

# The grid arrives in fragment-sized pieces, because one RPC cannot carry all 64
# zones inside RPClite's 256-byte request buffer. _grids accumulates them until
# on_reading closes the window.
_grids = {}


def _now():
    return time.time()


def on_gateway_up(gateway_id: int, channel: int, fragments: int, fw_version: int):
    print(
        f"MCU up: gateway 0x{gateway_id:04x} on channel 0x{channel:02x} "
        f"({862 + channel} MHz), {fragments} fragments per window, fw 0x{fw_version:02x}",
        flush=True,
    )


def on_mcu_log(line: str):
    print(f"mcu: {line}", flush=True)


def on_gateway_fault(gateway_id: int):
    print(f"MCU 0x{gateway_id:04x} is up but its radio is not; no readings will arrive", flush=True)


def on_hello(
    endpoint: int,
    seq: int,
    flags: int,
    temperature_c: int,
    valid_zones: int,
    frame_number: int,
    boot_count: int,
    failed_cycles: int,
    fw_version: int,
):
    record = {
        "flags": flags,
        "sensor_fault": bool(flags & 0x01),
        "degraded": bool(flags & 0x02),
        "retry": bool(flags & 0x04),
        "temperature_c": temperature_c,
        "valid_zones": valid_zones,
        "frame_number": frame_number,
        "boot_count": boot_count,
        "failed_cycles": failed_cycles,
        "fw_version": fw_version,
    }
    with _lock:
        _telemetry[(endpoint, seq)] = {"received_at": _now(), **record}

    print(
        f"hello 0x{endpoint:04x} seq={seq} valid={valid_zones}/64 T={temperature_c}C "
        f"boot={boot_count} fails={failed_cycles} flags=0x{flags:02x}",
        flush=True,
    )


def on_zones(endpoint: int, seq: int, first_zone: int, distance_mm, snr):
    key = (endpoint, seq)
    with _lock:
        grid = _grids.get(key)
        if grid is None:
            # 0 is the sensor's own no-target sentinel, so an unfilled zone and a
            # zone that saw nothing read the same. The fragment mask on the
            # closing on_reading is what tells the two apart.
            grid = {"distance_mm": [0] * ZONE_COUNT, "snr_raw": [0] * ZONE_COUNT}
            _grids[key] = grid
        for offset, (distance, snr_raw) in enumerate(zip(distance_mm, snr)):
            grid["distance_mm"][first_zone + offset] = distance
            grid["snr_raw"][first_zone + offset] = snr_raw


def on_reading(endpoint: int, seq: int, mask: int, complete: int):
    key = (endpoint, seq)
    with _lock:
        grid = _grids.pop(key, None)
        if grid is None:
            print(f"reading 0x{endpoint:04x} seq={seq} closed with no zones; dropped", flush=True)
            return
        _readings[key] = {
            "endpoint": endpoint,
            "seq": seq,
            "received_at": _now(),
            "fragment_mask": mask,
            "complete": bool(complete),
            "telemetry": _telemetry.get(key),
            **grid,
        }
        pending = len(_readings)

    state = "complete" if complete else f"partial mask=0x{mask:04x}"
    print(f"reading 0x{endpoint:04x} seq={seq} {state}, {pending} queued", flush=True)


def push_to_cloud(batch):
    """Hand a batch of readings to the backend. Placeholder: the backend is not
    chosen yet, so this only reports what would go out. Raise on failure and the
    batch is re-queued for the next uplink."""
    for reading in batch:
        grid = reading["distance_mm"]
        lit = [d for d in grid if d != 0]  # 0 is the sensor's no-target sentinel
        nearest = min(lit) if lit else None
        print(
            f"  uplink 0x{reading['endpoint']:04x} seq={reading['seq']} "
            f"{'complete' if reading['complete'] else 'partial'} "
            f"zones_with_target={len(lit)}/{ZONE_COUNT} nearest={nearest}mm",
            flush=True,
        )


def queue_config(endpoint: int, interval_s=None, channel=None, tof_frames=None, reboot=False):
    """Queue a config change for one endpoint, delivered on its next DATA_ACK.

    endpoint    16-bit endpoint address
    interval_s  reporting interval in seconds, or None to leave it alone
    channel     LoRa channel, or None; the MCU rejects one outside the ANATEL grants
    tof_frames  sensor frames per reading, or None
    reboot      True to ask the endpoint to restart after applying the rest
    """
    accepted = Bridge.call(
        "queue_config",
        endpoint,
        -1 if interval_s is None else int(interval_s),
        -1 if channel is None else int(channel),
        -1 if tof_frames is None else int(tof_frames),
        1 if reboot else 0,
        timeout=5,
    )
    return bool(accepted)


def _take_batch():
    cutoff = _now() - STALE_READING_S
    with _lock:
        batch = [r for r in _readings.values() if r["received_at"] >= cutoff]
        _readings.clear()
        # Telemetry is pruned by age, not by which readings just left: a HELLO
        # whose fragment burst is still on air has no reading to attach to yet.
        for key in [k for k, t in _telemetry.items() if t["received_at"] < cutoff]:
            _telemetry.pop(key)
        # A window whose closing on_reading never arrived would otherwise sit here
        # for good.
        for key in [k for k in _grids if k not in _telemetry]:
            _grids.pop(key)
    return sorted(batch, key=lambda r: r["received_at"])


def _requeue(batch):
    with _lock:
        for reading in batch:
            key = (reading["endpoint"], reading["seq"])
            _readings.setdefault(key, reading)


def loop():
    time.sleep(UPLINK_PERIOD_S)

    batch = _take_batch()
    if not batch:
        return

    try:
        push_to_cloud(batch)
    except Exception as err:
        # Readings cost an endpoint a wake-up and a transmission each; a backend
        # that is briefly down must not silently discard them.
        _requeue(batch)
        print(f"uplink failed, {len(batch)} readings re-queued: {err}", flush=True)
        return

    print(f"uplink: {len(batch)} readings sent", flush=True)


Bridge.provide("on_mcu_log", on_mcu_log)
Bridge.provide("on_gateway_up", on_gateway_up)
Bridge.provide("on_gateway_fault", on_gateway_fault)
Bridge.provide("on_hello", on_hello)
Bridge.provide("on_zones", on_zones)
Bridge.provide("on_reading", on_reading)


def _ask_mcu_status():
    # The MCU's own on_gateway_up goes out before this container exists, whether
    # at boot or after a flash. On a cold boot the reverse can hold, and the MCU
    # is still in setup() when this runs, hence the retries.
    for _ in range(10):
        try:
            Bridge.call("gateway_status")
            return
        except Exception as err:
            last_err = err
            time.sleep(3)
    print(f"MCU never answered gateway_status: {last_err}", flush=True)


threading.Thread(target=_ask_mcu_status, daemon=True).start()

App.run(user_loop=loop)
