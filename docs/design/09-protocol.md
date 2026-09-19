# Over-the-air protocol

The zlrs over-the-air protocol: what the physical layer hands us, what a frame looks like, how the link state machine moves, how sessions and sync are set up, how reliable transfer works, how the superframe is laid out, and how hopping and the regulations land in code. Identity and keys: [10-security](10-security.md); timing budget: [07-architecture](07-architecture.md) 4; chip parameters: [06-radio](06-radio.md).

## 1. What the physical layer already provides

| Capability | From hardware | What we do |
| --- | --- | --- |
| Preamble | yes (`agc_pbl_len`) | values in 5.3 |
| Sync word | yes (FLRC, three slots of 2 or 4 bytes) | used as the first filter, see 1.1 |
| CRC | yes (`SetFlrcPacketParams`) | **no CRC of our own** |
| Length field | yes (variable-length packets) | we carry no length |

Conclusion: **frames do not repeat the CRC or the length**; both belong to the radio link layer.

### 1.1 Pairing filtering uses the sync word

**No `pair_id` in the frame: the radio's sync word does the filtering.** Matching is done in hardware, so a non-matching packet is dropped by the radio and never wakes the MCU.

| Slot | Value | Used for |
| --- | --- | --- |
| 1 | `KDF(pair_key, "zlrs/sync/control")[:4]` | the sub-GHz control link |
| 2 | `KDF(pair_key, "zlrs/sync/data")[:4]` | the 2.4G data link |
| 3 | a public constant | pairing mode |

After pairing, this device cannot hear other pairs at the physical layer, spending no MCU time and no air bytes on them. The sync word is always 4 bytes (32 bits) and the rate tier is not encoded into it. The cost: changing band means reconfiguring the sync word too, and a new pairing has to derive a fresh one.

## 2. Frame format

| Field | Length | Notes |
| --- | --- | --- |
| `header` | 1 B | top 6 bits = type (64 of them), low 2 bits = flags |
| `seq` | 2 B | increases within a session; replay protection, reassembly and reliable transfer use it |
| `payload` | 0..N B | type dependent |
| `tag` | 0 or 4 B | truncated HMAC over header + seq + payload |
| `crc` | appended by hardware | generated and checked by the radio link layer |

**The MTU is not a separate parameter**: it is the largest data payload the current frame format allows and moves with the rate tier and packet format. Whether a `tag` is present and whether the payload is encrypted are decided by the profile, not by flags.

### 2.1 Header flags

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `MORE` | more fragments follow |
| 1 | `ACK_REQ` | asks the peer to acknowledge (used by the reliable mode) |

### 2.2 Packet types

| Value | Type | Direction | Notes |
| --- | --- | --- | --- |
| 0 | `RC` | TX to RX | RC channels |
| 1 | `TLM` | RX to TX | telemetry |
| 2 | `DATA_UP` | TX to RX | passthrough data |
| 3 | `DATA_DOWN` | RX to TX | passthrough data |
| 4 | `ACK` | both ways | reliable-transfer acknowledgement (see 4.3) |
| 5 | `SYNC` | TX to RX | session setup, hop sync, profile declaration |
| 6 | `PAIR_REQ` | TX to RX | pairing |
| 7 | `PAIR_RESP` | RX to TX | pairing |
| 8 | `PAIR_CFM` | TX to RX | pairing |
| 9 | `SESS_RESP` | RX to TX | session response carrying `nonce_r` |
| 10 | `SESS_CFM` | TX to RX | session confirmation tagged with the new session key |
| 11-63 | reserved | - | - |

### 2.3 Security profiles

| Profile | tag | Encryption | Used for |
| --- | --- | --- | --- |
| `P0` | none | none | **maximum performance**: least overhead and the shortest frames, good for racing; no authentication, so it can be injected into |
| `P1` | 4 B | none | **the default**: every frame authenticated, nothing encrypted |
| `P2` | 4 B | AES-CTR | authenticated encryption with the MAC over the ciphertext (encrypt-then-MAC) |

