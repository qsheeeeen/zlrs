# Over-the-air protocol

The zlrs protocol: the frame format, how the link state machine moves, how pairing and sessions
are set up, how the superframe is laid out, how reliable transfer and fragmentation work, and
how hopping and regulation land in code. Identity and keys: [10-security](10-security.md);
timing budget: [07-architecture](07-architecture.md) 4; chip parameters:
[06-radio](06-radio.md).

Protocol version: `ZLRS_PROTO_VERSION` (1 today), carried in `SYNC` and checked by the RX.

## 1. Frame format

Preamble, sync word, length and CRC belong to the LR2021 hardware and are not repeated in the
frame. What the protocol adds is:

| Field | Size | Purpose |
| --- | --- | --- |
| header | 1 B | 6-bit packet type, 2-bit flags |
| `seq` | 2 B | increases within a session; replay protection, reassembly and ARQ use it |
| payload | 0-248 B | type-specific |
| tag | 0 or 4 B | truncated HMAC-SHA256, depending on the security profile |

Multi-byte fields are little-endian. The tag covers the header, `seq` and the payload. The
largest payload is `255 - 3 - 4 = 248` bytes; the data link's usable payload (`data_mtu`) is
derived at boot from the frame format and the rate tier, not from a Kconfig knob.

Flags: `MORE` (more fragments follow) and `ACK_REQ` (acknowledgement requested).

## 2. Packet types

| # | Type | Direction | Purpose |
| --- | --- | --- | --- |
| 0 | `RC` | TX to RX | CRSF channel payload (22 B) |
| 1 | `TLM` | RX to TX | uplink telemetry (8 B, see [tlm.h] layout below) |
| 2 | `DATA_UP` | TX to RX | passthrough data |
| 3 | `DATA_DOWN` | RX to TX | passthrough data |
| 4 | `ACK` | both | cumulative ARQ acknowledgement |
| 5 | `SYNC` | TX to RX | session setup, hop alignment, profile and mix declaration |
| 6 | `PAIR_REQ` | TX to RX | pairing |
| 7 | `PAIR_RESP` | RX to TX | pairing |
| 8 | `PAIR_CFM` | TX to RX | pairing |
| 9 | `SESS_RESP` | RX to TX | session response carrying `nonce_r` |
| 10 | `SESS_CFM` | TX to RX | session confirmation, tagged with the new session key |

The telemetry payload is 8 bytes: LQ percent, uplink RSSI (as negated dBm, CRSF convention),
SNR (0 under FLRC, which cannot measure it), the RX's rate tier id, the RX's power tier id, the
RX link state and a flags word (paired / session).

## 3. Link state machine

### 3.1 States

| State | Meaning |
| --- | --- |
| `UNCONFIGURED` | no root key: the device stays off the air |
| `UNPAIRED` | key present, no peer record |
| `PAIRING` | pairing window open |
| `SEARCHING` | paired, trying to bring a session up |
| `CONNECTED` | session up, traffic flowing |

### 3.2 Transitions

| From | Event | To | Notes |
| --- | --- | --- | --- |
| `UNCONFIGURED` | root key written | `UNPAIRED` | immediate, no reboot |
| `UNPAIRED` | button held 1 s, or the `pair` command | `PAIRING` | 30 s window, `PAIR_REQ` every 100 ms |
| `PAIRING` | pairing succeeds | `SEARCHING` | peer id and both nonces stored, normal sync word adopted |
| `PAIRING` | window expires | `UNPAIRED` | |
| `SEARCHING` | `SYNC` answered with `SESS_RESP`, then `SESS_CFM` verifies | `CONNECTED` | hop index aligned from `SYNC` |
| `CONNECTED` | peer timeout (250 ms on the TX, 100 ms on the RX), or the TX loses its RC input | `SEARCHING` | |
| any | `erase-pair`, `factory-reset` (both next boot) | `UNCONFIGURED` / `UNPAIRED` | |

### 3.3 Pairing

Three messages carry one nonce from each end:

```
PAIR_REQ  {dev_a, dev_b=0, nonce_a, nonce_b=0, flags, [failsafe 22 B]}
PAIR_RESP {dev_a, dev_b, nonce_a, nonce_b, flags}
PAIR_CFM  {dev_a, dev_b, nonce_a, nonce_b, flags}
```

The TX (initiator) sends `PAIR_REQ` and ships its failsafe channel values when it has them; the
RX answers `PAIR_RESP` with its own id and a fresh `nonce_b`, and the TX finishes with
`PAIR_CFM`. Both ends keep `nonce_a || nonce_b` as the pair salt, from which the pairing-level
keys derive ([10-security](10-security.md) 3). Pairing messages are tagged with the shared
`auth_key` and the pairing sync word; once it succeeds both ends switch to their link's normal
sync word and derived keys.

### 3.4 Sessions

1. TX to RX: `SYNC{version, profile, mix, hop, nonce_t}`, authenticated with the pairing-level
   `link_auth`.
2. RX to TX: `SESS_RESP{nonce_t, nonce_r}`, also authenticated with `link_auth`; the RX draws
   `nonce_r` now.
3. TX to RX: `SESS_CFM{nonce_r, hop}` tagged with the **new session key**; the RX adopts the
   session only after it verifies.

Session key: `KDF(pair_key, "zlrs/session", nonce_t || nonce_r || sender id || receiver id)`,
one per direction. Sequence numbers restart at 0 for the session.

