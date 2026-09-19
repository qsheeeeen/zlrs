# Documentation index

The documentation has three parts: **design** (the specification: what is being
built and how it is put together), **using it** (build, flash, configure) and
**contributing** (the rules for changing this repository). Design documents own
one kind of fact each and the numbering is the reading order; other documents
link to them instead of repeating them.

Writing rules:

- Describe the **final shape** only. Undecided parts stay blank; no "TBD", no
  drafts. Process and progress belong in [design/13-roadmap.md](design/13-roadmap.md).
- Debugging material lives in [design/12-debug.md](design/12-debug.md) and
  nowhere else.
- Pins, aliases and partition numbers are defined once in the
  [board README](../boards/zlrs/rp2350_zero_core2021/README.md); elsewhere only
  referenced.

## Design

How the repository itself is organised — hardware model, capability macros,
layers, gates — is specified in [design/organization.md](design/organization.md).

### Product design (requirements)

| Document | Scope |
| --- | --- |
| [01-product](design/01-product.md) | positioning, the three link shapes and their scenarios, distance x rate targets, performance figures, boundaries |
| [02-vs-elrs](design/02-vs-elrs.md) | the relationship with ExpressLRS: what it borrows, where it differs |

### Hardware (implementation)

| Document | Scope |
| --- | --- |
| [03-platform](design/03-platform.md) | target hardware shape, board, build targets, toolchain, partitions and flashing |
| [04-prototype](design/04-prototype.md) | today's prototype: two modules plus USB, button faked with a jumper wire |
| [05-carrier](design/05-carrier.md) | the carrier board: USB, button, two 6P serial ports, power |
| [06-radio](design/06-radio.md) | LR2021 parameter summary |

### Software (implementation)

| Document | Scope |
| --- | --- |
| [07-architecture](design/07-architecture.md) | the two links, band roles, time sharing, timing budget, layering, threads |
| [08-firmware](design/08-firmware.md) | ports and endpoints, operating modes, runtime configuration, trimming, error handling |
| [09-protocol](design/09-protocol.md) | frame format, link state machine, sessions, sync, reliable transfer, hopping and regulation |
| [10-security](design/10-security.md) | identity and keys, pairing, threat model, the config protocol |
| [11-ux](design/11-ux.md) | button semantics and the status LED |

### Development

| Document | Scope |
| --- | --- |
| [12-debug](design/12-debug.md) | debug overlay, layered bench switches, on-target debugging |
| [13-roadmap](design/13-roadmap.md) | to-dos and acceptance criteria |

## Using it

Build, flash and configure instructions live in the [README](../README.md);
host tests, style and the rules for changing this repository are in
[organization.md](design/organization.md) section 6.

## Reading paths

- To use it: [README](../README.md) -> [04-prototype](design/04-prototype.md) -> [11-ux](design/11-ux.md)
- To change the firmware: [01-product](design/01-product.md) -> [07-architecture](design/07-architecture.md) -> [08-firmware](design/08-firmware.md) -> [09-protocol](design/09-protocol.md)
- To port it to new hardware: [organization](design/organization.md) -> [03-platform](design/03-platform.md) -> board README
- To understand the trade-offs: [01-product](design/01-product.md) -> [02-vs-elrs](design/02-vs-elrs.md) -> [09-protocol](design/09-protocol.md) -> [10-security](design/10-security.md)