- `tag` = HMAC-SHA256 truncated to 4 B over `header` + `seq` + `payload`; the key is directional, see [10-security](10-security.md) 3.
- HMAC-SHA256 keeps the identity layer on a single primitive: PBKDF2, HKDF and the MAC all sit in one function family.
- **P2 comes last**: encryption is implemented once everything else is done.
- The profile is a compile-time setting (`CONFIG_ZLRS_APP_SECURITY_PROFILE_P0`, `_P1`, `_P2`) and the profile declared in `SYNC` has to match the local one.

### 2.4 Control payload and rate tiers

| Item | Default | Other tiers |
| --- | --- | --- |
| Control rate | 150 Hz | 500 Hz for close-in racing; 1000 Hz is the target (`r=0`, all 2.4G) |
| Channels | 16 | - |
| Bits per channel | 11 | - |
| `RC` payload | 22 B | - |

The `RC` payload carries CRSF's 22 bytes of channel data verbatim (16 channels packed at 11 bits) and the RX rebuilds CRSF's header and CRC8. At 150 Hz with a 22 B payload, a 27-byte frame takes about 222 us at 0.975 Mbps, 3.3% of a 6.67 ms frame.

### 2.5 Fragmentation

- `MORE` set means more fragments follow.
- The receiver reassembles strictly in `seq` order; a gap or more than 100 ms without completion drops the whole run.
- No out-of-order reassembly: an explicit fragment header would cost 2 B per frame, more than it buys.

### 2.6 Overhead on air

With a 22-byte payload at FLRC 1.3 Mbps (0.975 Mbps effective):

| Composition | Bytes | Airtime (estimate) |
| --- | --- | --- |
| header + seq + payload | 25 | about 205 us |
| plus a 4 B tag (P1/P2) | 29 | about 238 us |
| preamble and sync word | - | another few tens of us |

The tag costs about 16%; P0 carries none and is therefore shorter and faster.

### 2.7 Link quality and diagnostics

**No separate diagnostics channel: CRSF's existing link statistics frame** (`0x14` LINK_STATISTICS) is reused:

| CRSF field | Where our value comes from |
| --- | --- |
| uplink RSSI / LQ / SNR | measured by the RX and sent back in telemetry |
| downlink RSSI / LQ / SNR | measured by the TX |
| RF mode | the current rate tier |

The `TLM` payload therefore carries exactly what CRSF's link statistics frame wants (whatever ELRS sends, we send). Rate adaptation takes the LQ from it and dynamic power takes RSSI and LQ, with no second measurement path. The data link's overflow and drops do not go into CRSF; only the config protocol's `STATUS` counts them.

## 3. Link state machine

### 3.1 How the two links relate

The control link and the data link each run a copy of the same state machine and are independent. System-level "online" is the control link's call; losing the data link only costs throughput.

### 3.2 States

| State | Meaning |
| --- | --- |
| `UNCONFIGURED` | no `root_key`, nothing happens |
| `UNPAIRED` | a `root_key` but no pairing record |
| `PAIRING` | the user held the button, pairing is under way |
| `SEARCHING` | paired, trying to bring a session up |
| `CONNECTED` | session up, traffic flowing |

### 3.3 Transitions on the TX

| From | Event | To | Action |
| --- | --- | --- | --- |
| `UNCONFIGURED` | `root_key` written | `UNPAIRED` | generate and store `dev_id` |
| `UNPAIRED` | long press | `PAIRING` | lower power, switch to the pairing sync word, send `PAIR_REQ` periodically |
| `PAIRING` | pairing succeeds | `SEARCHING` | store the peer's `dev_id` and both nonces, restore power, switch to the normal sync word |
| `PAIRING` | 30 s timeout | `UNPAIRED` | restore power and sync word |
| `SEARCHING` | `SYNC` sent periodically; a `SESS_RESP` derives the session and `SESS_CFM` goes out | `CONNECTED` | normal traffic starts |
| `CONNECTED` | 250 ms with no valid frame | `SEARCHING` | keep sending `SYNC` |

### 3.4 Transitions on the RX

