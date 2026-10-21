# Carrier board

The carrier design mounts an RP2350-Zero and a Core2021 on a two-layer PCB. It
exposes the module's USB-C port, one pairing button and one six-pin DATA UART
connector. The radio module owns the RF matching network and antenna connection;
the carrier must preserve its antenna clearance.

This is a hardware design specification. The wired assembly is described in
[prototype](02-prototype.md), and electrical assignments are listed in the
[board reference](../boards/zlrs/rp2350_zero_core2021/README.md).

## External interfaces

| Interface | Part or location | Function |
| --- | --- | --- |
| J2 DATA | JST-GH 1.25 mm, six pins, right-angle | 5 V input and bidirectional 3.3 V UART |
| USB-C | RP2350-Zero module | Power, DATA, management and installation |
| SW1 | 3 × 4 mm tactile button | GP12 to ground; hold to pair |
| Status LED | Module WS2812B at GP16 | Visible device status |

### J2 pinout

The table is viewed from the carrier. A compatible host cable is wired 1:1:
host TX connects to carrier RX and host RX connects to carrier TX.

| Pin | Host signal | Carrier signal | Connection |
| --- | --- | --- | --- |
| 1, red | +5 V | `5V_IN` | Protected module 5 V input |
| 2 | TX | DATA RX | GP9, UART1 RX |
| 3 | RX | DATA TX | GP8, UART1 TX |
| 4 | CTS | DATA RTS | GP11, UART1 RTS, active low output |
| 5 | RTS | DATA CTS | GP10, UART1 CTS, active low input with pull-up |
| 6 | Ground | Ground | Common ground |

Both UART peers enable RTS/CTS hardware flow control. The carrier deasserts RTS
when its input buffers fill; host RTS controls carrier transmission through CTS.
Earlier assemblies that left J2 pins 4/5 unconnected need these two connections.
USB DATA also supports backpressure. See [firmware](06-firmware.md).

## Module interconnect

| Net | RP2350-Zero | Core2021 |
| --- | --- | --- |
| SPI0 SCK / MOSI / MISO / CS | GP6 / GP7 / GP4 / GP5 | Corresponding SPI pins |
| BUSY / NRESET / DIO8 | GP13 / GP14 / GP15 | Corresponding control pins |
| Supply / ground | 3V3 / GND | 3V3 / GND |

DATA TX connects to J2 pin 3 and DATA RX to J2 pin 2. GP16 belongs to the module
LED and shall not be reused by radio wiring.

## Power

J2 5 V input shall include reverse-feed isolation, a resettable fuse and TVS
protection before joining the Zero's 5 V supply. The supply arrangement shall
prevent USB and external 5 V from backfeeding each other.

Provide 100 µF, 10 µF and 0.1 µF input decoupling, plus 10 µF and 0.1 µF near the
Core2021. The Zero supplies the radio's 3.3 V rail. Use ground pours on both layers
and stitching vias near module pads.

## Mechanical and routing requirements

- Mount the Zero on the top side with components facing outward and Core2021 on
  the bottom side with components facing outward.
- Allow 2–3 mm module overhang at USB-C for connector clearance.
- Place the antenna connector at the opposite short edge and reserve 15–20 mm
  without copper, traces, components or metal, according to the antenna design.
- Provide two 2.2 mm mounting holes for M2 hardware, clear of both footprints.
- Place the button on the bottom and J2 on the long edge away from the antenna.
- Keep SPI traces below 20 mm and out of the antenna clearance. Use at least
  0.5 mm width for the 5 V and 3.3 V supply traces.
- Follow module drawings for castellations, with 0.3 mm pad extension and 0.2 mm
  assembly clearance.
- Mark connector polarity and DATA direction on the silkscreen. Keep the module
  LED visible after assembly.

Acceptance shall verify power-source isolation, logic levels, module orientation,
button input, both antenna paths and operation on the intended LF/HF SKU.
