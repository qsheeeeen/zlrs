# Over-the-air protocol

This document defines protocol 10, link behavior and the management status contract.
The detailed air-field layout and data model are specified in [air-frame-design](air-frame-design.md).
Peers must select the same version, band and profile; other air versions are rejected.

## Air frame and DATA

Every frame has type/config(1 B), a common little-endian sequence(4 B), a typed body
and an optional authentication tag(4 B in P1, absent in P0). Eight type values 0–7
occupy the first byte's low three bits. SYNC uses its remaining bits for profile:1
and version:4; other types require those bits to be zero. ACK belongs only to DATA.

The frame model owns inline DATA and tag arrays. Bodies use a typed union, with
shared members only for identical field layouts. There is no on-air DATA length
or stop offset; received radio length determines used bytes. P1 permits 499 user
bytes and P0 permits 503, with a 511 B maximum radio payload.

DATA_UP and DATA_DOWN carry ack_seq(1 B), RSSI magnitude(1 B), LQ(1 B) and user bytes.
ack_seq is ACK in bit0 and DATA sequence in bits1–7. Empty feedback uses the same
layout: 12 B in P1 or 8 B in P0. ARQ-off operation preserves this format.

Each direction retains one outstanding DATA packet. DATA sequence wraps modulo128
and stays unchanged on retries; common frame sequence advances on every attempt.
ACK is the parity of the next expected DATA sequence and advances only after the
sink accepts the entire payload. Backpressure retains the packet; duplicate DATA
is not delivered twice.

RSSI magnitude zero means unavailable; other values represent negative dBm. LQ
uses exact integer percentages 0–100. Both describe this endpoint's reception of
its peer and feed the peer's power policy.

## Pairing, sessions and sequence checks

PAIR_REQ carries the initiator ID and nonce. PAIR_RESP and PAIR_CFM bind both IDs
and both nonces. Pairing requires an explicitly opened, bounded window; repeated
confirmation must not reinstall keys or reset stream progress.

The session exchange is SYNC, SESS_RESP, SESS_CFM in both P0 and P1. SYNC offers
nonce_t and the TX schedule; SESS_RESP returns nonce_t/nonce_r; SESS_CFM proves the
candidate session key and supplies the current hop and TDD frame index. RX adopts
the session only after that proof. Different endpoint IDs distinguish directional keys.

All eight types carry one common u32 frame sequence. Each direction shares one
counter across a pairing exchange or a session exchange; handshake-to-CONNECTED
does not reset it. Zero is reserved for an absent receive baseline, never transmitted.
Counters must not wrap; renewal requires a fresh nonce context.

Sequence screens old packets within a selected receive context. Identities, nonces
and pending state further bind handshake messages. P1 authentication must pass
before counters, ACK, synchronization or delivered bytes change. A forged high
sequence or an old response cannot directly replace an active session.

## Quality and synchronization

LQ counts accepted DATA, same-session SYNC and valid SESS_CFM using sequence gaps
in windows of at least 128 expected attempts. Pairing and pre-confirmation response
traffic must not enter an established session's denominator. The first accepted
session sample establishes the baseline; tracking remains continuous across
quality windows.128/128 gives 100%, 127/128 gives 99%.

frame_index counts shared TDD periods, including idle periods. It is separate from
per-direction frame sequence and occurs only in SYNC/SESS_CFM. hop_index is u8
and must be within the region's generated table; invalid indices must not be
normalized by modulo. RSSI and LQ remain separate bytes.

## Slots, hopping and regulation

Each frame contains two slots: DATA A (0, TX → RX) and DATA B (1, RX → TX). The LR2021 uses hardware sync word 1 for DATA and 2 for pairing. Pairing uses its own sync slot; connected traffic uses the DATA sync slot. A selected frequency band supplies its channel table and modem timing. FHSS derives a deterministic hop sequence from pairing material. The sub-G band uses its board SKU and region's channel, TX-power and duty rules; when the duty budget is exhausted, transmission is deferred. The active TX power never exceeds the configured/regional ceiling. DATA uses one selected band at a time.

Slot boundaries remain fixed while packets use variable lengths. DATA carries the queued bytes available for that packet, up to 499 bytes in P1 or 503 bytes in P0, without waiting to fill a maximum-sized frame or transmitting padding. For example, 20 user bytes produce a 32-byte P1 radio payload; feedback alone uses 12 bytes. Short packets reduce airtime and the airtime charged to the duty budget, but do not advance the next slot or increase the fixed packet rate. The LR2021's received length is sufficient to locate the end of DATA; no additional stop-offset field is sent.

