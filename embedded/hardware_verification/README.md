# Hardware verification

Standalone ESP32 firmware images for bringing up the silometer perfboard prototype, each
reporting to the USB serial console at 115200 baud: `tof_check` verifies the ESP32 to TMF8829
I2C link plus the EN and INT lines, `lora_check` verifies the ESP32 to E32-900T20D UART link
plus the M0, M1 and AUX lines, and `tof_read` boots the sensor's application firmware and
prints an 8x8 distance frame every 5 seconds. The two `*_check` apps exercise communication
only (no ranging, no RF transmission) and depend on no vendor driver, so that a failure
implicates a wire; `tof_read` uses the ams-OSRAM driver fetched into `vendor/`.

```
embedded/hardware_verification/
├── platformio.ini
├── include/
│   ├── board_pins.h
│   └── check_report.h
├── scripts/
│   ├── fetch_vendor.sh
│   └── gen_compiledb.sh
├── vendor/                  # gitignored, created by fetch_vendor.sh
│   └── tmf8829/
└── src/
    ├── tof_check/
    │   └── main.cpp
    ├── lora_check/
    │   └── main.cpp
    └── tof_read/
        ├── main.cpp
        ├── tmf8829_shim.h
        └── tmf8829_shim.cpp
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
./scripts/fetch_vendor.sh        # only needed for tof_read
```

`fetch_vendor.sh` clones ams-OSRAM/tmf8829_driver_arduino and keeps `tmf8829.c/.h` and the
firmware image. The vendor shim targets the Arduino Uno, so `src/tof_read/tmf8829_shim.h` and
`.cpp` replace it and also carry the result callbacks.

For WSL2 runs, check general Windows-WSL USB configuration instructions on [this link](../README.md)

`/dev/ttyUSB0` then appears in WSL2. Without `usbipd-win`, build in WSL2 and flash
`.pio/build/<env>/firmware.bin` from Windows with `esptool`.

## Run

```bash
pio run                                     # build both
pio run -e tof_check  -t upload -t monitor
pio run -e lora_check -t upload -t monitor
pio run -e tof_read   -t upload -t monitor
```
