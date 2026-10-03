# Gateway (Arduino UNO Q)

Farm gateway for the silo network. The MCU owns the E32-900T20D and both sides of
the point-to-point session specified in
[docs/protocol/lora-p2p-v0.1.md](../../docs/protocol/lora-p2p-v0.1.md) and hands every closed
window to the MPU over the Bridge, where readings are batched and pushed upstream.
The gateway address is fixed at `0x0001`, one per farm. This directory is itself an
Arduino App Lab app, so it can be copied onto a board as-is.

```
embedded/gateway_arduinounoq/
├── app.yaml                    App Lab manifest
├── python/main.py              MPU side, runs in a container on the board
├── sketch/
│   ├── sketch.ino              MCU side, wires the radio to the session and the Bridge
│   ├── sketch.yaml             pinned core and library versions
│   └── src/
│       ├── gateway_config.h    pins and timings the gateway alone owns
│       ├── e32_radio.{h,cpp}   E32-900T20D driver, UNO Q port
│       ├── session.{h,cpp}     p2p session, free of Arduino and the radio
│       ├── monitor_log.h
│       └── protocol/           symlink to the endpoint's wire-format library
├── test/gateway_selftest.cpp
└── scripts/
    ├── unoq.sh                 repo <-> board sync, not copied to the board
    └── gateway_selftest.sh     host-side session checks, no board needed
```

`sketch/src/protocol` is a symlink to
[`embedded/silometer_endpoint/lib/protocol/`](../silometer_endpoint/lib/protocol),
so the two firmwares encode and decode the same frames from one source. `unoq.sh`
dereferences it on the way to the board and never writes it back.

On the board the app lives at `/home/arduino/ArduinoApps/gateway_arduinounoq`,
which is where App Lab reads and writes it. The repo is the canonical copy; the
board copy is a working copy that `unoq.sh` moves in either direction.

## Wiring

E32 net names are written from the module's point of view, so `LORA_RXD` carries
UNO Q transmit data.

| E32 pin | UNO Q |
|---|---|
| 1 M0 | D2 |
| 2 M1 | D3 |
| 3 RXD | D1 (Serial1 TX) |
| 4 TXD | D0 (Serial1 RX) |
| 5 AUX | D4 |

## Setup

`adb` and `rsync` are needed on the development machine; the board needs neither.

```bash
adb devices          # the board must be listed before any unoq.sh command
./scripts/gateway_selftest.sh
```

Set `UNOQ_SERIAL` when more than one device is attached. For WSL2, the board is
reached through the same `usbipd` passthrough as the ESP32 boards — see the
[USB passthrough notes](../README.md).

## Workflow

```bash
./scripts/unoq.sh run     # push, build, flash the MCU, follow the log
./scripts/unoq.sh diff    # repo against board, touching nothing
./scripts/unoq.sh pull    # App Lab edits back into the repo
```

One log carries both halves. `arduino-app-cli monitor` attaches nothing while the
app is running, so the MCU sends its diagnostics over the Bridge as well and they
appear in the app log prefixed `mcu:`:

```
mcu: gw: HELLO from 0x0002 seq=48 valid=64 boot=561 reset=8 fails=0 flags=0x00
hello 0x0002 seq=48 valid=64/64 T=26C boot=561 fails=0 flags=0x00
mcu: gw: reading 0x0002 seq=48 mask=0x001F of 0x001F
reading 0x0002 seq=48 complete, 2 queued
  uplink 0x0002 seq=48 complete zones_with_target=64/64 nearest=107mm
```

`push` replaces everything on the board except `.cache`, which is kept so an
edit does not cost a full sketch rebuild. Both directions print what they are
about to overwrite and take `-n` to stop there. Nothing merges: `pull` before
`push` when the board has been edited.

Config is pushed down from the MPU, and reaches an endpoint on its next
`DATA_ACK`:

```python
queue_config(0x0007, interval_s=900)
queue_config(0x0007, channel=0x36, reboot=True)
```

## Hardware preconditions

* **Fit the 915 MHz SMA antenna before powering up.** transmitting into an open SMA
  can destroy the PA.
* **Fit pull-ups on M0 and M1 to 3V3.** Until Zephyr has brought the router UART
  up, the Bridge falls back to Serial1, so the E32 is fed a burst of router
  traffic on its RXD before the sketch runs. With M0/M1 floating the module may
  be in a transmitting mode for that window and will put the noise on air.
  Pull-ups hold it in mode 3 until `setup()` takes the pins over.
* The E32 draws ~120 mA transmitting and its manual asks for a supply able to
  deliver 250 mA with under 100 mV of ripple. Fit the bulk capacitor at the
  module's VCC pins before trusting a range test.

## Pinned versions

`sketch.yaml` pins `arduino:zephyr` 1.0.0. The gateway needs Serial1 for the radio
while the Bridge runs on lpuart1, and
[Arduino_RouterBridge issue #76](https://github.com/arduino-libraries/Arduino_RouterBridge/issues/76)
reported that opening a second UART killed the Bridge for the rest of the boot on
core 0.90.0. It does not reproduce on 1.0.0. Moving off this version means
re-testing that the radio and the Bridge coexist before trusting a session.
