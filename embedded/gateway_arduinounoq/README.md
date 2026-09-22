# Gateway (Arduino UNO Q)

Gateway firmware and MPU service for the silo network: the MCU will own the
E32-900T20D and the point-to-point protocol of
[docs/protocol/lora-p2p-v0.1.md](../../docs/protocol/lora-p2p-v0.1.md), the MPU
takes the decoded samples and pushes them to the cloud. This directory is itself
an Arduino App Lab app, so it can be copied onto a board as-is. At this stage it
carries no radio: the MCU emits a synthetic sample every two seconds and the MPU
sets that cadence back over the Bridge, which exercises both directions of the
MCU<->MPU link and nothing else.

```
embedded/gateway_arduinounoq/
├── app.yaml            App Lab manifest
├── python/main.py      MPU side, runs in a container on the board
├── sketch/
│   ├── sketch.ino      MCU side
│   └── sketch.yaml     board profile and MCU libraries
└── scripts/unoq.sh     repo <-> board sync, not copied to the board
```

On the board the app lives at `/home/arduino/ArduinoApps/gateway_arduinounoq`,
which is where App Lab reads and writes it. The repo is the canonical copy; the
board copy is a working copy that `unoq.sh` moves in either direction.

## Setup

The board talks over adb on the USB-C data port. Confirm devices is available:

```bash
adb devices                # the UNO Q shows up as a device
```

With more than one adb device attached, export `UNOQ_SERIAL=<serial>`.
`scripts/unoq.sh` needs `adb` and `rsync` on the dev machine and nothing else;
the board already ships `arduino-app-cli`.

## Workflow

```bash
./scripts/unoq.sh run        # push, build, flash the MCU, follow the MPU log
./scripts/unoq.sh diff       # what differs between repo and board
./scripts/unoq.sh pull       # bring App Lab edits back, then git diff
./scripts/unoq.sh monitor    # MCU serial monitor
```

`push` replaces everything on the board except `.cache`, so the sketch is not
rebuilt from scratch every time. `push` and `pull` print what they are about to
overwrite and take `-n` to stop after that.

Editing in App Lab is optional — the board copy is what App Lab shows, so the
same `run` works whether the edit came from App Lab or from the repo.
