# Silometer endpoint

Product firmware for a silo-level endpoint: it wakes on a randomised ~30 minute
schedule, reads an 8x8 distance grid from the TMF8829, ships it to the gateway
over point-to-point LoRa, and returns to deep sleep. The wire format is specified
in [docs/protocol/lora-p2p-v0.1.md](../../docs/protocol/lora-p2p-v0.1.md). Two
further build targets exist only to test this one: `bench_gateway` answers an
endpoint so the session can be exercised, and `probe` runs the two bench
measurements the schedule and the framing rest on.

```
embedded/silometer_endpoint/
├── platformio.ini
├── include/
│   ├── app_config.hpp        timing, radio registers, jitter
│   ├── board_pins.h          copy of the hardware_verification one, synced by hand
│   ├── device_id.hpp
│   └── log.hpp
├── lib/
│   ├── e32/                  E32-900T20D driver
│   ├── protocol/             wire format, no hardware dependencies
│   └── tmf8829_shim/         ESP32 port of the vendor Arduino shim
├── scripts/
│   ├── dual_monitor.py       both consoles on one timeline, can drive the fixture
│   ├── fetch_vendor.sh       clones the pinned ams-OSRAM driver into vendor/
│   ├── identify_boards.py    reports which firmware is on which serial port
│   ├── protocol_selftest.sh  host-side wire-format checks, no board needed
│   ├── protocol_selftest.cpp
│   └── hoststub/             esp_random.h / esp_timer.h stubs for the host build
├── vendor/                   gitignored, created by fetch_vendor.sh
│   └── tmf8829/
└── src/
    ├── endpoint/             the firmware
    ├── bench_gateway/        test fixture
    └── probe/                Phase 1 bench measurements
```

## Setup

```bash
uv tool install platformio
./scripts/fetch_vendor.sh
```

`fetch_vendor.sh` pins an upstream commit. Pass `--ref <commit-ish>` to move it
deliberately; do not let it float, because the sensor firmware image ships inside
that checkout.

For WSL2 runs, see the [USB passthrough notes](../README.md).

## Build and flash

`ENDPOINT_ID` is required by every target that touches a board, `bench_gateway`
included — `device_id.hpp` refuses to compile without it. It is the endpoint's
16-bit radio address; `0x0000`, `0xFFFF` and the gateway's `0x0001` are rejected.
A value stored in NVS under `silometer/ep_id` overrides it at runtime.

**Name the port on every command.** Both DevKit V1 boards use a CP2102 with its
serial number left unprogrammed, so they enumerate identically (`SER=0001`) and
`/dev/serial/by-id/` holds only one symlink for the pair. PlatformIO's auto-detect
cannot tell them apart and will send both uploads to whichever port it lists
first, silently flashing one board twice.

```bash
pio device list                  # LOCATION is the usbipd busid; note which is which
```

Run the two roles from separate terminals, each pinned:

```bash
ENDPOINT_ID=0x0007 pio run -e endpoint_bench \
    --upload-port=/dev/ttyUSB0 --monitor-port=/dev/ttyUSB0 -t upload -t monitor
ENDPOINT_ID=0x0002 pio run -e bench_gateway \
    --upload-port=/dev/ttyUSB1 --monitor-port=/dev/ttyUSB1 -t upload -t monitor
```

The other targets take the same `--upload-port` / `--monitor-port` pair:
`endpoint_field` (~30 min cycle, quieter log) and `probe` (Phase 1 bench
measurements). `./scripts/protocol_selftest.sh` needs no board at all.

To check what is already on each board without reflashing:

```bash
./scripts/identify_boards.py                      # resets each board, reads the banner
./scripts/identify_boards.py /dev/ttyUSB1 --seconds 26 --no-reset
```

`--no-reset` listens without disturbing the board, which is how you watch a
sleeping endpoint wake on its own and confirm the timer path.

The bench and field timing profiles are two complete blocks in `app_config.hpp`,
selected by `-DBENCH_TIMING`. Edit the values there; the boot banner prints which
profile is compiled in.

