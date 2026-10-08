# Over-the-air protocol

The ZLRS protocol: the frame format, how the link state machine moves, how pairing and sessions
are set up, how the superframe is laid out, how reliable transfer works, and
how hopping and regulation land in code. Identity and keys: [09-security](09-security.md);
timing budget: [06-architecture](06-architecture.md) 4; chip parameters:
[05-radio](05-radio.md).

Protocol version: `ZLRS_PROTO_VERSION` (5 today), carried in `SYNC` and checked by the RX.

## 1. Frame format

Preamble, sync word, length and CRC belong to the LR2021 hardware and are not repeated in the
frame. What the protocol adds is:

| Field | Size | Purpose |
| --- | --- | --- |
| `header` | 1 B | 6-bit packet type tells the receiver how to parse the frame; bit 1 is the reverse DATA ACK toggle and bit 0 is reserved. |
| `seq` | 2 B | Per-transmission frame number. The receiver uses it to reject replayed frames. |
| `payload` | 0-504 B | Packet-specific content. ARQ DATA starts with a 2-byte data sequence, followed by user bytes; an empty DATA reply has no payload. |
| `tag` | 0 or 4 B | Truncated HMAC-SHA256. Authenticates the ZLRS frame so modified or forged fields are rejected; omitted only in the debug P0 profile. |

Multi-byte fields are little-endian. The tag covers the header, `seq` and payload. The LR2021
adds its own physical-layer framing around these bytes; those fields are listed below.

