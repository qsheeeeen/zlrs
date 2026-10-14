# RP2350-Zero and Core2021 board reference

This board combines an RP2350-Zero (RP2350A Cortex-M33, 4 MiB flash) and a Core2021
(Semtech LR2021). The board contract defines pins, endpoint aliases, RF coverage,
calibration and storage. Assembly is specified in
[prototype](../../../doc/02-prototype.md); builds and installation in
[firmware](../../../doc/06-firmware.md).

## Pins and endpoints

| Signal | Pin | Alias / node | Electrical requirement |
| --- | --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | `lora-transceiver`, `lora_core2021` | Radio device permits 16 MHz SPI |
| SPI0 chip select | GP5 | Radio `cs-gpios` | GPIO, active low |
| BUSY | GP13 | Radio `busy-gpios` | Active high, pull-up |
| NRESET | GP14 | Radio `reset-gpios` | Active low |
| DIO8 IRQ | GP15 | Radio DIO8 | Active high, pull-down |
| UART1 TX / RX | GP8 / GP9 | `zlrs-data-uart` | 3.3 V logic; default 115200 baud |
| Pairing button | GP12 | `zlrs-button`, `smtc-user-button` | Active low to ground, pull-up |
| WS2812B | GP16 | `zlrs-led`, `led-strip` | One pixel, PIO0, GRB order, 800 kHz |
| Slot timer | — | `zlrs-slot-timer`, TIMER1 | Dedicated microsecond radio clock |
| Watchdog | — | `watchdog0` | Application progress timeout |

External UART wiring crosses TX/RX and shares ground. GP16 is reserved for the
module LED. Radio wires use GP4–GP7 and GP13–GP15.

| USB endpoint | Alias | Application mode |
| --- | --- | --- |
| DATA CDC, `if00` | `zlrs-data-cdc` | Production and debug |
| Management CDC, `if02` | `zlrs-mgmt-cdc` | Production and debug |
| Console CDC | Console/shell endpoint | Debug only |

MCUboot exposes recovery CDC at `if00` and a console. The firmware selects one
active DATA endpoint when UART and USB coexist; see
[DATA endpoints](../../../doc/06-firmware.md#data-endpoints).

## SKU and RF coverage

| Build target | SubG design | Regions | Default |
| --- | --- | --- | --- |
| `rp2350_zero_core2021/rp2350a/m33/lf` | 410–510 MHz | CN470, CN433 | CN433 |
| `rp2350_zero_core2021/rp2350a/m33/hf` | 850–930 MHz | FCC915, EU868 | FCC915 |

Both targets support the same 2.4 GHz path and use sysbuild with MCUboot.
Cortex-M33 is supported; Hazard3 is not a build target for this board.
The RF design shall declare SKU, coverage and default region. Regional edges,
channel centers and configured power must fit that design before transmission.

Core2021 uses a crystal, DC/DC mode, internal low-frequency RC, zero TX-power
offset and receiver boost 7. Crystal operation has no TCXO wake-up reservation.
The allowed setpoint range is −17 to +12 dBm on 2.4 GHz and −10 to +22 dBm
on SubG. Regional policy further bounds the SubG maximum. Each SKU uses its
own SubG PA tables.

| Calibration property | HF target | LF target |
| --- | --- | --- |
| SubG PA/current references | 902 MHz reference tables | 470 MHz reference tables |
| Calibration frequencies | 897.5 MHz, 2441 MHz, unused zero | 433.3 MHz, 470.3 MHz, 2441 MHz |
| Calibration slots | Exactly three | Exactly three |

Both targets share the 2.4 GHz PA/current references. Reference tables require
output-power, current and receive-quality calibration on the actual module and
carrier. See [radio](../../../doc/04-radio.md) and
[qualification](../../../doc/11-roadmap.md).

## Peripheral and timing requirements

| Peripheral | Transfer contract |
| --- | --- |
| SPI0 / LR2021 | Synchronous polling commands and FIFO transfers |
| PIO0 / LED | Direct one-pixel output without DMA |
| UART1 | Interrupt-driven FIFO I/O |
| USB | DPRAM and interrupt/work-queue service |
| Flash | ROM programming/erase, with RF paused for ordinary writes |
| TRNG | Fresh health-checked blocks, bounded waits, interrupts enabled |
| SHA-256 | Serialized hardware operation, multipart input, no DMA |
| TIMER1 / GPIO | Fixed launch and hardware completion timestamps |
| MCU temperature | Internal ADC die sensor; no external signal pin |

System DMA is disabled in the application; declaring available channels does not
allocate them. RF owns ordinary SPI preparation. TIMER1 issues prepared SET_TX
at the fixed boundary. Preparation is 2.5 ms on 2.4 GHz and 6 ms on SubG, with a
200 µs launch guard. Both peers require matching timing. Critical radio and timer
paths run from SRAM.

The entropy provider is the RP2350 hardware TRNG with health checks and bounded
thread-context collection. Identity and handshake generation fail when entropy
cannot be supplied. Peripheral presence and provider choice belong to the board
contract; failure cannot silently substitute a timer or test generator.

## Flash partitions

Both application and MCUboot share this 4 MiB flash layout:

| Partition | Offset | Size | Purpose |
| --- | --- | --- | --- |
| MCUboot | `0x000000` | 128 KiB | Signature verification and USB serial recovery |
| image-0 | `0x020000` | 832 KiB | Signed primary application |
| image-1 | `0x0F0000` | 832 KiB | Inactive upload and swap slot |
| log | `0x1C0000` | 1 MiB | LittleFS rotating logs |
| coredump | `0x2C0000` | 128 KiB | Fault context |
| storage | `0x2E0000` | 32 KiB | NVS settings, identity, root key and pairing |

Application and bootloader must agree on every partition. The application links
into its code partition and must fail to build if it exceeds that allocation.
Flash reads bypass stale XIP cache contents after programming.

The boot chain is ROM → MCUboot → signed application. Swap-using-move supports
unconfirmed test upgrades and rollback without a scratch partition. A four-byte
retained boot-mode flag provides explicit serial-recovery entry. BOOTSEL installs
the MCUboot UF2; applications use signed serial recovery. The application does
not produce an installation UF2. See
[updates and rollback](../../../doc/06-firmware.md#installation-updates-and-rollback).

## Image defaults

Production enables USB DATA/management, interrupt-driven UART, GPIO, flash,
watchdog, secure entropy and hardware hashing. Console is absent; debug enables
its CDC. USB initialization must register every enabled CDC independently.
Application image management handles confirmation, while MCUboot handles recovery
and swap. The bootloader produces UF2 and the application produces a signed binary.

Board acceptance requires LF/HF builds, USB interfaces, power/region validation,
radio timing, entropy failure/recovery, SRAM placement and image rollback.