| From | Event | To | Action |
| --- | --- | --- | --- |
| `UNCONFIGURED` | `root_key` written | `UNPAIRED` | generate and store `dev_id` |
| `UNPAIRED` | long press | `PAIRING` | switch to the pairing sync word and listen for `PAIR_REQ` |
| `PAIRING` | pairing succeeds | `SEARCHING` | store the peer's `dev_id` and both nonces, switch to the normal sync word |
| `SEARCHING` | a `SYNC` is answered with `SESS_RESP`, then a `SESS_CFM` tagged with this session's key arrives | `CONNECTED` | align on the hop index from `SYNC` |
| `CONNECTED` | 100 ms with no valid frame | `SEARCHING` | output failsafe values |

Failsafe is an **output behaviour** outside `CONNECTED`, not a link state: while `SEARCHING` the RX has to keep feeding the configured failsafe values to the flight controller. The TX configures them and ships them during pairing for the RX to store; the default is throttle at zero and the other channels centred.

Pairing payload (shared by `PAIR_REQ`, `PAIR_RESP` and `PAIR_CFM`):

```
[dev_a(4)][dev_b(4)][nonce_a(4)][nonce_b(4)][flags(1)][failsafe(22)]
```

Only the initiator (TX) fills in `failsafe`: when it has values it sets bit0 of `flags` and the responder stores exactly what it received, NVS included; without the bit the responder keeps its own.

### 3.5 Session setup (a three-way handshake)

The same shape as pairing: one nonce from each end, both feeding the KDF, and a last message tagged with the new key that echoes the other end's nonce. What differs is the key material and its lifetime: pairing uses the long-lived shared `root_key`, a session uses the `pair_key` that pairing established.

The TX sends `SYNC` periodically on the sync channel carrying:

| Content | Purpose |
| --- | --- |
| `nonce_t` | this session's random number |
| channel and hop index | what the RX aligns to |
| profile | rate tier, tag on or off, encryption on or off |
| sub-band | mix ratio `r` and hop table version (chosen by the TX, effective on the RX from the next sync frame) |

The three steps:

1. TX to RX: `SYNC{version, profile, hop, nonce_t}`, authenticated with the pairing-level `link_auth`.
2. RX to TX: `SESS_RESP{nonce_t, nonce_r}`, also authenticated with `link_auth`; the RX draws `nonce_r` now.
3. TX to RX: `SESS_CFM{nonce_r, hop}` tagged with the **new session key**, echoing the hop index this end is on right now; the RX adopts the session only after it verifies.

Session key `KDF(pair_key, "zlrs/session", nonce_t || nonce_r || sender id || receiver id)`: one per direction, out for what we send and in for what the peer sends (see [10-security](10-security.md) 3). Sequence numbers start at 0.

**Why the second step is mandatory**: with `nonce_t` alone the key would come entirely from a value the TX puts on air and anyone can replay; mixing in the RX's fresh `nonce_r` means a replay only earns a candidate session under a brand new key, and adopting it needs the TX to confirm with that same key.

**Retransmits**: for the same `nonce_t` the RX repeats the same response and the TX repeats its confirmation when a duplicate response arrives. Losing any one message costs a single `SYNC` period.

**When the link counts as up**: the TX enters `CONNECTED` as soon as the confirmation leaves, the RX only after it verifies one. If the peer never hears the confirmation, the peer-loss check pulls the TX back to `SEARCHING`.

### 3.6 Sync cadence

`SYNC` only ever goes out on the sync channel, at state-dependent intervals (following ELRS):

| State | Interval |
| --- | --- |
| searching or disconnected | 100-600 ms depending on the rate tier, with the RX scanning the sync channel |
| connected | **5 s** |

The sparse `SYNC` while connected does two jobs: it wipes out the hop-index error that crystal drift accumulates, and it notices a peer that changed sessions (a reboot or re-pair) so the handshake restarts by itself. A peer reboot is therefore noticed within 5 s at worst. Each link sends its own `SYNC` and sets up its own session.

### 3.7 Timers

| Item | Control link | Data link |
| --- | --- | --- |
| pairing window | 30 s | 30 s |
| reassembly | 100 ms | 100 ms |
| peer loss | 100 ms for RX failsafe, 250 ms for the TX | none: best-effort sends no idle frames |
| `SYNC` period | 5 s connected, 100-600 ms searching | the same |

