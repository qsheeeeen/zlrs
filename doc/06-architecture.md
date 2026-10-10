# System architecture

ZLRS has one bidirectional DATA link. The selected band is 2.4 GHz or the board's sub-GHz band. Both roles share the same link implementation, frame format and serial-data API. Sub-GHz remains part of the design so a later scheduler can switch bands or run two DATA radios without changing the host-facing data port.

## Ownership

| Owner | Responsibility |
| --- | --- |
| `core/proto` | Frame encoding, integrity, authentication and protocol limits; no Zephyr dependency. |
| `core/link` | Pairing, sessions, hopping, DATA ARQ, receive-quality feedback and link state; no hardware dependency. |
| `core/region` | Region channels, power limits, sub-band tables and duty accounting. |
| `core/mgmt` | Versioned management wire protocol and command dispatch. |
| `app/src/rf` | Sole radio/link owner: one DATA instance, slot scheduling, regional gate, dynamic TX-power policy and diagnostics. |
| `app/src/data` | UART/USB DATA endpoints and byte queues; calls the RF business interface. |
| `app/src/config` | Stored settings, identity and pairing records; applies boot-time configuration. |
| `app/src/mgmt` | Management CDC, settings and update commands; enters a bounded session that pauses the radio. |
| `app/src/platform` | Board capabilities and hardware adapters. |
| `app/src/ui` | Button and LED presentation of published state. |
| `app/src/runtime` | Startup and owner-loop integration. |

`core/` uses plain C and is host-testable. The application binds it to Zephyr, the LR2021 radio, flash and CDC/UART devices. RF keeps the link object and keys private. Other modules use narrow functions or copied status values. Each publisher owns its zbus channel; there is no central mutable state bus.

## DATA scheduling

Each frame has alternating DATA A and DATA B opportunities for TX and RX. The selected band determines modulation and airtime. One `zlrs_link_t` produces radio requests; the RF owner executes them on slot boundaries. The link can send data, acknowledgments, quality feedback, pairing, session setup and sync. When idle it sends a small feedback frame so dynamic power does not depend on application traffic.

The DATA band is persisted and applied on reboot. Region and board SKU are validated before transmission. On sub-G, the RF owner accounts for airtime and defers transmissions when the duty budget is exhausted. The configured power is a ceiling; the DATA feedback policy may lower the active setpoint, and restores the ceiling when feedback stops.

## Concurrency and diagnostics

The radio, link and configuration calls run on the RF owner thread. USB/UART interrupt and workqueue handlers only move bounded bytes to their module queues. Management pauses the RF owner at a safe boundary before settings, flash writes or firmware recovery. LED and management read copied facts, not link storage.

Host simulation covers pairing, sessions, bidirectional ARQ, loss, backpressure and feedback. Hardware validation uses both CDC data ports and the management interface. Debug images expose link, rate, radio and power reports; production images retain status and logs. See [development and debugging](11-debug.md).
