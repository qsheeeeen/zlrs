# Product design

zlrs is a point-to-point radio link: **it carries both telemetry and control**, time-sharing one Semtech LR2021. TX and RX run the same firmware.

## 1. Link shapes

**Two bands make up one link**: the hop table spans both sub-bands and mixes them by slot ratio.

| Sub-band | Character |
| --- | --- |
| 2.4G | the fast sub-band: 2.6 Mbps FLRC, best latency and throughput |
| sub-G | the long-range sub-band: 0.325 Mbps narrow-band FLRC (or LoRa), best distance and penetration |

**Mix ratio `r`** is the share of slots that land on sub-G: four levels, **0 / 25 / 50 / 100%**, chosen by the TX and followed by the RX (see [09-protocol](09-protocol.md) 5.1).

- **Racing**: normally `r = 0` (all 2.4G); as the link degrades, slots move to sub-G in the order **telemetry, then data, then RC**, and in the end the whole link falls back to the long-range tier. There is always a floor.
- **Long range**: normally `r = 100` (all sub-G); closing in or improving quality lowers `r` again and buys the rate back.

The two bands never transmit together: one half-duplex radio sends one frame per slot, and `r` decides which of the two hop tables that frame's frequency comes from.

Two port shapes:

- **Two UARTs**: a control port and a data port; the two links time-share the radio and stay independent, each with its own mix ratio;
- **One UART**: only one link (control or data, picked at build time); the other is not compiled in.

## 2. Targets

**Distance and rate are two ends of one thing**: changing band or rate changes reach, so the targets are written as shape, band, rate and distance together rather than as two independent figures.

| Scenario | Sub-band mix | Rate | Order of distance |
| --- | --- | --- | --- |
| Racing (`r=0`) | all 2.4G | 2.6 Mbps FLRC, 500-1000 Hz | a racing course, hundreds of metres |
| Racing floor (`r=100`) | all sub-G | 0.325 Mbps narrow-band FLRC | **2 km** |
| Long range (`r=100`) | all sub-G | 1.3 Mbps down to 0.325 Mbps FLRC, or LoRa SF7/BW125 | **2 km and beyond** |
| Long range, sped up (`r` lowered) | mostly sub-G plus 2.4G slots | mixed, the 2.4G slots run 2.6 Mbps | close in |

**Control latency stays qualitative**: it has to fly a quadcopter and aim at FPV racing levels. **A 1000 Hz control rate is the target** and should be reachable at `r=0` (all 2.4G).

## 3. Performance figures (benchmarked against ELRS)

Performance is measured against ExpressLRS v4.1's published numbers (details in [02-vs-elrs](02-vs-elrs.md)):

| Figure | ELRS v4.1 | zlrs target |
| --- | --- | --- |
| Highest control rate | 1000 Hz (2.4G `F1000`, 388.8 us on air) | 1000 Hz (`r=0`) |
| 2.4G FLRC sensitivity | -104 dBm at the 1000 Hz tier | -99 dBm at 2.6 Mbps, -108 dBm at 0.325 Mbps, per the LR2021 datasheet |
| sub-G sensitivity | 900M LoRa at 25-250 Hz, -112 to -123 dBm | LoRa SF7 at -118 dBm (sub-G, 64 B, 1% PER) |
| Data throughput | about 445-700 B/s in MAVLink mode, tier dependent | about 97 kB/s on 2.4G FLRC, two to three orders of magnitude higher |
| Reliable transfer | a 1-bit ack (`stubbornAck`) plus resending the current payload, 16 message slots | same idea, on by default |
| Regulation | compile-time regulatory domain presets | four runtime region tables, radio-design checking and duty accounting |

Full chip figures are in [06-radio](06-radio.md).

## 4. Quality requirements

**Stable, deterministic latency, failures that are predictable and detectable, and no degradation over long runs.** No test exists for these on their own, but every design choice has to survive them: a fixed superframe, errors that cannot hide, and failures graded by consequence.

## 5. Who it is for

- Fixed-wing, multirotor, car and boat: one design covers them all
- Serious DIY users who solder their own boards, flash their own firmware and configure it themselves
- Industrial and CNC work is the long-term goal; nothing is designed specifically for it today, but the architecture must not block it

## 6. Boundaries

**The link only** (a module, firmware and reference hardware), never a finished product. Deliberately out of scope:

- no compatibility with stock ExpressLRS (see [02-vs-elrs](02-vs-elrs.md))
- no chasing 20 km links, which belong to cellular
- no video
- no production line and no factory key injection
- no defence against L3 attackers (see [10-security](10-security.md) 7)

Why build it: control, extensibility (off-the-shelf links cannot bend to future projects) and security: per-frame authentication, session keys and replay protection are things stock RC links do not have.
