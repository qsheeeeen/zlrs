# Security policy

## What is in scope

- The over-the-air protocol: frame authentication, pairing, session keys,
  replay protection, key derivation (`docs/design/09-protocol.md`,
  `docs/design/10-security.md`).
- The configuration channel and the update path (A/B slots, rollback, image
  confirmation).
- The host tools in `scripts/` when they handle key material or images.

Out of scope, by design, and documented as such:

- `P0` builds carry no authentication at all (maximum performance profile,
  bench and racing only).
- Anyone with physical access to the USB port may configure the device, which
  includes writing a root key.
- No protection against an attacker who already knows the group's root key
  (see the adversary model in `docs/design/10-security.md` section 7).

## Reporting

Use GitHub's private vulnerability reporting on this repository
(<https://github.com/qsheeeeen/zlrs/security/advisories/new>). Please do not open
a public issue for anything exploitable.

Include what you did, what you expected, what happened, and — if you have one —
a packet capture or a reproducer. A capture of the pairing or session handshake
is the most useful single artefact.

## What to expect

- An acknowledgement within about a week.
- An assessment: confirmed, needs more information, or out of scope with the
  reasoning. Design limitations listed above are not vulnerabilities.
- A fix and a release note when confirmed. Credit in the advisory if you want
  it; hardware-bound issues may need time to reproduce on a board.

This is a hobby-scale project without a security team or a bug bounty. The
protocol is small and the crypto is deliberately one primitive family
(SHA-256, via Zephyr's PSA Crypto), which is where reports are most useful.
