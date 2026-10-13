# Security

This document defines identity, keys, authenticated exchanges, replay checks and
the local management trust boundary. Air fields, receive rules and link behavior
are specified in [08-protocol](08-protocol.md).

## 1. Identity

Two values identify a device and its group:

- **Device id** (4 bytes): generated from the TRNG on the first boot and stored in NVS. It is
  not secret; it is how two ends name each other. `new-dev-id` makes a fresh one on demand,
  which drops the pairing record because the key tree is derived from it.
- **Root key** (32 bytes): the shared secret of a group. It is written once with the management tool
  (a 64-character hex key, or a passphrase the device derives), never leaves the device and is
  the root of every derived value.

One device pairs with one peer: there is a single pairing record (peer id plus the two pairing
nonces) in NVS, and re-pairing replaces it.

## 2. Threat model

P1 must authenticate paired endpoints and reject stale messages within the
selected exchange. An adversary may record, replay, forge or jam radio traffic.
P1 exposes plaintext and uses a 32-bit authentication tag, limiting forgery
resistance; encryption is a future profile extension.

Possession of the group root key allows deriving traffic keys and impersonating
members. Session keys and sequences do not repair that compromise. Physical
compromise and forward secrecy are outside this design.

Consequences worth stating plainly:

- **Key compromise is total for that group**: past recorded traffic can be decrypted once P2
  exists, and a holder of the key can impersonate a member.
- **Revocation means changing the root key**: it is per group, not per device.
- **P0 has no authentication at all**: it carries no tag and can be injected into. It exists for
  maximum performance in trusted environments and cannot be selected in a production image.

## 3. Key tree

All derivations are HKDF-SHA256 with an empty salt and a fixed label; the labels are the domain
separation. `KDF(key, label[, ctx])`:

```
root_key ── KDF "zlrs/auth" ───────────────────────────► auth_key        (pairing messages)
         └─ KDF "zlrs/pair", ordered_ids || salt ─────► pair_key
                                                          ├─ KDF "zlrs/link-auth", ids ────► link_auth_out / link_auth_in
                                                          ├─ KDF "zlrs/fhss" ─────────────► hop-table seed
                                                          ├─ KDF "zlrs/sync/data" ────────► data sync word (4 B)
                                                          └─ KDF "zlrs/session", nonces ──► session keys (per direction)
```

- `ordered_ids` and the ordered pairing salt (both sorted numerically) mean both ends derive the
  same `pair_key` regardless of who did what first.
- `link_auth_*` and the session keys are **directional**: the context carries local-then-peer for
  outgoing frames and peer-then-local for incoming ones. One key for both directions would let
  our own frame, echoed back, pass as the peer's (reflection).
- Re-pairing produces new nonces, so the whole subtree below `pair_key` changes.

The link instance holds the root key and derives the subtree. The frame codec never receives
the root key. Management stores the key through persist in a maintenance window.

## 4. Pairing

Pairing decides **who** we talk to; the session decides **which key** this conversation uses.

- Pairing runs on the selected DATA band and stores one peer record.
- The window is opened by a 1 s button hold or by the `pair` command and lasts 30 s; `PAIR_REQ`
  repeats every 100 ms.
- The request carries the initiator's `dev_a` and `nonce_a`. The response and confirmation
  carry both device IDs and both 4-byte nonces. They are tagged with `auth_key` and use the
  pairing sync word.
- **No key is transferred**: pairing authenticates the two device ids and exchanges the nonces
  from which both ends derive `pair_key` ([08-protocol](08-protocol.md)).
- The responder contributes a fresh `nonce_b` for a new request; response and confirmation
  bind both IDs and nonces. The initial request cannot cover a responder nonce that does
  not yet exist. Repeated requests must retain their pending responder nonce; the reviewed
  design identifies a request by the initiator ID together with `nonce_a`.

## 5. Sessions

A session is a fresh key pair derived from `pair_key` and two fresh nonces, one from each end:

```
session_out = KDF(pair_key, "zlrs/session", nonce_t || nonce_r || sender || receiver)
```

Properties:

- **One nonce from each end**: a TX-only nonce could be replayed whole.
- **Adopted only after confirmation**: the TX tags `SESS_CFM` with the new key, and the RX
  adopts the session only once that verifies.
- **Only traffic keys rotate**: the pairing record is untouched, and the nonces, session keys
  and sequence numbers are never stored, so power loss ends them.
- **`SYNC` is authenticated with the pairing-level `link_auth`**: the nonce is public, but nobody
  can forge a session message that verifies.

## 6. Profiles

| Profile | Over the air | Status |
| --- | --- | --- |
| `P0` | no tag | debug images only; maximum performance, no authentication |
| `P1` | 4 B truncated HMAC-SHA256 per frame | production default |
| `P2` | 4 B tag plus AES-CTR payload encryption (encrypt-then-MAC) | future extension requiring a new agreed format; see [12-roadmap](12-roadmap.md) |

The profile is configuration: `set-profile` stores it, the build carries the default, a
production image refuses `P0`, and the profile a peer declares in `SYNC` has to match the local
one.

## 7. Replay protection

- Every frame carries a common u32 sequence. Zero is not transmitted. Within a
  selected receive context, an accepted frame must have a strictly greater sequence.
- Each direction's handshake and subsequent traffic share a continuous counter.
  Adoption must not reset it. Exhaustion requires a fresh nonce context and session.
- A preliminary sequence check is read-only. Direction, identity, nonce, receive
  state and P1 authentication must pass before counters or link state change.
