# Product design

zlrs is a point-to-point radio link. It carries **control** (CRSF channels down, telemetry
up) and **data** (a byte stream) at the same time, over one half-duplex Semtech LR2021. TX and
RX run the same firmware; which end a device is, is configuration.

## 1. The two links

| Link | Carries | Optimised for |
| --- | --- | --- |
| Control | CRSF: 16 channels down, telemetry up, pairing | distance and stability |
| Data | a passthrough byte stream | rate and throughput |

The links are independent state machines sharing one radio: losing one does not take the other
down, and each has its own hop table and its own sub-band mix. They are not redundant radios —
a supply or radio failure takes both.

## 2. Bands and shapes

Both bands form one link per direction of travel: the hop table spans 2.4G and sub-G, and the
mix ratio `r` (0 / 25 / 50 / 100 %) decides how many slots land on sub-G. The TX decides `r`
and carries it in `SYNC`; the RX follows. The migration order is telemetry, then data, then the
RC downlink, so latency steps down instead of jittering.

Two band roles:

- **Long range** (default): control owns sub-G, data owns 2.4G. Sub-G is what carries the link
  far; 2.4G gives the data link its throughput.
- **Racing**: control owns 2.4G alone — short airtime, high frame rate — and the data link
  moves to sub-G under the regional rules (or is switched off).

Two port shapes:

- **Two UARTs**: a control port and a data port; the two links time-share the radio and stay
  independent, each with its own mix ratio;
- **One UART**: control or data, whichever the configuration says.

## 3. Targets

**Distance and rate are two ends of one thing**: changing band or rate changes reach, so the
targets are written as shape, band, rate and distance together rather than as two independent
figures.

| Scenario | Sub-band mix | Rate | Order of distance |
| --- | --- | --- | --- |
| Racing (`r=0`) | all 2.4G | 2.6 Mbps FLRC, 500-1000 Hz | a racing course, hundreds of metres |
| Racing floor (`r=100`) | all sub-G | 0.325 Mbps narrow-band FLRC | **2 km** |
| Long range (`r=100`) | all sub-G | 1.3 Mbps down to 0.325 Mbps FLRC, or LoRa SF7/BW125 | **2 km and beyond** |
| Long range, sped up (`r` lowered) | mostly sub-G plus 2.4G slots | mixed, the 2.4G slots run 2.6 Mbps | close in |

**Control latency stays qualitative**: it has to fly a quadcopter and aim at FPV racing levels.
**A 1000 Hz control rate is the target** and should be reachable at `r=0` (all 2.4G).

## 4. Performance figures (benchmarked against ELRS)

Measured against ExpressLRS v4.1's published numbers; the comparison itself is in
[02-vs-elrs](02-vs-elrs.md).

| Figure | ELRS v4.1 | zlrs target |
| --- | --- | --- |
| Highest control rate | 1000 Hz (2.4G `F1000`, 388.8 us on air) | 1000 Hz (`r=0`) |
| 2.4G FLRC sensitivity | -104 dBm at the 1000 Hz tier | -99 dBm at 2.6 Mbps, -108 dBm at 0.325 Mbps, per the LR2021 datasheet |
| sub-G sensitivity | 900M LoRa at 25-250 Hz, -112 to -123 dBm | LoRa SF7 at -118 dBm (sub-G, 64 B, 1% PER) |
| Data throughput | about 445-700 B/s in MAVLink mode, tier dependent | about 97 kB/s on 2.4G FLRC, two to three orders of magnitude higher |
| Reliable transfer | a 1-bit ack (`stubbornAck`) plus resending the current payload, 16 message slots | same idea, on by default |
| Regulation | compile-time regulatory domain presets | four runtime region tables, radio-design checking and duty accounting |

Full chip figures are in [06-radio](06-radio.md).

## 5. Quality requirements

**Stable, deterministic latency, failures that are predictable and detectable, and no
degradation over long runs.** Every design choice has to survive them: a fixed superframe,
errors that cannot hide, failures graded by consequence (a regulatory problem keeps the radio
off but leaves the device configurable and flashable).

## 6. Who it is for

- Fixed-wing, multirotor, car and boat: one design covers them all;
- anything that wants a serial link and a control link over the same small module;
- builders who want to see the numbers and change the behaviour without a compiler.

What it deliberately is not: no Lua scripting, no WiFi, no OTA, no handset ecosystem and no
byte-compatibility with another protocol. See [02-vs-elrs](02-vs-elrs.md).
