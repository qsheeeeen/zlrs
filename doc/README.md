# Documentation index

The documentation has four parts: **product design** (requirements: what to build and how far to take it), **hardware** and **software** (the implementation), and **development** (debugging and planning). The numbering is the reading order; every document owns one kind of fact and other places link to it. How the repository itself is organised — hardware model, capability macros, layers, gates — is in [organization](organization.md).

Writing rules:

- Describe the **final shape** only. Undecided parts stay blank; no "TBD", no drafts. Process and progress belong in [13-roadmap](13-roadmap.md).
- Written for users: debugging material lives in [12-debug](12-debug.md) and nowhere else.
- Pins, aliases and partition numbers are defined once in the [board README](../boards/zlrs/rp2350_zero_core2021/README.md); elsewhere only referenced.

## 1. Product design (requirements)

| Document | Scope |
| --- | --- |
| [01-product](01-product.md) | positioning, the three link shapes and their scenarios, distance x rate targets, performance figures, boundaries |
| [02-vs-elrs](02-vs-elrs.md) | the relationship with ExpressLRS: what it borrows, where it differs |

## 2. Hardware (implementation)

| Document | Scope |
| --- | --- |
| [03-platform](03-platform.md) | target hardware shape, board, build targets, toolchain, partitions and flashing |
| [04-prototype](04-prototype.md) | today's prototype: two modules plus USB, button faked with a jumper wire |
| [05-carrier](05-carrier.md) | the carrier board: USB, button, two 6P serial ports, power |
| [06-radio](06-radio.md) | LR2021 parameter summary |

## 3. Software (implementation)

| Document | Scope |
| --- | --- |
| [07-architecture](07-architecture.md) | the two links, band roles, time sharing, timing budget, layering, threads |
| [08-firmware](08-firmware.md) | ports and endpoints, operating modes, runtime configuration, trimming, error handling |
| [09-protocol](09-protocol.md) | frame format, link state machine, sessions, sync, reliable transfer, hopping and regulation |
| [10-security](10-security.md) | identity and keys, pairing, threat model, the config protocol |
| [11-ux](11-ux.md) | button semantics and the status LED |

## 4. Development

| Document | Scope |
| --- | --- |
| [12-debug](12-debug.md) | debug overlay, layered bench switches, on-target debugging |
| [13-roadmap](13-roadmap.md) | to-dos and acceptance criteria |
| [organization](organization.md) | repository layout, hardware model, capability macros, rules for changing the tree |

## Reading paths

- To use it: [README](../README.md) -> [04-prototype](04-prototype.md) -> [11-ux](11-ux.md)
- To change the firmware: [01-product](01-product.md) -> [07-architecture](07-architecture.md) -> [08-firmware](08-firmware.md) -> [09-protocol](09-protocol.md)
- To port it to new hardware: [organization](organization.md) -> [03-platform](03-platform.md) -> board README
- To understand the trade-offs: [01-product](01-product.md) -> [02-vs-elrs](02-vs-elrs.md) -> [07-architecture](07-architecture.md) -> [09-protocol](09-protocol.md) -> [10-security](10-security.md)
