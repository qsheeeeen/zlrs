# Prototype

The prototype connects a Waveshare RP2350-Zero to a Waveshare Core2021. USB-C
provides power, DATA, management and firmware installation. The DATA UART remains
accessible on module pads. A jumper from GP12 to ground substitutes for a fitted
pairing button.

## Components and capabilities

| Component | Requirement |
| --- | --- |
| MCU module | RP2350-Zero: RP2350A Cortex-M33, 4 MiB QSPI flash, native USB-C |
| Radio module | Core2021 with Semtech LR2021; LF or HF SubG SKU |
| SubG antenna | Match the module SKU and selected regional band |
| 2.4 GHz antenna path | Provide the module's specified antenna connection and clearance |
| Power | USB-C to the Zero; the Zero supplies 3.3 V to the Core2021 |
| Pairing input | GP12 to ground, internally pulled up |
| Status output | On-board WS2812B on GP16 |

A single LR2021 handles SubG and 2.4 GHz sequentially. Automatic switching requires
usable antenna paths for both bands; a SubG antenna alone does not establish
2.4 GHz performance. Both endpoints must select compatible SubG regions.

| Module SKU | Declared SubG coverage | Supported regions | Default region |
| --- | --- | --- | --- |
| LF | 410–510 MHz | CN470, CN433 | CN433 |
| HF | 850–930 MHz | FCC915, EU868 | FCC915 |

The platform provides SPI, GPIO IRQs, a dedicated microsecond slot timer, UART,
USB, flash storage, watchdog, hardware SHA-256, hardware entropy and MCU die
temperature. Cortex-M33 is the supported core. Radio SPI uses synchronous polling;
radio transfers and the one-pixel PIO LED do not allocate DMA channels.

## Wiring

| Signal | RP2350-Zero pin | Core2021 connection |
| --- | --- | --- |
| SPI0 SCK | GP6 | SCK |
| SPI0 MOSI | GP7 | MOSI |
| SPI0 MISO | GP4 | MISO |
| SPI0 chip select | GP5 | NSS, active low |
| BUSY | GP13 | BUSY, active high |
| Reset | GP14 | NRESET, active low |
| Radio IRQ | GP15 | DIO8, active high |
| Supply | 3V3 | 3V3 |
| Ground | GND | GND |

The radio SPI device permits a 16 MHz clock. BUSY has a pull-up and DIO8 has a
pull-down. GP16 is reserved for the on-board LED. The DATA UART uses GP8 for TX and
GP9 for RX, GP10 for CTS and GP11 for RTS at 3.3 V logic levels. External UART
wiring crosses TX/RX and RTS/CTS and shares ground; both peers enable hardware
flow control. An unconnected CTS is pulled high and stops UART transmission.

## USB interfaces

| Firmware mode | Interfaces |
| --- | --- |
| Production application | DATA CDC at `if00`, management CDC at `if02` |
| Debug application | DATA, management and an additional console CDC |
| MCUboot recovery | Recovery CDC at `if00` and a console CDC |
| ROM BOOTSEL | `RPI-RP2` mass-storage volume for the bootloader UF2 |

DATA USB and UART are alternative endpoints of the same byte stream. Management
uses its dedicated CDC. While management pauses RF, accepted DATA stays queued;
USB and the RTS/CTS UART apply backpressure when buffers fill. A UART peer that
ignores RTS can still overrun the hardware FIFO.
Endpoint selection is specified in [firmware operation](06-firmware.md#data-endpoints).

## Assembly requirements

Keep SPI jumpers short and route them away from antenna connections. Share ground
between both modules and external controllers. Reserve 15–20 mm on the antenna
side without metal or wiring, subject to the module's antenna drawing.

Hardware descriptions shall specify pin assignments, endpoint availability,
radio coverage, default region, calibration and recovery support. Region and
power validation must use those hardware constraints. Hardware acceptance checks
power, logic levels, wiring, both antenna paths and the intended module SKU.

The [board reference](../boards/zlrs/rp2350_zero_core2021/README.md) owns pin aliases,
calibration details and flash partitions. [Firmware](06-firmware.md#build-environment)
owns build targets, tools, installation and updates. A PCB mounting arrangement is
specified in [carrier](03-carrier.md), and button/LED behavior in
[user interface](09-ux.md).