FLRC accepts up to 511 bytes for the entire ZLRS frame. With P1 authentication, ARQ carries
502 user bytes, including when reporting the reverse ACK bit. LoRa retains its 255-byte limit and
the link clamps its MTU separately. Both peers must run protocol version 5. The public LR20xx
RAL already configures separate TX/RX FIFOs large enough to hold a full FLRC frame; no FIFO
streaming or upstream changes are required. See the [LR2021 datasheet, sections 5.3 and 18.2](https://www.mouser.com/pdfDocs/61979758LR2021_V1_1_datasheet.pdf).

### 1.1 LR2021 FLRC fields

The current data link uses FLRC variable-length packets, a 24-bit AGC preamble, a 21-bit timing
recovery preamble, a 16-bit Sync Word, a 16-bit variable-length header and a 16-bit hardware CRC.
With CR 3/4, FLRC also applies convolutional coding and transmits its 6-bit tail. The modem creates
these fields; they are not bytes in the ZLRS radio buffer.

| Field | Size | Purpose |
| --- | --- | --- |
| AGC preamble | 24 bits | Gives the receiver time to settle automatic gain control before decoding. |
| Timing recovery preamble | 21 bits | Lets the demodulator recover the bit clock and align sampling to the incoming bit stream. |
| Sync Word | 16 bits | Marks the packet for the configured link/slot so the receiver can reject unrelated signals before reading the frame. |
| Variable-length header | 16 bits | Carries packet length (9 bits); the other 7 bits are reserved by LR2021. |
| ZLRS frame | variable | The application protocol fields in the table above. |
| Hardware CRC | 16 bits | Detects radio packet corruption before ZLRS processes the frame. |
| FEC tail | 6 bits | Terminates the CR 3/4 convolutional encoder state; it is not application data. |

The current 16-bit Sync Word is configured in `app/src/platform/radio.c`. LR2021 permits 16- or
32-bit Sync Words; the shorter one saves 16 uncoded bits per packet while increasing the chance
of an unrelated signal matching.

## 2. Packet types

| # | Type | Direction | Purpose |
| --- | --- | --- | --- |
| 0 | `RC` | TX to RX | CRSF channel payload (22 B) |
| 1 | `TLM` | RX to TX | uplink telemetry (8 B, see [tlm.h] layout below) |
| 2 | `DATA_UP` | TX to RX | passthrough data |
| 3 | `DATA_DOWN` | RX to TX | passthrough data |
| 4 | `SYNC` | TX to RX | session setup, hop alignment, profile and mix declaration |
| 5 | `PAIR_REQ` | TX to RX | pairing |
| 6 | `PAIR_RESP` | RX to TX | pairing |
| 7 | `PAIR_CFM` | TX to RX | pairing |
| 8 | `SESS_RESP` | RX to TX | session response carrying `nonce_r` |
| 9 | `SESS_CFM` | TX to RX | session confirmation, tagged with the new session key |

The telemetry payload is the standard CRSF `0x1D Link Statistics TX` payload: RSSI, RSSI percent,
link quality, SNR, receiver RF power in dBm, actual frame rate divided by 10, antenna 2 RSSI and
active antenna. The TX additionally emits standard CRSF `0x14 Link Statistics` for handset status.

`0x14` uplink power is the ground TX's power, not the air RX's power. Its RF profile enumeration
only defines 4/50/150 fps; other rates use reserved `0xff`, and `0x1D` reports the actual rate at
10 fps resolution. RSSI percentage is zero until a calibrated mapping exists. The unsigned
`0x1D` dBm field cannot represent negative bench power, so negative setpoints saturate to zero.

RC uses CRSF cut failsafe: the TX clears its channel payload after the handset-input timeout,
and RX only forwards newly received RC. Neither end generates preset channel values at boot,
after management mode or during link loss. The flight controller detects missing `0x16` frames;
the [CRSF specification](https://github.com/tbs-fpv/tbs-crsf-spec/blob/main/crsf.md#0x16-rc-channels-packed-payload)
recommends that the FC wait one second before starting its failsafe routine.

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
PAIR_REQ  {dev_a, dev_b=0, nonce_a, nonce_b=0}
PAIR_RESP {dev_a, dev_b, nonce_a, nonce_b}
PAIR_CFM  {dev_a, dev_b, nonce_a, nonce_b}
```

All three pairing payloads are fixed at 16 bytes. The TX (initiator) sends `PAIR_REQ`; the RX
answers `PAIR_RESP` with its own id and a fresh `nonce_b`, and the TX finishes with `PAIR_CFM`.
Both ends keep `nonce_a || nonce_b` as the pair salt, from which the pairing-level
keys derive ([09-security](09-security.md) 3). Pairing messages are tagged with the shared
`auth_key` and the pairing sync word; once it succeeds both ends switch to their link's normal
sync word and derived keys.

### 3.4 Sessions

1. TX to RX: `SYNC{version, profile, mix, hop, nonce_t, frame}`, authenticated with the pairing-level
   `link_auth`.
2. RX to TX: `SESS_RESP{nonce_t, nonce_r, hop, frame}`, also authenticated with `link_auth`; the RX draws
   `nonce_r` now.
3. TX to RX: `SESS_CFM{nonce_r, hop, frame}` tagged with the **new session key**; the RX adopts the
   session only after it verifies.

The DATA initiator repeats its confirm until it receives a newer frame authenticated with the
session key. The responder answers a valid confirm with an empty DATA ACK, even without reverse
application data or ARQ. A newer repeated confirm requests another reply and updates the shared
epoch without resetting stream or ARQ progress; old confirms are rejected. Both peers keep DATA
on the sync channel for 16 frames counted from the most recent confirm. A delayed pairing-level
response for the already confirmed nonce pair cannot reopen this exchange. While confirmation
is pending, TX also sends SYNC at the acquisition interval, so an RX that rebooted and lost its
candidate can generate a new response. No new packet type or application payload is added.

Session key: `KDF(pair_key, "zlrs/session", nonce_t || nonce_r || sender id || receiver id)`,
one per direction. Sequence numbers restart at 0 for the session.

SYNC appends a little-endian 32-bit frame counter after `nonce_t` (12-byte payload), and
SESS_CFM appends it after `hop` (10-byte payload). RX adopts the counter only after authenticated
session confirmation or an accepted, newer same-session SYNC. This aligns telemetry/data thinning
and periodic sync windows across independent boot times. Only the fixed 12-byte SYNC,
14-byte SESS_RESP and 10-byte SESS_CFM payloads are accepted; replay rejection precedes counter
changes.
SESS_RESP appends hop (16 bits) and frame (32 bits), making a 14-byte payload. Only the debug
handshake-disabled initiator adopts those fields, so lost responses do not leave its hop count
behind the responder. For the first 500 ms after connection, both ends keep the uplink on the
sync channel while normal hop accounting continues; this also leaves room to repeat a lost confirm.

**Why the second nonce is mandatory**: with `nonce_t` alone the key would come entirely from a
value the TX puts on air, which anyone can replay; mixing in the RX's fresh `nonce_r` means a
replay only earns a candidate session under a brand new key, and adopting it needs the TX to
confirm with that same key. Retransmits repeat the same response/confirmation, so losing one
message costs a single `SYNC` period.

### 3.5 Sync and resync

- **SEARCHING**: `SYNC` goes out every 100-600 ms depending on the modulation (faster rates
  tolerate a tighter cadence), each one carrying the hop index the sender is on.
- **CONNECTED**: a sparse `SYNC` at `ZLRS_APP_RESYNC_PERIOD_MS` (default 5 s, converted to
  frames and capped at 255 frames; the actual interval may be shorter)
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

Messages larger than one frame are split at the radio MTU. Each accepted packet is delivered to
the byte stream immediately; packet boundaries are not message boundaries.

### 4.3 Reliable transfer (ARQ)

The data link uses stop-and-wait independently in each direction:

- Header bit 1 is the ACK toggle for data received from the peer; bit 0 is reserved and must be zero.
  The toggle starts at 0 for every new session and flips only after accepting the next
  in-order data packet. Empty DATA and duplicate data never flip it.
- Each sender keeps exactly one outstanding packet. Before receiving the matching flipped
  ACK bit it sends no next packet; its queued bytes remain retained. The expected bit is
  `(data_seq + 1) & 1`, with the first data sequence 0 expecting bit 1.
- Every DATA reports the current receive toggle, even when it is 0. With no reverse user
  data, the receiver sends an empty DATA containing only the common header and tag
  (7 bytes under P1). There is no ACK packet type, ACK sequence field or header extension.
- ARQ payload retains a 2-byte data sequence for deduplication. Retransmissions keep that sequence
  and payload but obtain a fresh outer transmission sequence.
  Duplicates repeat the same receive toggle; future data sequences are rejected.
- The sender retransmits after its configured timeout. The core fallback is 30 ms;
  the application budgets two data opportunities, with a 10 ms minimum in data24.
  Unchanged ACK bits do not postpone the timer. Long retry runs are counted every 64 retries,
  and the same packet continues to be retransmitted without a recovery probe or progress reset.
- A new session resets both data sequences and receive toggles. Repeated confirmation of the
  existing session, periodic SYNC and idle periods preserve stream progress.
- ACK bits are authenticated with the DATA header; malformed DATA cannot acknowledge bytes.
  Replay checks run before ACK processing.

Connected data24 may encode and authenticate its next eligible DATA during RX and preload
its separate TX FIFO. Preparation never advances sequence numbers, hopping or queue state.
While a packet is outstanding only its timed-out retransmission can be prepared; the next
packet becomes eligible after ACK. Activation validates the cached sequence, key, MTU and
profile. A prepared packet can report the previous reverse ACK toggle; any newer receive
progress remains pending for the following DATA reply.

Only the current wire format is implemented; both peers must run version 5.

An idle transmit slot remains idle rather than listening to the opposite direction. After the
RX data link misses two periodic SYNC intervals, its A window listens on the sync channel until an authenticated periodic SYNC restores frame/hop alignment. The
session and ARQ receive position are retained; recovery does not require matching the stale
receiver's resync-frame counter.

Without ARQ (`CONFIG_ZLRS_APP_DATA_ARQ=n`, debug images) the data link is best-effort: bytes are
dropped and counted when the port rings are full.

## 5. Hopping and regulation

### 5.1 Hop tables and sub-band mixing

- Each link derives a hop table with capacity **256 entries** and actual length
  `floor(256 / channel_count) * channel_count` from `fhss_seed`; every block of `channel_count`
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