The default 2.4 GHz preparation reservation is 2.5 ms. It includes the previous RX window's cleanup, authentication, radio configuration and FIFO loading before the timer's fixed TX launch. The frame period is derived from this reservation and the maximum packet airtime, so both peers must use the same timing configuration. Increasing this reservation trades nominal slot rate for fewer aborted preparations; it does not allow packets to transmit late.

FLRC uses a 24-bit preamble, a 32-bit hardware sync word and a 16-bit physical CRC with coding rate 3/4. DATA matches the full sync word derived from pairing material; pairing uses `ZLRS`. Full sync matching limits noise packets reaching the CRC decoder. Packet airtime and the slot plan are computed from these same modem settings.

Periodic SYNC defaults to 5 seconds, converted to a whole number of actual frames. With the default 2.4 GHz timing this is 450 frames (4.991 seconds). The frame count is 16 bits and includes debug pacing when deriving the interval. Each DATA slot advances the hop index even if idle or skipped. The 2.4 GHz table contains three 79-channel blocks (237 entries); CN433 contains 51 five-channel blocks (255 entries). Each block visits every channel once and reserves its first position for the synchronization channel; the other positions are shuffled from the pairing seed.

RX follows authenticated packet timestamps. When receptions stop, it scans the frame phase with a step derived from the actual frame length and SYNC interval: approximately one third of a frame between SYNC opportunities. The default steps are 8 us per frame on 2.4 GHz and 155 us on CN433. This lets a searching receiver sample different phases of a quiet peer's periodic SYNC. After acquisition, scanning waits through two SYNC intervals without an authenticated reception.

## Management status

Management version `4` uses a fixed 90-byte `STATUS_RESP`, filling one 96-byte management frame. Multibyte integers are little endian; RSSI and temperature are signed 16-bit values, and power values are signed 8-bit dBm. The firmware and host use this one status layout.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Management version, role, state, flags |
| 4 | 8 | Device ID, peer ID |
| 12 | 4 | Local LQ, sub-G hardware SKU, region, configured TX ceiling |
| 16 | 4 | Saved DATA UART baud |
| 20 | 1 | Active security profile |
| 21 | 4 | Local RSSI, peer RSSI; zero means unavailable |
| 25 | 3 | Peer LQ, active TX setpoint, effective power floor |
| 28 | 2 | MCU die temperature in 0.01 °C; `INT16_MIN` means unavailable |
| 30 | 32 | Eight `u32` counters: TX frames, RX frames, CRC errors, authentication errors, ARQ retries, dropped DATA bytes, missed slots, radio failures |
| 62 | 4 | FHSS snapshot frequency in Hz |
| 66 | 2 | FHSS index |
| 68 | 4 | Frame period in microseconds |
| 72 | 2 | LR2021 junction temperature in 0.01 °C |
| 74 | 2 | LR2021 temperature near XOSC in 0.01 °C |
| 76 | 2 | LR2021 VBAT supply in millivolts |
| 78 | 2 | LR2021 version major, minor |
| 80 | 3 | Chip mode, last command status, reset source |
| 83 | 1 | Validity bits: bit0 junction temperature, bit1 XOSC temperature, bit2 VBAT, bit3 version, bit4 mode/command/reset/IRQ, bit5 chip errors |
| 84 | 2 | LR2021 latched chip error mask |
| 86 | 4 | LR2021 IRQ mask at the maintenance boundary |

The frequency is the hopping schedule's current channel; periodic SYNC uses the fixed synchronization channel. MCU temperature is the internal die sensor reading, not ambient or LR2021 temperature. FLRC does not supply a LoRa SNR measurement.

LR2021 measurements run only on the RF owner at boot or during a paused management session. The mode and IRQ are captured after RF has stopped at the maintenance boundary, so standby XOSC is expected; these are not the preceding live RX/TX state. Sensor reads use the chip's unit format at 13-bit ADC resolution; temperatures are signed Q8.5 converted to hundredths of a degree. Validity bits distinguish unavailable values from valid zero readings. Reads do not clear the chip's error/IRQ/reset information. The debug shell reads the same cached observation via the RF zbus channel and prints its age; it does not issue SPI or pause RF to refresh it. Internal sensor readings are not external ambient temperature or calibrated battery-pack voltage. See [LR2021 datasheet Rev. 1.1, sections 6.5 and 6.7](https://www.mouser.com/pdfDocs/61979758LR2021_V1_1_datasheet.pdf).