**Why the second nonce is mandatory**: with `nonce_t` alone the key would come entirely from a
value the TX puts on air, which anyone can replay; mixing in the RX's fresh `nonce_r` means a
replay only earns a candidate session under a brand new key, and adopting it needs the TX to
confirm with that same key. Retransmits repeat the same response/confirmation, so losing one
message costs a single `SYNC` period.

### 3.5 Sync and resync

- **SEARCHING**: `SYNC` goes out every 100-600 ms depending on the modulation (faster rates
  tolerate a tighter cadence), each one carrying the hop index the sender is on.
- **CONNECTED**: a sparse `SYNC` every 5 s (`ZLRS_APP_RESYNC_PERIOD_MS`, converted to frames)
  wipes out the hop-index error that crystal drift accumulates and spots a peer that changed
  sessions (reboot or re-pair), so the handshake restarts by itself.
- **Replay gate**: a `SYNC` is accepted as a session change only after recent total silence
  (`now - last_rx > beacon period`); an old `SYNC` replayed inside a live session is dropped.
  That same rule is how a peer reboot reconnects by itself.

## 4. Superframe and transfer

### 4.1 Slots

One superframe is four slots, walked by the application in order:

| Slot | Direction | Content |
| --- | --- | --- |
| 0 `CTL_DOWN` | TX to RX | RC frame (the RX side is a receive window) |
| 1 `CTL_UP` | RX to TX | telemetry frame (the TX side is a receive window) |
| 2 `DATA_A` | TX to RX | first half of the data window |
| 3 `DATA_B` | RX to TX | second half of the data window |

The frame rate, per-slot budget and the telemetry/data thinning factors (`tlm_every_n`,
`data_every_n`) come from the control time plan in `core/region`: the plan starts from the
configured rate, clamps it by the region's duty cap and by the airtime the two links need, and
amortises the data window over the frames in between.

### 4.2 Fragmentation

Messages larger than one frame are split with `MORE` and reassembled strictly in `seq` order; a
gap, an oversize message or a 100 ms timeout drops the whole run (the reassembly buffer is
512 B). The receiver is told to void a half-finished run when the sender gives up, so the layer
above never sees a stream at the wrong offset.

### 4.3 Reliable transfer (ARQ)

The data link's default mode:

- a sliding window of **4 frames**, each carrying a 2-byte data sequence number before the
  payload;
- the receiver answers with a **cumulative ACK** (2-byte sequence plus a flags byte whose bit 0
  asks for a resync);
- the sender retransmits the window after 30 ms (`ZLRS_ARQ_TIMEOUT_MS`, raised to twice the
  airtime when a frame is slow) and gives up after 6 retries;
- giving up counts the run as dropped, clears the window, restarts the sequence and tells the
  receiver to void the half message;
- the first frame of a session has to start at sequence 0: dropping a run is better than
  handing the layer above a stream at the wrong offset.

Without ARQ (`CONFIG_ZLRS_APP_DATA_ARQ=n`, debug images) the data link is best-effort: bytes are
dropped and counted when the port rings are full.

## 5. Hopping and regulation

### 5.1 Hop tables and sub-band mixing

- Each link derives a **256-entry hop table** from `fhss_seed`; every block of `channel_count`
  entries holds the sync channel in position 0, and the block is shuffled with the same PRNG on
  both ends, so no table is ever transmitted.
- **Sub-band mixing** adds a second table: each link has a main table and a sub-band table with
  its own index. The mix level is one of four values (0 / 1 / 2 / 4 slots of every four land on
  sub-G = 0 / 25 / 50 / 100 %).
- The TX moves the mix and a 2-bit table version through `SYNC`; the RX adopts both while
  handling that frame, and falls back to the default mix when it misses several versions.
- The migration order is fixed: telemetry first, then data, then the RC downlink; recovery
  moves them back in reverse.

Channel plans:

| Region | Band | Plan |
| --- | --- | --- |
| FCC915 | 902-928 MHz | 50 channels, 500 kHz apart, from 902.5 MHz |
| EU868 | 863-870 MHz | 13 channels, 500 kHz apart, from 863.25 MHz |
| CN470 | 470-510 MHz | 96 channels, 300 kHz apart, from 470.3 MHz |
| CN433 | 433.05-434.79 MHz | 5 channels, 300 kHz apart, from 433.3 MHz |
| ISM2400 | 2400-2483.5 MHz | 79 channels, 1 MHz apart, from 2402 MHz |

### 5.2 Region, power and duty

`core/region` holds the four region tables. A region carries a power ceiling, an optional duty
cap and a maximum rate implied by its band width:

| Region | Power ceiling | Duty | Rate ceiling |
| --- | --- | --- | --- |
| FCC915 | 22 dBm | none | 2.6 Mbps |
| EU868 | 14 dBm | 1 % per 60 s | 0.325 Mbps |
| CN470 | 17 dBm | none | 0.325 Mbps |
| CN433 | 10 dBm | none | 0.325 Mbps |

Validation happens in two places and is deliberately separate: at boot the region is checked
against the board's radio design (band and coverage), the channel plan and the configured
power — a mismatch shows the alarm and keeps the radio off; before every transmission the duty
accounting decides whether there is airtime budget left, and a slot with none left is given up
and counted. The configured rate and the LoRa bandwidth are clamped to the region's limits, and
the mix ceiling for sub-G slots is derived from the duty cap.
