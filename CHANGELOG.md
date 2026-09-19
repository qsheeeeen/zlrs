# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Dates are added when
a release is tagged.

## [Unreleased]

Nothing yet: the entries below have not been released.

## [0.1.0] - unreleased

First working version. No release artifact or tag yet; the version string is
baked into the firmware and reported over the config protocol (`build`).

### Added

- Point-to-point link over a single Semtech LR2021: a control link (CRSF in and
  out, telemetry back) and a data link (serial passthrough), time-sharing one
  radio and independent of each other.
- Dual-band hopping: one hop table spanning sub-GHz and 2.4 GHz, mixed by a
  ratio the transmitter chooses and the receiver follows; regional duty
  accounting, power ceilings and SKU checks in `core/region`.
- Pairing by long press (three-way handshake with per-message MACs), session
  setup with nonces from both ends, directional per-frame HMAC tags, replay
  protection, and a compile-time `P0` profile with no authentication for racing.
- Reliable data transfer (ARQ): cumulative acknowledgements, a four-frame
  window, retransmits, in-order delivery and gap counting.
- Runtime configuration over a dedicated USB CDC: region, power ceiling, baud
  rate, failsafe values, pairing, root key or passphrase, factory reset.
- A/B firmware updates over USB: MCUboot serial recovery writes the inactive
  slot, the host confirms the running image by hash, and anything unconfirmed
  rolls back.
- Host tests for the pure C core (unit tests plus a two-node radio simulation)
  with ASan and UBSan, and a firmware build matrix covering both sub-G SKUs, the
  bench-switch combinations, single-link cuts and MCUboot.
- Board `rp2350_zero_core2021` in HF (850-930 MHz) and LF (410-510 MHz)
  variants: Waveshare RP2350-Zero plus Core2021, three USB CDCs, two UARTs,
  button, WS2812 status LED.
- An RP2350 TRNG entropy driver that keeps interrupts enabled (the Zephyr one
  busy-waits with interrupts masked and stalls USB).

[Unreleased]: https://github.com/qsheeeeen/zlrs/commits/main
