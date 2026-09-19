# Relationship with ExpressLRS

This is the single source for how zlrs relates to ExpressLRS (ELRS): what it borrows and where it differs. Other documents do not repeat the comparison; they link here.

Every ELRS figure comes from its official documentation and source, linked at the end; the comparison is against v4.1.

## What we borrow

- **Performance targets**: control rate, sensitivity and data throughput are measured against ELRS's published numbers (see [01-product](01-product.md) 3).
- **Reliable transfer**: the data link defaults to acknowledgements plus retransmits (cumulative ACKs, a 4-frame window), the same idea as ELRS's AirPort/MAVLink; the difference is a separate ack frame instead of a single header bit. See [09-protocol](09-protocol.md) 4.3.
- **Hopping and regulation**: per-frame hopping, the sync channel and regional power and duty limits follow the same approach as ELRS; the difference is that our region tables are runtime configuration with an added radio-design consistency check.
- **Periodic sync**: sparse while connected (ELRS uses 5 s) and dense while searching, a cadence copied from ELRS.

## Where we differ

- **Not protocol compatible**: no byte-level compatibility with stock ELRS, no shared binding or hopping parameters and no support for ELRS hardware modules.
- **No ecosystem**: none of the handset or flight-controller ecosystem work: no Lua parameter protocol, Model Match, VTX control, Backpack or BLE gamepad.
- **Different positioning**: ELRS is an RC-link ecosystem (hundreds of hardware targets, WiFi config and OTA, community measurements); zlrs is one link of its own carrying control and data, focused on per-frame authentication, data throughput and regulatory compliance.
- **Verification differs**: ELRS relies on mature CI and community testing; we rely on pure C layering, a two-node host simulation and script self-tests.

## Item by item

| Dimension | ExpressLRS v4.1 | zlrs |
| --- | --- | --- |
| Positioning and shape | an RC-link ecosystem: TX modules, receivers, SPI receivers, Backpack, hundreds of targets | one link: an RP2350-Zero plus Core2021 board in hf and lf SKU variants |
| Bands and modulation | 900M: LoRa 25-250 Hz plus K1000; 2.4G: LoRa, FLRC `F500`/`F1000`, DVDA; dual-band modules can cross bands | control can be FLRC (2.6/1.3/0.65/0.325 Mbps) or LoRa (SF5-12), data is always 2.4G FLRC; one radio time-shared, modulation chosen per link |
| Control rate | 25-1000 Hz | 150 and 500 Hz implemented, 1000 Hz is the target (all 2.4G, `r=0`) |
| Control protocol | CRSF in and out, 16 channels; Lua parameters on the handset | CRSF in and out, 16 channels; no Lua |
| Receiver output | CRSF / SBUS / SUMD / DJI RS2 / HoTT / PWM / SPI | CRSF only (UART or USB CDC) |
| Data link | AirPort (a transparent serial over the air); MAVLink passthrough (hundreds of bps to a few kbps) | a dedicated 2.4G link around 97 kB/s; reliable transfer (ARQ) by default, best-effort optional |
| Telemetry | LINK packets every 512 ms plus DATA packets; MSP, MAVLink, GPS, VTX | an 8 B TLM frame plus the CRSF link statistics frame, fields filled as CRSF requires |
| Security | the binding phrase is explicitly "not a password and not a key, only anti-collision"; no per-frame authentication | root key -> HKDF derivation; every frame carries a 4 B truncated HMAC-SHA256 tag (directional, so reflections fail); both ends contribute nonces to pairing and sessions; sequence numbers stop replays; a P0 profile (no tag) exists for maximum performance |
| Pairing and cross-talk | binding phrase or the classic button; Model Match; Loan Model | the config tool writes a root key, a long press runs the handshake; one device, one peer |
| Dynamic power | yes, driven by telemetry | yes, on by default: judged once a second from telemetry RSSI/LQ, 3 dB steps |
| Configuration | Lua, WebUI, Configurator (compile-time regulatory domain) | a dedicated config CDC with the runtime config protocol (region, power ceiling and more); no WiFi or Lua |
| Updates | WiFi OTA, Betaflight passthrough, DFU, serial | UF2 (BOOTSEL) and SWD; MCUboot variants support serial-recovery DFU with signatures and rollback; no wireless OTA |
| Regulation | compile-time presets | four runtime region tables plus radio-design checks, duty accounting and rate/frame-rate clamping |
| Peripheral integration | Serial VTX, Team Racing, Trainer, Sentinel, Backpack, BLE | none |
| Dual-band use | Gemini dual-transmit diversity; Xrossband dual-band redundancy (**two radios transmitting at once**) | one radio **hops across sub-bands**: the two bands form one link mixed by `r`, neither redundant nor diversity |
| Firmware and roles | separate TX and RX targets | one firmware, the role is a Kconfig option |

## Gaps, in priority order

1. **1000 Hz**: ELRS's `F1000` ships; we are at 500 Hz, and 1000 Hz needs the control link to own the radio, driving frames from IRQs.
2. **Handset-side ecosystem**: Lua, Model Match, VTX control, head tracking, trainer and team racing are all out of scope; CRSF is the way in and out.
3. **LoRa tier verification**: implemented in software, but sensitivity and packet error rate curves are not measured yet.
4. **Wireless OTA**: the board has no WiFi, so it would have to run over the 2.4G data link.
5. **Hardware coverage**: one board exists; the lightweight variant has no hardware yet.

## What is ours alone

- **Per-frame authentication**: ELRS states that its binding phrase is not a key; every one of our frames carries an HMAC tag, session keys and replay protection.
- **The data link is a first-class citizen**: a dedicated 2.4G link with reliable transfer by default, two to three orders of magnitude above AirPort/MAVLink.
- **Regulation in software**: radio-design checks, duty accounting and rate clamping all take effect at runtime.
- **Runtime configuration**: region and power ceiling need no firmware rebuild.

## Sources

- <https://www.expresslrs.org/info/signal-health/>
- <https://www.expresslrs.org/quick-start/binding/>
- <https://www.expresslrs.org/software/airport/>
- <https://www.expresslrs.org/software/mavlink/>
- <https://www.expresslrs.org/info/telem-bandwidth/>
- <https://www.expresslrs.org/software/dynamic-transmit-power/>
- <https://www.expresslrs.org/quick-start/firmware-options/>
- ELRS source (sync interval, `stubbornAck`, `OTA_Sync_s`): <https://github.com/ExpressLRS/ExpressLRS>
