# Carrier board

Target shape: fix the wiring between the RP2350-Zero and the Core2021 onto one PCB, exposing
USB-C, a button and two 6P serial ports, powered from 5 V. Two layers, with no RF layout
constraints (the module carries its own matching network and antenna connector).

The prototype (two modules wired directly) is in [04-prototype](04-prototype.md); pins and
aliases are defined in the [board README](../boards/zlrs/rp2350_zero_core2021/README.md).

## 1. Ports

| Port | Part | Carries |
| --- | --- | --- |
| J1 (control) | JST-GH 1.25 mm, 6P, right-angle | Pixhawk telemetry port order: `5V_IN` + CRSF |
| J2 (data) | JST-GH 1.25 mm, 6P, right-angle | same order: `5V_IN` + the data port |
| USB-C | on the RP2350-Zero module | control, data, management, flashing; not routed on the PCB |
| SW1 button | SMD tactile 3x4 mm | GP12 to GND: hold to pair |
| Status LED | WS2812 on the module (GP16) | not routed on the PCB |

### 1.1 6P pin order (DS-009 telemetry port, seen from the peripheral)

The cable is 1:1 and **the crossover happens at the peripheral** (the FC's pin 2 is a TX
output, so pin 2 here is an RX input). No hardware flow control: CTS and RTS stay unconnected.

| pin | Flight controller side | This board | Wired to |
| --- | --- | --- | --- |
| 1 (red) | VCC +5V | `5V_IN` | the module's 5 V pin |
| 2 | TX (FC output) | RX | UART0_RX = GP1 (GP9 on J2) |
| 3 | RX (FC input) | TX | UART0_TX = GP0 (GP8 on J2) |
| 4 | CTS | unconnected | no flow control |
| 5 | RTS | unconnected | no flow control |
| 6 | GND | GND | GND |

Each port's `5V_IN` goes through its own Schottky (SS14 / BAT54) before joining the 5 V rail:
either port can power the board and feeding both does not back-feed.

## 2. Nets

Between the modules:

| Signal | Zero | Core2021 |
| --- | --- | --- |
| SPI0 SCK / MOSI / MISO / CS | GP6 / GP7 / GP4 / GP5 | the matching pins |
| BUSY / NRESET / DIO8 | GP13 / GP14 / GP15 | the matching pins |
| Power | 3V3, GND | 3V3, GND (3V3 comes from the Zero) |

To the outside:

| Net | Connection |
| --- | --- |
| `5V_IN` | J1 / J2 pin 1 -> its own diode -> input protection -> the Zero's 5 V pin |
| `CTRL_TX` / `CTRL_RX` | J1 pins 3 / 2 to GP0 / GP1 |
| `DATA_TX` / `DATA_RX` | J2 pins 3 / 2 to GP8 / GP9 |
| `GND` | J1, J2, both modules, the copper pour |

## 3. Power

- `5V_IN` -> resettable fuse + TVS -> 100 uF + 10 uF + 0.1 uF -> the Zero's 5 V pin
- Add 10 uF + 0.1 uF next to the Core2021
- Pour GND on both sides and add vias near the module pads

## 4. Mechanics and assembly

- **Parts on both sides**: top = RP2350-Zero (components facing up), bottom = Core2021 (facing
  down)
- Let the module overhang the carrier by 2-3 mm on the USB-C side so cables fit; the antenna
  connector sits on the opposite short edge, which needs 15-20 mm of clearance (no copper, no
  traces, no parts)
- Two mounting holes, 2.2 mm for M2, on opposite corners, clear of the modules' footprint
- The button goes on the bottom (the top is mostly covered by the Zero)
- Keep SPI traces short (under 20 mm) and out of the antenna clearance; 5 V and 3V3 traces at
  least 0.5 mm wide
- Castellation pads follow the module drawing (0.3 mm extension, 0.2 mm assembly margin all
  round)
- J1 and J2 are right-angle, on the same long edge away from the antenna clearance,
  silkscreened `5V/GND` and `TX/RX`