Reassembly gets 100 ms because reliable transfer retransmits every 30 ms: anything shorter would throw away half-finished messages in mid-retransmit. Every timer is defined in time rather than frames, so changing the frame rate changes no parameter.

## 4. Superframe and link policy

### 4.1 A fixed superframe

**Every control period has exactly the same shape.** Allocating slots dynamically would save about 3% of switching cost, at the price of an uncomputable latency bound and problems that are hard to localise; the "deterministic latency, predictable and detectable failures" of [01-product](01-product.md) is worth more than those 3%.

One period (150 Hz, 6.67 ms) looks like this:

| Leg | Content | Roughly |
| --- | --- | --- |
| 1 | sub-GHz: the TX sends an `RC` frame | 238 us |
| 2 | sub-GHz: switch to receive, the RX sends telemetry (**mandatory answer**) | 91 + 238 us |
| 3 | switch to 2.4G | - |
| 4 | 2.4G: the data window, TX to RX first, RX to TX second | about 5.7 ms |
| 5 | switch back to sub-GHz | - |

The control link plus the band switches comes to about 924 us, 14% of the period (a rough figure for the 1.3 Mbps tier with 29-byte frames). **The frame rate is a tier**: 150 Hz by default, 500 Hz for racing, 1000 Hz the target; the effective value comes from the time plan (`zlrs_region_control_plan()`, with airtime from `zlrs_mod_airtime_us()`), clamped by the regional duty cap and airtime, with the telemetry and data windows thinned into each frame (1/3 and 1/5 at the 500 Hz tier).

1000 Hz needs the control link to own the radio and drive frames from IRQs: RAC's FLRC burst is a one-way batch transfer whose start still comes from a millisecond-granularity planner, so 1 ms frames are out of reach. At `r=0` a 31 B frame at 2.6 Mbps takes 108 us and the slot budget is 308 us, which separates the four slots cleanly.

### 4.2 Telemetry window

**The RX must answer**, sending a minimal empty frame (about 7 B) even with nothing to report: the TX decides the link is alive by whether something arrived this period, and without the answer it cannot tell "the link is down" from "there is nothing to say yet".

### 4.3 Data window and reliable transfer

The data window splits in half: TX to RX first, RX to TX second. That keeps the timing maximally predictable; an idle direction wastes half of it, which the data link accepts as a secondary channel.

The data link has **two modes** and defaults to reliable transfer:

| Mode | Behaviour |
| --- | --- |
| **reliable (ARQ, the default)** | the receiver sends cumulative ACKs ("this is the highest in-order sequence number"), the sender retransmits unacknowledged frames in the window (4 frames, 30 ms timeout, at most 6 tries); the receiver dedups, delivers in order and counts sequence gaps |
| best-effort | a raw byte stream that drops when the buffer is full and counts the overflow; the lowest latency, for upper layers that retransmit themselves and do not mind loss |

Reliable transfer follows ELRS's AirPort/MAVLink idea (acknowledge and resend until acknowledged) but uses a separate cumulative ACK frame instead of a single header bit: an ELRS receiver always has a frame to ride along in, while our RX direction may have no data at all, so a dedicated ack frame is needed. Narrowing that to a piggybacked bit is on [13-roadmap](13-roadmap.md).

When retries run out the run is counted as dropped (visibly), the window clears, sequence numbers restart and the receiver is told to void the half message. The first frame of a session has to start at sequence 0: dropping a run is better than handing the layer above a stream at the wrong offset.

Frame length is dynamic and capped by the largest payload the current frame format fits (the MTU), and **a frame has to finish inside its window, never spanning two**: at low rates a long frame no longer fits, so the cap follows the rate tier.

### 4.4 Rate adaptation

Both links adapt, with different objectives:

| Link | Objective | When quality drops |
| --- | --- | --- |
| Control | distance and stability | drop a rate tier for sensitivity and accept a lower frame rate |
| Data | rate and throughput | keep the rate and frame rate, never trade speed for distance |

A tier is therefore a combination of band, rate and frame rate. The data link's rate ceiling is derived from the configured serial baud rate (115200 has to carry about 11.5 kB/s, 921600 about 92 kB/s) and the config tool checks that it is reachable.

