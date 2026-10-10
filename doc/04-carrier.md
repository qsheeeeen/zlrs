# Carrier board

Target shape: fix the wiring between the RP2350-Zero and the Core2021 onto one PCB, exposing
USB-C, a button and one 6P DATA serial port, powered from 5 V. Two layers, with no RF layout
constraints (the module carries its own matching network and antenna connector).

The prototype (two modules wired directly) is in [03-prototype](03-prototype.md); pins and
aliases are defined in the [board README](../boards/zlrs/rp2350_zero_core2021/README.md).

## 1. Ports

| Port | Part | Carries |
| --- | --- | --- |
| J2 (DATA) | JST-GH 1.25 mm, 6P, right-angle | `5V_IN` + bidirectional UART DATA |
| USB-C | on the RP2350-Zero module | DATA, management and flashing |
| SW1 button | SMD tactile 3x4 mm | GP12 to GND: hold to pair |
| Status LED | WS2812 on the module (GP16) | not routed on the PCB |

### 1.1 J2 6P pin order (seen from this board)

The cable is 1:1 with a compatible host UART header: pin 2 is host TX / board RX, pin 3 is host RX / board TX. CTS and RTS are unconnected.

| pin | Host side | This board | Wired to |
| --- | --- | --- | --- |
| 1 (red) | VCC +5V | `5V_IN` | the module's 5 V pin |
| 2 | TX output | RX | UART1_RX = GP9 |
| 3 | RX input | TX | UART1_TX = GP8 |
| 4 | CTS | unconnected | no flow control |
| 5 | RTS | unconnected | no flow control |
| 6 | GND | GND | GND |

J2 `5V_IN` goes through input protection before joining the 5 V rail.

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
| `5V_IN` | J2 pin 1 -> diode -> input protection -> the Zero's 5 V pin |
| `DATA_TX` / `DATA_RX` | J2 pins 3 / 2 to GP8 / GP9 |
| `GND` | J2, both modules, the copper pour |

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
- J2 is right-angle, on the long edge away from the antenna clearance,
  silkscreened `5V/GND` and `TX/RX`
