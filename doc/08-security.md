# Security

This specification defines identity, key derivation, authentication, replay
protection, randomness and local trust. Wire fields and state transitions are
specified in [air protocol](07-protocol.md).

## Identity and threat model

A device has a public four-byte identity and a secret 32-byte group root key.
Identity is generated from hardware entropy when absent and stored in NVS.
An all-zero identity is invalid. One pairing record stores peer ID and the two
pairing nonces. Equal endpoint IDs cannot form a pair.

The root key is provisioned over local USB management, directly or from a phrase.
It is never returned through status or diagnostics and is never sent by radio.
Devices sharing the root key can authenticate pairing exchanges.

An adversary may record, replay, forge or jam radio traffic. Production profile P1
authenticates packets but exposes plaintext. Its four-byte tag provides limited
forgery resistance; fresh sessions do not provide payload confidentiality. Group
root-key compromise permits deriving keys and impersonating members. Group
revocation requires replacing that root key. Physical compromise, protection
against radio jamming and forward secrecy are outside the design.

| Profile | Air behavior | Availability |
| --- | --- | --- |
| P0 | No authentication tag | Debug images only |
| P1 | Four-byte truncated HMAC-SHA256 | Production default |
| P2 | Encrypted profile | Reserved scope; unsupported by this wire format and firmware |

Both endpoints select the same profile locally. A received SYNC profile cannot
change local authentication or tag length. Firmware must reject unsupported
profiles, including P0 in production.

## Cryptographic parameters

| Purpose | Requirement |
| --- | --- |
| Phrase to root key | PBKDF2-HMAC-SHA256, 100000 iterations, salt `zlrs/kdf/v1`, 32-byte output |
| Key derivation | HKDF-SHA256 with empty salt and label/context separation |
| Frame authentication | First four bytes of HMAC-SHA256 |
| Tag comparison | Constant-time comparison |

A phrase has 1–32 UTF-8 bytes; a strong phrase of at least 12 characters is
recommended. Direct root-key provisioning accepts 64 hexadecimal characters.
Phrase derivation runs on the device in management with RF paused and the
application watchdog disarmed. Its fixed salt and work factor are protocol
parameters, not a guarantee against weak phrases.

For key derivation, HKDF info is the ASCII label without a terminating NUL,
followed immediately by the context bytes. Empty salt is equivalent to 32 zero
bytes for SHA-256 extraction. Context integers are little endian.

| Derived value | Input key | Label | Context | Output |
| --- | --- | --- | --- | --- |
| Pairing authentication | Root | `zlrs/auth` | Empty | 32 bytes |
| Pair key | Root | `zlrs/pair` | Ordered IDs followed by ordered pairing nonces | 32 bytes |
| Directional link authentication | Pair key | `zlrs/link-auth` | Sender ID, receiver ID | 32 bytes |
| FHSS seed | Pair key | `zlrs/fhss` | Empty | 4 bytes |
| DATA sync word | Pair key | `zlrs/sync/data` | Empty | 4 bytes |
| Directional session key | Pair key | `zlrs/session` | Leader nonce, Follower nonce, sender ID, receiver ID | 32 bytes |

Device IDs are ordered lexicographically by their four bytes. Pairing nonces are
ordered independently as unsigned 32-bit values, then encoded little endian.
This canonical pairing context gives both endpoints the same pair key.
Directional contexts give one endpoint's outgoing key to the other's incoming
side and prevent reflection of local traffic as peer traffic.

SHA-256 must preserve ordered input spans, partial words and final padding.
Hardware hashing requires exclusive ownership of the operation, interrupts left
enabled and no DMA allocation. Hardware failure must not produce a fabricated
digest or valid tag. Keys and secret intermediate values must not enter state
channels, logs or diagnostic output.

## Pairing and session proof

Pairing runs for 30 seconds after a one-second button hold or a management
request. Leader initiates requests every 100 ms. Automatic mode uses 2.4 GHz;
fixed mode uses its selected band. Pairing messages authenticate explicit IDs
and nonces using the root-derived pairing authentication key.

PAIR_REQ contains the initiator identity and nonce. A responder contributes a
fresh nonce for each distinct identity/nonce request. Duplicate requests reuse
that pending nonce. PAIR_RESP and PAIR_CFM both bind the complete two-ID,
two-nonce tuple. Confirmation is idempotent; re-pairing replaces the peer record
and derives a new pair subtree. Keys are derived rather than exchanged.

