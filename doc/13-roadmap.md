# Roadmap

This document holds **to-dos and acceptance criteria** only. Finished work gets a one-line summary, details live in code comments and the design is in the design documents.

## To-do

### Radio and protocol

- **First frames in the air, one board (passed)**: with the Core2021 attached, pairing `PAIR_REQ` frames really go out: one every 100 ms through the 30 s window, about 1700 transactions over five rounds with `fail=0`. The CRSF entry was checked on the same board: 911 valid RC frames fed into the CRSF CDC gave `crsf_bad=0`, then 550 frames with the wrong payload length took `crsf_bad` up by exactly 550. This covers SPI and the control lines, RAC transactions and CRSF parsing; receive is not part of it.
- **Pairing, sessions and round trips on both links**: needs a second module (LF or HF SKU, both ends in the same region). The pairing handshake, session handshake, telemetry, data and ARQ only mean something with two boards present.
- **Sub-band mixing on hardware**: the software side is done (two hop tables, `SYNC` carrying `r` and the table version, the telemetry-then-data-then-RC migration order, the regional duty cap and LQ-driven adaptation) and the host tests cover both ends picking the same frequency and falling back; the real switching cost and interference benefit wait for a second module.
- **1000 Hz control rate** (`r=0`, all 2.4G): the control link has to own the radio, bypassing RAC's millisecond planner and driving the LR20xx driver plus IRQs frame by frame. The protocol side (1 ms frames, slot budgets, thinning factors) is ready.
- **LoRa long-range tier on hardware**: sensitivity, packet error rate curves and the gap between measured airtime and the model. EU868's 1% duty cannot feed this tier, so it is offered only in regions without a duty cap.
- **Reliable transfer (ARQ) on hardware**: on by default (a 4-frame window, cumulative ACKs, retransmits); throughput and latency need measuring on the board. Later the ack can shrink to ELRS's 1 header bit once the RX always has a frame to ride along.
- **Over-the-air firmware update** (optional): images travel over the 2.4G data link, roughly 10 s per MB.

### Hardware

- **Carrier board spin**: the two-layer board from [05-carrier](05-carrier.md).
- **Lightweight variant**: USB and the data port trimmed, control link only.
- **RP2350 TRNG** (verified on hardware, on by default): Zephyr's `entropy_rpi_pico` is unusable (pico-sdk reads with interrupts off and busy waits, which stops USB dead; reproduced on the board). `drivers/entropy/` is zlrs's own driver: probe soft-resets, configures and pre-fetches, reads come from a two-block pool, **interrupts stay on and no spinlock is taken**, and a timeout or failed check is an error rather than something to paper over. Register timing follows datasheet 12.12 and the bootrom; the sample count is 100 because the datasheet's suggested 20-25 fails the internal Von Neumann check on RP2350 (measured: runs of 32 zeroes or ones), while ten blocks at 100 came out clean. Verified on the board: `entropy self-test ok` at boot, 25 consecutive `zlrs entropy` runs and over 150 s of uptime without a watchdog reset.

### Update path

- **MCUboot slot swap plus host confirmation** (verified on hardware, see [08-firmware](08-firmware.md) 6): all four paths pass: the first image needs no confirmation, an upgrade plus `confirm` stays, no confirmation means a reset and rollback after 20/60 s, and a hung application is rolled back by the bootloader's 30 s watchdog. Two details: the swap has to use `swap using move` (`swap using offset` goes in but never comes back on this board) and the tool has to name the inactive slot, since MCUboot's recovery upload defaults to the primary one.
- **mcumgr in the application** (optional): update while running, writing the image straight into slot1.

### Security

- **P2 encryption profile**: implement once everything else is done (AES-CTR with the MAC covering the ciphertext).

## Pending defaults

Some knobs are on or off today only because they have not been accepted on hardware yet — "what the system will do" is already decided, "proven on a board" is not. Each row is that debt: the knob, its default today, what has to be measured, and what happens afterwards. This is the only place where "not tested yet" is written down; the design documents describe the final shape and [12-debug](12-debug.md) owns the bench switches themselves.

| Knob | Default today | What is missing | Afterwards |
| --- | --- | --- | --- |
| `ZLRS_APP_RATE_ADAPT` | off | per-tier packet error rate and SNR thresholds, measured with a second module | on, thresholds in the tier table |
| `ZLRS_APP_WATCHDOG` | off | a long run on hardware without false resets (PBKDF2 inside config mode is the slowest feed interval) | on |
| `ZLRS_APP_DATA_ARQ` | on | throughput and latency on hardware; the ack can shrink to one header bit once the RX always has a frame to ride along | stays on, window sized from measurements |
| `ZLRS_APP_DYNAMIC_POWER` | on | thresholds and hysteresis from measured RSSI/SNR | stays on, thresholds replaced |
| `ZLRS_APP_SUB_BAND_MIX` | on | switching cost and interference benefit, with two modules | stays on |
| `ZLRS_APP_FHSS`, `ZLRS_APP_SESSION_HANDSHAKE`, `ZLRS_APP_TELEMETRY` | on | the first two-module session (pairing, session, telemetry round trip) | stay on |
| role TX/RX and the security profile | compile time, P1 | `set-role` / `set-profile` in the config protocol ([organization](organization.md) 4) | runtime, stored in NVS |

The switches that exist to isolate a layer (`…=n` forms, P0, one link at a time) are *not* debt either: they cut the product's shape and stay, see [12-debug](12-debug.md) 3. The ones that only make sense on the bench — a build whose radio sits out, for a board with no module attached — are gated by `CONFIG_ZLRS_APP_DEBUG` and cannot be reached from a shipped configuration.

## Acceptance criteria

| Item | What counts as passing |
| --- | --- |
| Hardware freeze | the module's RSSI for a known transmitter on the target band matches the datasheet's order of magnitude |
| Pairing and sessions | two devices pair on a long press, establish a session and run both the control and data links |
| Configuration | a blank RX can be configured with the tool (config CDC) until it is able to pair |
| Data link | short the RX's UART TX to RX as a loopback, send data from the TX side and get it back |
| 1000 Hz | steady 1000 Hz in the all-2.4G shape, with latency and loss that can be measured |
| Interference | link retention and recovery time measured with several devices on the band plus interferers |