The verdict: each tier has a minimum LQ, moving up needs a further 10 percentage points of margin, three consecutive windows (1 s each, judged in `report()` at 1 Hz) have to agree, and nothing is judged for 2 s after a change. RSSI plays no part in the rate decision.

### 4.5 Dynamic power

Power also sets the link budget and changing it costs no airtime. The decision uses the uplink RSSI and LQ the RX reports, judged once a second:

| Condition | Action |
| --- | --- |
| RSSI below the floor (-95 dBm by default) or LQ below it (90%) | after 3 consecutive judgements, up one step (3 dB) |
| RSSI above the ceiling (-70 dBm) and LQ above it (98%) | after 3 consecutive judgements, down one step |
| in between | nothing moves, this is the hysteresis band |
| no telemetry for a second | straight back to the ceiling |

The ceiling is the smaller of the configured power and the regional limit (the regulation wins) and the floor is a setting (0 dBm by default). On by default.

## 5. Hopping and regulation

**Per-frame hopping**: every frame changes frequency, the index comes from the table derived from `fhss_seed`, the TX carries the index in `SYNC` and the RX stays in lockstep. That buys interference resistance, coexistence with other zlrs links and the channel count and dwell time the hopping rules want, at the price of the RX having to open a specific frequency at a specific moment.

### 5.1 Preconditions and resync

| Precondition | Where it comes from |
| --- | --- |
| a shared frequency table | derived from `fhss_seed + r + table version` on each end: one table holding both 2.4G and sub-G frequencies |
| a shared slot length | decided by the rate tier |
| a shared index | carried by `SYNC`, aligned when the link comes up and again every 5 s while connected |
| clock drift | the periodic `SYNC` while connected pulls the index back; searching relies on dense `SYNC` plus a scan of the sync channel |

A `SYNC` on a resync frame is tagged with the pairing-level `link_auth`, so an old frame verifies too; accepting one therefore needs two gates: **the sequence number must be new** (one sequence space per session) and **switching sessions needs recent total silence** (a new `nonce_t` is believed only when `now - last_rx > beacon_period`, otherwise it is a replay and dropped). That same rule is how a peer reboot reconnects by itself.

### 5.2 Regulation

- FCC 15.247's hopping rules want at least 50 channels, a dwell time of 400 ms or less and equal use of every channel; per-frame hopping satisfies them.
- Some sub-bands of ETSI EN 300 220 carry duty limits (1% or 10%) that directly bound the control rate and the share of slots the data link may use.
- 2.4G ISM has no hopping requirement.

This lives in `core/region`: four tables (FCC915 with no duty limit, EU868 at 1% per 60 s, CN470, CN433), `zlrs_region_validate()` at boot checks that the region's sub-G hardware matches the board (HF SKU for 868/915, LF SKU for 470/433, and a mismatch means a red alert and no radio), every transmission is booked against the duty budget, and a slot with none left is given up and counted.

Channel spacing follows the modulation bandwidth: CN470 uses 300 kHz spacing (96 channels) while CN433 is only 1.74 MHz wide and fits five 300 kHz channels. Narrow-band regions carry a `max_rate_bps`, and a configuration above it is clamped to 0.325 Mbps narrow-band FLRC.

**LoRa is the trade-rate-for-distance option and applies to the control link only**: the default is narrow-band FLRC (0.325 Mbps in 300 kHz, which fits narrow regions); LoRa buys sensitivity (SF7 is -118 dBm on sub-G) and costs tens of times the airtime, 72 ms for a 31 B frame. EU868's 1% duty cannot feed that tier (0.14 frames per second), so it stays on narrow-band FLRC.

### 5.3 Preamble and sync words

- FLRC: sync words are 4 bytes and the three slots (1 control, 2 data, 3 pairing) are derived from the pairing key by role (`derive_pair()`).
- LoRa: the sync word is one byte derived separately (`zlrs/sync/lora/control` and friends) with the top bit set as the private bit, satisfying the rule that `0x0X` values are not allowed; pairing frames always use the private sync word `0x12`.
- Preamble lengths leave margin over the datasheet minimum (8 symbols for LoRa by default, 12 or more suggested for SF5/SF6).
