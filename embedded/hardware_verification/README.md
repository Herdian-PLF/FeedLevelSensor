# Hardware verification

Two standalone ESP32 firmware images that prove the point-to-point wiring of the silometer
perfboard prototype before any application firmware is written: `tof_check` verifies the
ESP32 to TMF8829 I2C link plus the EN and INT lines, and `lora_check` verifies the ESP32 to
E32-900T20D UART link plus the M0, M1 and AUX lines. Both report a PASS/FAIL verdict per step
on the USB serial console at 115200 baud, exercise communication only (no ranging, no RF
transmission), and depend on no vendor driver so that a failure implicates a wire.

```
embedded/hardware_verification/
├── platformio.ini
├── include/
│   ├── board_pins.h
│   └── check_report.h
└── src/
    ├── tof_check/
    │   └── main.cpp
    └── lora_check/
        └── main.cpp
```

Pin assignments live in `include/board_pins.h`, taken from the KiCad project
`hardware_project/silometer/silometer.kicad_sch`. TMF8829 register addresses and bit
positions are those of the vendor Python driver under `python-poc/driver/tmf8829/`.

## Hardware preconditions

* Unplug the battery at J1. VBAT is tied directly to the DevKit 3V3 pin, so USB and cell
  connected together back-feed the on-board regulator. These tests are USB-powered.
* Fit the 915 MHz SMA antenna before running anything that transmits. Neither app here does,
  but application firmware will.

## Setup

```bash
uv tool install platformio
```

For WSL2 runs, check general Windows-WSL USB configuration instructions on [this link](../README.md)

`/dev/ttyUSB0` then appears in WSL2. Without `usbipd-win`, build in WSL2 and flash
`.pio/build/<env>/firmware.bin` from Windows with `esptool`.

## Run

```bash
pio run                                     # build both
pio run -e tof_check  -t upload -t monitor
pio run -e lora_check -t upload -t monitor
```
