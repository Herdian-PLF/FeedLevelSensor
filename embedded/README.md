# About

Electronic modules used:
* ESP32-WROOM-32 (30 pins)
* TMF8829 eval board
* EBYTE E32-900T20D (SX1276, 915 MHz, UART)

# Developer Setup

## WSL configuration

On a windows machine, provide ESP32 access through an **admin** windows terminal:

```
winget install usbipd
usbipd list                          # find the busid of the CP2102/CH340
usbipd bind   --busid <busid>        # once per device, needs admin
usbipd attach --wsl --busid <busid>  # after each replug
```

Verify on a WSL terminal through:

```
ls /dev/ttyUSB0
```