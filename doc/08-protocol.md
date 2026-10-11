# Over-the-air protocol

Protocol version `8` carries one bidirectional DATA service. Both peers must use this version. A `SYNC` with another version is rejected. The LR2021 supplies preamble, sync word, length and physical CRC; the ZLRS frame below is the radio payload.

## Frame

| Field | Size | Meaning |
| --- | --- | --- |
| Header | 1 B | Packet type in bits 1–7, ACK toggle in bit 0. |
| Frame sequence | 2 B | Little-endian counter for authenticated replay rejection. |
| Payload | Variable | Packet-specific fields; DATA includes peer-quality feedback. |
| Authentication tag | 0 or 4 B | Truncated HMAC-SHA256 over header, sequence and payload; absent only in debug P0. |

The FLRC frame limit is 511 bytes. All multi-byte protocol fields are little-endian. The application sets a DATA MTU that leaves room for the four-byte ARQ/feedback header and authentication tag.

| Type | Value | Direction | Use |
| --- | --- | --- | --- |
| `DATA_UP` | 0 | TX → RX | DATA bytes or feedback-only frame. |
| `DATA_DOWN` | 1 | RX → TX | DATA bytes or feedback-only frame. |
| `SYNC` | 2 | TX → RX | Version, session and hop alignment. |
| `PAIR_REQ` | 3 | TX → RX | Start authenticated pairing. |
| `PAIR_RESP` | 4 | RX → TX | Return peer identity and nonce. |
| `PAIR_CFM` | 5 | TX → RX | Confirm pairing. |
| `SESS_RESP` | 6 | RX → TX | Session nonce response. |
| `SESS_CFM` | 7 | TX → RX | Confirm the fresh session key. |

## DATA and quality feedback

With ARQ enabled, a data-bearing payload is `{data_seq:u16, rssi_magnitude:u8, lq_percent:u8, bytes[]}`. Without ARQ it is `{rssi_magnitude:u8, lq_percent:u8, bytes[]}`. A feedback-only frame carries just the last two bytes, even when ARQ is enabled. A magnitude of zero means no usable RSSI sample; otherwise the receiver interprets it as a negative dBm value. Quality is 0–100 percent. These values describe the peer's reception of this node's transmissions, so each sender can adjust its own TX power. Feedback-only frames are sent during idle periods, allowing power adaptation without user traffic.

Each direction has a stop-and-wait ARQ sequence. A receiver changes its ACK toggle only after its serial sink accepts the bytes; if it is full, the sender retains and retransmits the packet. Retransmissions keep the DATA sequence, while new radio frames keep advancing their authenticated frame sequence. A new session resets the ARQ state. Payload bytes remain opaque to the radio protocol.

## SYNC payload

`SYNC` carries 12 bytes: `{version:u8, profile:u8, hop_index:u16, nonce_t:u32, frame_index:u32}`. The high four bits of `profile` are reserved and must be zero. The current DATA product uses one channel domain at a time; no second-table mix or version is negotiated. These fields and the normal zero reserved bits retain the existing version 8 byte format.

## Pairing and sessions

A device with no root key remains off air. A configured device without a peer is unpaired. Pairing is opened by button or management command for a bounded window; authenticated request, response and confirmation exchange IDs and nonces, from which both sides derive and store their pairing record. A paired device searches for its peer and uses `SYNC`, `SESS_RESP` and `SESS_CFM` to establish fresh directional session keys. `CONNECTED` means the session is valid and DATA can flow. Authentication and replay checks precede ACK processing and delivery. See [security](09-security.md) for the key tree and trust limits.

## Slots, hopping and regulation

Each frame contains two slots: DATA A (0, TX → RX) and DATA B (1, RX → TX). The LR2021 uses hardware sync word 1 for DATA and 2 for pairing. Pairing uses its own sync slot; connected traffic uses the DATA sync slot. A selected frequency band supplies its channel table and modem timing. FHSS derives a deterministic hop sequence from pairing material. The sub-G band uses its board SKU and region's channel, TX-power and duty rules; when the duty budget is exhausted, transmission is deferred. The active TX power never exceeds the configured/regional ceiling. Current firmware runs DATA on one selected band at a time.