A session uses fresh Leader and Follower nonces. SYNC and SESS_RESP authenticate with the
directional link key. SESS_RESP must echo a pending Leader challenge. Leader confirms
with the candidate session key; Follower adopts the session only after its confirmation
verifies the expected Follower nonce and receive context. Three-step confirmation is
required even for P0's unauthenticated debug format.

Repeated valid responses and confirmations preserve confirmed progress. A
response to a completed challenge cannot directly replace keys or reset ARQ.
A rebooted peer requires a new challenge and full exchange. Session nonces, keys
and sequences are volatile; pairing persists. A frequency change alone retains
the session and its replay/deduplication state.

## Replay protection

Every air type carries a common u32 transmission sequence. Zero is not transmitted.
Each direction uses a continuous sequence for the selected pairing/session round,
including handshake and connected traffic. Retries receive a fresh common
sequence; the outstanding DATA packet retains its seven-bit DATA identity.

An accepted packet must have a strictly greater sequence in its applicable
receive context. Counters cannot wrap or reset within that context. Exhaustion
requires a fresh nonce context before further transmission. DATA's modulo-128
sequence and reverse one-bit ACK serve delivery and remain separate from replay
sequences and the shared TDD period count.

Preliminary sequence checks are read-only. Role, direction, identity, nonce,
receive state and authentication must pass before a baseline or link effect is
committed. A forged high sequence cannot block later valid packets. Pairing,
candidate sessions and active DATA sessions use distinct baselines; a larger
sequence does not authorize key replacement.

A recorded initial request can elicit a response when its receive baseline is
absent. The responder's fresh nonce and authenticated confirmation prevent the
recorded exchange from completing. Sequence checks alone cannot establish
freshness across reboot. Nonces are 32-bit random values with collision risk,
not globally unique identifiers.

Band plans are authenticated within SYNC or session confirmation. Mode, active
domain, target and future period must pass the [plan constraints](07-protocol.md#authenticated-plan).
Invalid plans cannot retune RF, move the clock or advance replay state. P0 lacks
these authentication guarantees even though its structural checks are identical.

## Entropy contract

Session and pairing randomness shall use a cryptographic random service backed by
the board's hardware entropy source. Identity generation also uses hardware
entropy. Each operation reports success or failure; a failed or partial result
must not become an identity, nonce or proof.

The RP2350 source collects fresh 192-bit blocks with hardware health checks
enabled. Reads consume complete blocks, retain the requested bytes and stop the
source when finished. A block failing health checks is discarded and collection
restarts within one three-second deadline per block. Health-check recovery cannot
extend that deadline; continuous errors and stalled generation return failure.
This accommodates the variable collection time documented in the
[RP2350 datasheet, section 12.12.4](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf).
Reads are
serialized in thread context with interrupts enabled so USB and watchdog service
remain possible. ISR entropy requests are unsupported.

Nonce generation permits zero and rejects repetition of the preceding local
nonce, with a bounded number of fresh draws. Random failure prevents transmission
of a new handshake proof. Time, counters and noncryptographic generators cannot
replace entropy. A recovered source permits a new attempt without treating
queued DATA as acknowledged. The deterministic FHSS shuffle is separate from
security randomness.

## Local management and firmware trust

Management runs only on its USB CDC. Physical/local host access is the trust
boundary; management framing uses CRC8 and does not authenticate the host.
Authorized local operations can replace credentials, erase pairing, reset
settings, inspect status or enter firmware recovery. Credential operations
invalidate the affected active link in the same maintenance window.

Firmware authenticity uses MCUboot's ECDSA-P256 trust anchor, independently of the
wireless root key. Test images require successful host confirmation or are subject
to rollback. Release signing keys must be private and match the installed
bootloader. See [firmware lifecycle](06-firmware.md#installation-updates-and-rollback).

Acceptance requires independent crypto vectors, wire tags, direction/reflection
checks, replay boundaries, repeated handshakes, nonce mismatches, entropy timeout
and recovery, constant-time comparison and hardware execution. Results belong in
validation artifacts, separate from these design requirements.