## Bench test

With the gateway fixture on one board and the endpoint on the other, both
consoles on one timeline are what make a session readable - the two halves
interleave and the fragment exchange spans several seconds.

```bash
./scripts/dual_monitor.py --gw /dev/ttyUSB0 --ep /dev/ttyUSB1 --seconds 30 --reset
```

A healthy cycle is HELLO, HELLO_ACK, five fragments, `DATA_ACK mask=0x001F of
0x001F`, the reassembled 8x8 grid, then `deep sleep`. Point the sensor at the same
target as `hardware_verification`'s `tof_read` and the grids must agree zone for
zone.

`--gw-keys` drives the fixture's console partway through a run, to force the paths
that are otherwise hard to reach:

```bash
./scripts/dual_monitor.py --seconds 22 --reset --gw-keys d --keys-at 1.5   # drop a fragment
./scripts/dual_monitor.py --seconds 24 --reset --gw-keys b --keys-at 1.5   # answer BUSY
./scripts/dual_monitor.py --seconds 26 --reset --gw-keys i --keys-at 1.2   # push a 45 s interval
```

A pushed interval persists in NVS and outlives a reflash. Clear it with
`pio run -e endpoint_bench --upload-port=... -t erase` before further bench runs,
or the board keeps the long cycle.

### When flashing goes wrong

* `Could not exclusively lock port ... [Errno 11]` — a monitor still holds it.
  `fuser /dev/ttyUSB1` names the process. A running `-t monitor` must be stopped
  before that board can be reflashed.
* **Wrong board flashed.** With auto-detect both uploads land on one port. Confirm
  with `identify_boards.py`, or by banner: `SILOMETER bench gateway` against
  `I app: silometer endpoint fw ...`.
* **`ttyUSBn` is not the usbipd busid.** Under WSL2 the numbers follow `attach`
  order, not physical sockets: busid `1-1` becomes `/dev/ttyUSB0` and `1-2`
  becomes `/dev/ttyUSB1`. `pio device list` prints the busid as `LOCATION`, which
  is the only thing tying the two namespaces together. Re-check after every replug.
* **One log line repeating thousands of times is not a boot loop.** Resetting into
  a board that is in deep sleep leaves U0TXD undriven and the bridge re-delivers
  the last buffered line. Read the uptime prefixes: one boot, monotonically rising.
  `identify_boards.py` collapses these runs to `(xN)`.
* **`[E][Preferences.cpp:50] nvs_open failed: NOT_FOUND` on a brand-new board** is
  expected exactly once. The namespace is created on that first boot and the error
  does not recur; if it does, NVS is genuinely failing.

## Hardware preconditions

* **Fit the 915 MHz SMA antenna before powering up.** Every target here except the
  host self-test transmits, and transmitting into an open SMA can destroy the PA.
* Unplug the battery at J1 while USB is connected. VBAT is tied directly to the
  DevKit 3V3 pin, so the two together back-feed the on-board regulator.
* The E32 draws ~120 mA transmitting and its manual asks for a supply able to
  deliver 250 mA with under 100 mV of ripple. On the unregulated perfboard
  prototype, fit the bulk capacitor at the module's VCC pins before trusting a
  range test. `cfg::kLoraOption` drops to `0xC5` for 17 dBm if that is not enough.

## Sleep current on this board

The ESP32 module sleeps at roughly 10 µA. **This DevKit V1 carrier board does
not**: its AMS1117 regulator is being back-fed from the 3V3 pin, and the USB
bridge and power LED stay powered. Expect milliamps, and measure rather than
assume. At ~1 mA a 1500 mAh cell lasts about 60 days no matter what the firmware
does, while the active cycle averages under 0.5 mA — less than the board's own
idle. Deep sleep is implemented here for schedule correctness and to have the
architecture right for a future board, not for battery life on this one. A
measurement in the milliamps is the board, not a firmware bug.
