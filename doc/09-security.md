# Security

The identity model, the key tree, pairing and sessions, the security profiles, the threat model
and the configuration protocol. Frames and states: [08-protocol](08-protocol.md);
implementation: `core/proto/crypto*.c` and `core/link/link.c`.

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

The design targets two levels and explicitly not a third:

- **L1 - a hostile outsider**: someone with a radio can record, replay, forge or jam. P1 exposes
  plaintext and a 32-bit authentication tag. Forgery resistance is limited by that tag length;
  payload encryption requires the unimplemented P2 profile.
- **L2 - a stolen device or a leaked group key**, mitigated by per-frame authentication, per
  session keys and replay protection rather than by secrecy of the format.
- **L3 - an adversary with the root key and physical access** is out of scope: there is no
  forward secrecy (that would need ephemeral key agreement) and anyone who holds `root_key` can
  pair with any device in the group and displace the existing pairing.

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
                                                          ├─ KDF "zlrs/sync/lora/*" ──────► LoRa sync byte (bit 7 = private)
                                                          └─ KDF "zlrs/session", nonces ──► session keys (per direction)
```

- `ordered_ids` and the ordered pairing salt (both sorted numerically) mean both ends derive the
  same `pair_key` regardless of who did what first.
- `link_auth_*` and the session keys are **directional**: the context carries local-then-peer for
  outgoing frames and peer-then-local for incoming ones. One key for both directions would let
  our own frame, echoed back, pass as the peer's (reflection).
- Re-pairing produces new nonces, so the whole subtree below `pair_key` changes.
- LoRa sync words are one byte plus a private bit, derived through their own labels (named after
  the link, not the band).

The protocol core never sees `root_key`: the link layer derives and holds the subtree, and the
configuration protocol hands the root key straight to NVS.

## 4. Pairing

Pairing decides **who** we talk to; the session decides **which key** this conversation uses.

- Pairing runs on the selected DATA band and stores one peer record.
- The window is opened by a 1 s button hold or by the `pair` command and lasts 30 s; `PAIR_REQ`
  repeats every 100 ms.
- The three messages carry `dev_a`, `dev_b` and the two 4-byte nonces. They are tagged with
  `auth_key` and use the pairing sync word.
- **No key is transferred**: pairing authenticates the two device ids and exchanges the nonces
  from which both ends derive `pair_key` ([08-protocol](08-protocol.md) 3.3).
- Both nonces are covered by every message, so a recorded handshake cannot be replayed into a
  new pairing.

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
| `P2` | 4 B tag plus AES-CTR payload encryption (encrypt-then-MAC) | not implemented yet; the last item in [12-roadmap](12-roadmap.md) |

The profile is configuration: `set-profile` stores it, the build carries the default, a
production image refuses `P0`, and the profile a peer declares in `SYNC` has to match the local
one.

## 7. Replay protection

- Every frame carries a `seq` that increases within a session; a frame whose sequence does not
  move forward is dropped and counted.
- Sequence numbers restart at 0 with each session, and a session is only adopted after the
  `SESS_CFM` verifies.
- A `SYNC` that asks for a session change is believed only after recent total silence, so an old
  `SYNC` cannot be replayed inside a live session.
- ARQ retains one outstanding DATA per direction. The authenticated header ACK bit flips on
  each accepted in-order data packet; duplicates repeat the same bit without duplicate delivery.
  Retransmissions retain the inner data sequence and use a fresh outer frame sequence.

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

`core/proto/crypto.c` implements HMAC, HKDF, PBKDF2, root-key derivation and tag comparison
once, without Zephyr headers. Its private `sha256.h` declares the only hash operation it needs:
SHA-256 over two spans in order. Protocol algorithms and parameters are shared by firmware
and host tests.

`app/src/platform/sha256.c` implements that operation through Zephyr's Crypto API and the
SHA peripheral selected by devicetree. Firmware requires ZLRS's driver
(`drivers/crypto/rp2350_sha256.c`, compatible `zlrs,rp2350-sha256`), based on the upstream
RP2350 driver with multipart hashing and session ownership added. Each digest uses
`hash_begin_session()`, `hash_update()`, `hash_compute()` and `hash_free_session()`. HMAC's
key prefix and frame data stay in their original buffers and are fed without a concatenation
copy. A mutex protects complete hash sessions and provides priority inheritance; the driver
rejects access from other contexts while a session owns the accelerator. Pico SDK handles
partial input words and final SHA padding inside the driver. Hashing leaves interrupts enabled
and does not allocate DMA channels; reported driver failures stop execution rather than
producing an authentication tag. There is no PSA protocol backend or runtime backend selection.

Host tests replace only the SHA operation with `tests/host/sha256_soft.c`, linking the same
protocol algorithms. The separate Zephyr Ztest application in `tests/hardware/unit` checks
the real driver and shared algorithms, including multipart SHA-256, wire tags and multi-block
derivation. Test vectors and timing code are excluded from production sources; see
[hardware tests](11-debug.md#7-tests-organized-by-module).

The 16-bit sequence comparison across long sessions, the 32-bit tag budget and explicit
failure reporting for invalid derivation parameters need further work. The current code review
and acceptance gaps are tracked in [12-roadmap](12-roadmap.md).

## 9. Management protocol

The management protocol runs on the **management CDC only**, on a local USB connection; physical
access is the boundary. It is a small framed protocol (`0xA5 0x5A`, version 3, CRC8) with request
and response frames, documented by `include/zlrs/mgmt/mgmt.h` and implemented in `core/mgmt/`.

| Host command | Effect |
| --- | --- |
| read status | role, link state, flags (bit 3: active data24 mode), `dev_id`, `peer_id`, LQ, radio design band, region, security profile, power ceiling, data baud |
| enter management mode | pauses radio work; ACK follows session setup and synchronous log flushing |
| start pairing | opens the pairing window at once |
| erase pairing | effective on the next boot |
| write passphrase | the device derives the root key with PBKDF2 |
| write root key | 64 hex characters |
| set data baud | 9600-4000000, effective on the next boot |
| set region | validated against the board's radio design, refused on mismatch, next boot |
| set power ceiling | never above the chosen region's limit, next boot |
| set role | `tx` or `rx`, next boot |
| set mode | `mixed` or `data24` (exclusive 2.4 GHz data and pairing), next boot |
| set profile | `p0` (debug images only), `p1`, `p2` (not implemented), next boot |
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
[07-firmware](07-firmware.md) 5.
