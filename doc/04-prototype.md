# Prototype

Today's hardware is **two modules wired together**: a Waveshare RP2350-Zero and a Waveshare Core2021. The only interface it exposes is **USB-C** (power, data, config, flashing); CRSF and data are two UARTs on the module pads and need wires of your own. No button is fitted: a jumper wire from GP12 to GND stands in for it.

Pins and aliases are defined in the [board README](../boards/zlrs/rp2350_zero_core2021/README.md); the port and mode design is in [08-firmware](08-firmware.md) 2 and 5.

## 1. What it is made of

| Part | Spec | Notes |
| --- | --- | --- |
| MCU | Waveshare RP2350-Zero: RP2350A (Cortex-M33), 4 MB QSPI flash, native USB-C, on-board WS2812B | BOOTSEL and RESET live on the module |
| Radio | Waveshare Core2021 (Semtech LR2021), LF SKU | one chip, time-sharing two bands: sub-G and 2.4G |
| Antenna | 433 MHz on the module's connector | check the 2.4G path against the module drawing once it is on a board |
| Power | USB-C | the Zero feeds 3V3 to the Core2021 and the two share ground |
| Button | none fitted: short GP12 to GND with a wire | the devicetree enables the internal pull-up |
| Status LED | the module's own WS2812 (GP16) | semantics in [11-ux](11-ux.md) |

## 2. Wiring between the modules (Zero to Core2021)

Only used when `CONFIG_ZLRS_APP_RADIO=y`; with `=n` nothing touches the radio, SPI and the three control lines included.

| Signal | RP2350-Zero | Core2021 | Notes |
| --- | --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | matching pins | 16 MHz bus |
| SPI0 CS | GP5 | matching pin | a plain GPIO chip select (`cs-gpios`), not the hardware CSN |
| BUSY | GP13 | BUSY | active high, internal pull-up |
| NRESET | GP14 | NRESET | active low |
| DIO8 (IRQ) | GP15 | DIO8 | active high, internal pull-down |
| 3V3 / GND | 3V3, GND | 3V3, GND | supplied by the Zero |

All seven wires stay at **GP15 or below** (SPI on GP4-GP7, control lines on GP13-GP15): GP16 drives the on-board WS2812B and the pins above it are awkward to reach on the prototype.

The two UARTs (GP0/GP1 for CRSF, GP8/GP9 for data) stay on the module pads and are unconnected here; the console never uses pins, only USB.

## 3. Interfaces and modes

One USB-C port that shows up differently depending on the mode:

| Mode | What USB shows |
| --- | --- |
| Application | three CDCs: CRSF, data, config |
| Serial recovery | two CDCs: recovery and console |
| ROM BOOTSEL | an `RPI-RP2` mass-storage volume (drag a UF2 onto it) |
| Debug | the above plus one more console CDC (logs and shell), see [12-debug](12-debug.md) |

| Interface | Application | Config | Serial recovery | ROM BOOTSEL |
| --- | --- | --- | --- | --- |
| USB-C | three CDCs | the config CDC speaks the config protocol, radio stopped | two CDCs | `RPI-RP2` |
| UART0 GP0/1 | CRSF at 420000 | not forwarded (radio stopped) | not initialised | no firmware |
| UART1 GP8/9 | passthrough | incoming bytes are dropped and counted | unused | no firmware |
| Button (jumper) | hold 1 s or more to pair | ignored once config mode is up | unused | BOOTSEL is the chip-level button |
| Status LED | see [11-ux](11-ux.md) | white at 1 Hz | MCUboot does not drive it | not driven |

Config mode is entered by a command the config tool sends on the config CDC; details in [08-firmware](08-firmware.md) 2.

## 4. Flashing

| Variant | First time | Afterwards |
| --- | --- | --- |
| Without a bootloader | hold BOOTSEL and drag `zephyr.uf2` on, or use SWD | the same |
| With MCUboot | two BOOTSEL rounds: the bootloader, then `zephyr.signed.uf2` | trigger recovery from the application and upload a signed image with `smpmgr` |

One command upgrades it: `python3 tools/zlrs-update.py --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04` (the config port; the tool finds the bootloader's recovery port itself).

## 5. Wiring and placement

- The Zero feeds 3V3 to the Core2021 and the two share ground.
- Keep the SPI jumpers short; GP16 belongs to the on-board WS2812B, so the radio bus uses GP4-GP7 and the control lines GP13-GP15, all at GP15 or below.
- Leave 15-20 mm of clearance on the antenna side: no metal, no wires.
