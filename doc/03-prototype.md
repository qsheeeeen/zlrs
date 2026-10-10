# Prototype

Today's hardware is **two modules wired together**: a Waveshare RP2350-Zero and a Waveshare
Core2021. Its assembled interface is **USB-C** (power, data, management, flashing); the DATA
UART is available on the module pads for external wiring. No button is fitted: a jumper
wire from GP12 to GND stands in for it.

Pins and aliases are defined in the [board README](../boards/zlrs/rp2350_zero_core2021/README.md);
port behavior in [07-firmware](07-firmware.md).

## 1. What it is made of

| Part | Spec | Notes |
| --- | --- | --- |
| MCU | Waveshare RP2350-Zero: RP2350A (Cortex-M33), 4 MB QSPI flash, native USB-C, on-board WS2812B | BOOTSEL and RESET live on the module |
| Radio | Waveshare Core2021 (Semtech LR2021), LF SKU | one chip supporting sub-G and 2.4G DATA operation |
| Antenna | 433 MHz on the module's connector | check the 2.4G path against the module drawing once it is on a board |
| Power | USB-C | the Zero feeds 3V3 to the Core2021 and the two share ground |
| Button | none fitted: short GP12 to GND with a wire | the devicetree enables the internal pull-up |
| Status LED | the module's own WS2812 (GP16) | semantics in [10-ux](10-ux.md) |

## 2. Wiring between the modules

Only used by the radio: SPI and its three control lines.

| Signal | RP2350-Zero | Core2021 | Notes |
| --- | --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | matching pins | 16 MHz bus |
| SPI0 CS | GP5 | matching pin | a plain GPIO chip select (`cs-gpios`), not the hardware CSN |
| BUSY | GP13 | BUSY | active high, internal pull-up |
| NRESET | GP14 | NRESET | active low |
| DIO8 (IRQ) | GP15 | DIO8 | active high, internal pull-down |
| 3V3 / GND | 3V3, GND | 3V3, GND | supplied by the Zero |

All seven wires stay at **GP15 or below** (SPI on GP4-GP7, control lines on GP13-GP15): GP16
drives the on-board WS2812B and the pins above it are awkward to reach on the prototype.

The DATA UART (GP8/GP9) stays on the module pads and is unconnected here; the console uses USB only.

## 3. Interfaces

| State | USB interfaces |
| --- | --- |
| Application | DATA CDC (`if00`) and management CDC (`if02`). |
| Debug application | The same plus a console CDC. |
| MCUboot recovery | Recovery CDC (`if00`) and console CDC. |
| ROM BOOTSEL | `RPI-RP2` mass-storage volume for the bootloader UF2. |

The DATA UART uses GP8/GP9. A button jumper from GP12 to GND starts pairing. During a management session the radio is paused and incoming DATA bytes are dropped and counted. See [firmware behaviour](07-firmware.md).

## 4. Flashing

| Image | First time | Afterwards |
| --- | --- | --- |
| Bootloader | hold BOOTSEL and drag its UF2 on, or use SWD | never: only BOOTSEL writes it |
| Application | through the bootloader's serial recovery port | one command, through the application's management port |

```shell
python3 tools/zlrs.py update --serial <serial> \
  --recovery-port /dev/serial/by-id/usb-zlrs_ZLRS_MCUBOOT_<serial>-if00 --no-trigger \
  --image ../build/app/zephyr/zephyr.signed.bin        # first flash
python3 tools/zlrs.py update --serial <serial>
# later updates
```

**The application is never installed as a UF2**: BOOTSEL is the bootloader's door, and everything
with an application in it goes through the bootloader so that signature checks, confirmation and
rollback apply ([07-firmware](07-firmware.md)).

## 5. Wiring and placement

- The Zero feeds 3V3 to the Core2021 and the two share ground.
- Keep the SPI jumpers short; GP16 belongs to the on-board WS2812B, so the radio bus uses
  GP4-GP7 and the control lines GP13-GP15, all at GP15 or below.
- Leave 15-20 mm of clearance on the antenna side: no metal, no wires.
