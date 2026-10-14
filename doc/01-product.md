# Product

ZLRS provides a transparent, bidirectional serial byte stream between two wireless
endpoints. A host or controller supplies bytes through USB CDC or UART; the devices
handle pairing, authenticated sessions, retransmission and delivery without
interpreting the application protocol.

## Roles and radio operation

Both endpoints run the same firmware. One is configured as TX and the other as RX.
TX owns the radio clock and initiates sessions; both roles send and receive user
data.

One LR2021 operates on one DATA band at a time. The available modes are fixed
2.4 GHz, fixed SubG and automatic switching. Automatic mode starts on 2.4 GHz,
coordinates a move to SubG after sustained poor quality, searches both bands after
loss of contact and probes a return to 2.4 GHz after sustained healthy SubG
feedback. Band changes preserve the session and queued, unacknowledged data.
Transmission pauses while the new radio timing is acquired.

The SubG region must match the module's LF or HF hardware. Both endpoints require
matching band mode, SubG region, security profile and air protocol version.

## Interfaces and configuration

USB CDC and UART are alternative endpoints of one DATA service. A separate USB
management interface provides configuration, status, pairing, logs and firmware
updates. USB DATA applies backpressure when buffers fill; UART without hardware
flow control can overflow and reports dropped bytes.

Role, band mode, region, UART baud, power ceiling and security profile are persistent
settings applied at boot. Identity, a shared root key and one peer pairing record
also survive reboot. Sessions, queues and radio observations are volatile.

## Security and delivery

Production profile P1 authenticates frames and rejects replays within the selected
exchange. User data remains plaintext. Each direction uses stop-and-wait ARQ:
received bytes are acknowledged only after the local DATA endpoint accepts the
whole packet. Reliability is bounded by finite buffers and device resets; the
service is not persistent message storage.

Both endpoints report receive quality and adjust their own transmit power within
the configured ceiling. SubG transmission observes the selected regional power,
rate and duty policy. These policies require RF qualification on the finished
hardware.

The prototype consists of a Waveshare RP2350-Zero and Core2021. See
[prototype hardware](02-prototype.md), [firmware operation](06-firmware.md),
[architecture](05-architecture.md), [air protocol](07-protocol.md) and
[security](08-security.md). ZLRS uses its own protocol and is not compatible with
ExpressLRS.