- Pairing, candidate exchanges and confirmed sessions have distinct contexts. A
  restarted peer's request cannot be judged solely against a previous DATA baseline;
  a larger sequence alone cannot authorize a session change.
- SESS_RESP must match a pending local challenge. An old response cannot reinstall
  a key or reset ARQ. After loss of peer traffic, recovery must issue a new local
  nonce and complete the full exchange.
- Replaying an initial request may cause an answer; the responder's fresh challenge
  and authenticated confirmation must prevent completing the old handshake.
  Repeated valid messages must preserve confirmed progress.
- One outstanding DATA is retained per direction. Its one-bit ACK and seven-bit
  sequence are independent of common frame sequence; retries preserve DATA identity
  but use a new frame sequence.
- Nonces require a secure random source that reports failure. Missing entropy must
  stop handshake transmission, never trigger a clock fallback. Zero is valid; a
  new local round must not repeat the preceding local nonce. A 32-bit random value
  has collision risk and is not a globally unique identifier.

The application requires a cryptographic random service backed by the board's
entropy source. Each request reports success or failure; output from a failed
request must not be consumed. Entropy waits and retries must be bounded, with
interrupts enabled so USB and the watchdog remain serviceable. A failed hardware
health check discards the block. No clock value or noncryptographic generator may
replace missing entropy. Once the source recovers, a fresh request may resume the
pending exchange without treating queued DATA as delivered.

## 8. Parameters and algorithms

- Input is a **passphrase** (at least 12 characters recommended) or 64 hex characters.
- PBKDF2-HMAC-SHA256, **100,000 rounds**, fixed salt `zlrs/kdf/v1` that carries the KDF version;
  the derivation runs on the device, so a third-party configurator cannot weaken it.
- Derivation runs in management mode with the watchdog disarmed; its duration depends on the
  crypto backend.
- MAC: HMAC-SHA256 truncated to 4 bytes. Keeping PBKDF2, HKDF and the MAC in one hash family
  means P1 needs no AES.

| Purpose | Algorithm |
| --- | --- |
| passphrase to root key | PBKDF2-HMAC-SHA256 |
| root key to derived values | HKDF-SHA256 |
| per-frame tag | HMAC-SHA256 truncated to 4 B |
| P2 encryption (planned) | AES-CTR with the MAC over the ciphertext |

Cryptographic operations must follow the algorithms and parameters above
independently of the hardware provider. SHA-256 must support ordered input spans,
partial words and final padding without changing protocol bytes.

A complete hardware hashing operation requires exclusive ownership, with
interrupts enabled and no DMA allocation. Provider failure must stop the operation,
never produce a fabricated digest or tag. Secrets must not enter status channels
or diagnostic output.

Acceptance includes independent SHA/HMAC/HKDF/PBKDF2 vectors, multipart input, wire
tags, constant-time tag comparison, sequence boundaries, malformed frames, replay,
entropy failure and native/hardware execution. Results are recorded separately.
Random-service acceptance also covers nonce-sized and partial-block reads,
successive requests, the error path from entropy service to handshake, timeout
with USB still available, and successful reads after the source is restored.

## 9. Management protocol

The management protocol runs on the **management CDC only**, on a local USB connection; physical
access is the boundary. It is a small framed protocol (`0xA5 0x5A`, version 4, CRC8) with request
and response frames. Business effects and status fields form the management contract.

| Host command | Effect |
| --- | --- |
| read status | role, link state, flags (bit 3: DATA uses sub-GHz), `dev_id`, `peer_id`, LQ, radio design band, region, security profile, power ceiling, data baud |
| enter management mode | pauses radio work; ACK follows session setup and synchronous log flushing |
| start pairing | opens the pairing window at once |
| erase pairing | erase the saved record and invalidate active pairing in the same maintenance window |
| write passphrase | the device derives the root key with PBKDF2 |
| write root key | 64 hex characters |
| set data baud | 9600-4000000, effective on the next boot |
| set region | validated against the board's radio design, refused on mismatch, next boot |
| set power ceiling | never above the chosen region's limit, next boot |
| set role | `tx` or `rx`, next boot |
| set DATA band | `auto`, `2.4` or `subg`, next boot; auto pairing uses 2.4 GHz |
| set profile | `p0` (debug images only), `p1`, `p2` (future extension), next boot |
| new device id | makes a fresh device id from the TRNG and stores it; the pairing record is erased with it (the peer knows the old id) |
| factory reset | wipes NVS, next boot |
| enter recovery | sets the boot-mode flag and reboots into MCUboot serial recovery |
| read build string | which version is running after an upgrade |
| read image hash | the update tool's verdict: the image just sent, or an older one after rollback |
| confirm upgrade | the host confirms this upgrade; without it the image rolls back |
| exit | leaves management mode |

`ENTER` is synchronous: after its successful ACK, the management session is ready and queued
logs have been processed by the logging backends and the file backend has synchronized them.
The idle timeout starts after this flush.
Sending `ENTER` again in an active session flushes pending logs before acknowledging it.

The tool that speaks it is `tools/zlrs.py`; the update flow built on top is in
[07-firmware](07-firmware.md).

Automatic band plans are authenticated as part of SYNC/session confirmation and accepted only in their nonce/sequence context. Changing bands preserves session counters and DATA deduplication; it never rolls keys or resets anti-replay state merely to change frequencies. Both peers must agree on automatic/fixed mode and the active domain.
