# System architecture

How the system is put together: what the two links do, how the bands mix, how the radio is
shared, how the timing budget is spent, how the code is layered and what the threading and
callback contracts are. Product: [01-product](01-product.md); platform:
[03-platform](03-platform.md); protocol: [09-protocol](09-protocol.md).

## 1. The two links

| Link | Default band | Modulation | Carries | Optimised for |
| --- | --- | --- | --- | --- |
| Control | sub-GHz | FLRC (or LoRa) | CRSF: channels down, telemetry up, pairing | distance and stability |
| Data | 2.4 GHz | FLRC | a passthrough byte stream | rate and throughput |

Each link runs its own copy of the same state machine, and they are **independent**: losing one
does not touch the other. They do share a radio, an MCU and a supply, so they are not redundant
links, only mutual backup against interference; a radio or supply failure takes both down.

There is no automatic band failover between roles at runtime: the band role is a build-time
choice (long range or racing), and the sub-band mix below is what adapts.

## 2. Bands and sub-band mixing

Each link has one **cross-sub-band hop table**: 2.4G and sub-G frequencies in the same table,
split by the mix ratio `r` (four levels: 0 / 25 / 50 / 100 %). The TX decides `r`, carries it
plus a table version in `SYNC`, and the RX adopts both while handling that frame, so nobody is
half on the old table. In essence it is "a frequency hop, just a bigger one", so no extra
rendezvous protocol is needed.

The two links time-share the radio and stay independent, each with its own `r`. A single-port
board carries one of them, chosen by configuration.

The migration order is fixed: **telemetry, then data, then the RC downlink**. `r` moves the
least critical traffic first and the RC last, so latency steps down instead of jittering.
Sync word derivation labels follow the link (`sync/control`, `sync/data`, `sync/pairing`) rather
than the band, so mixing leaves the key structure untouched ([10-security](10-security.md) 3).

**The regulatory gate only applies to sub-G slots**: 2.4G has no region table. The ceiling for
`r` comes from the regional duty cap (about 10 frames per second at EU868's 1 %), and going
over it clamps the tier and warns.

The superframe period follows the slower link: data windows amortise into every frame through
`data_every_n`, and the time plan works out how far that drags the control rate down.

## 3. Time sharing and scheduling

Both links share one radio owner on the link thread. USP supplies RAL, the LR20xx driver and
Zephyr SPI/GPIO support; RAC and the radio planner are not initialized or run.

RP2350 TIMER1 runs at 1 MHz. Alarm 0 advances the four absolute slot boundaries and publishes
the latest frame; alarm 1 wakes the owner for preparation; alarm 2 ends the radio window and
publishes ordinary completion notifications. Alarm 3 issues the prepared five-byte `SET_TX`
from a RAM-resident zero-latency ISR, bypassing kernel interrupt locks. A delayed thread skips
expired slots and advances hop/frame state without building or transmitting old packets.
Frame snapshots consume any pending wake token under the same interrupt lock, and the owner
rejects an already consumed frame number. Management pauses radio work while the absolute clock keeps running, so missed frames can be
accounted for when it resumes. The superframe shape is in [09-protocol](09-protocol.md) 4.

The TX is the clock master. The RX follows authenticated packet timestamps captured by the
GPIO ISR, subtracting packet airtime and the TX launch offset. Corrections are bounded and
applied to a future frame boundary; only RX scans its phase when it has heard nothing.

Both links default to FLRC. Changing modulation (FLRC to LoRa) costs far more on the radio side
than changing frequency, so the data link always stays on 2.4G FLRC while the control link may
move to LoRa for range (`CONFIG_ZLRS_APP_CONTROL_MODULATION_*`, SF7 / BW125 / CR 4/5 by
default). The superframe then follows the slower link.

## 4. Timing budget and rate tiers

From LR2021 datasheet Rev 1.1 Table 3-23 (typical values); the full table is in
[06-radio](06-radio.md):

| Symbol | Meaning | typ |
| --- | --- | --- |
| `PLL_LOCK` | synthesiser lock | 32 us |
| `TSFSTX1` | FS to Tx | 45 us |
| `TSFSRX2` | FS to Rx (FSK/FLRC) | 37 us |
| `TSTXRX` | Tx to Rx | 91 us |
| `TSRXTX` | Rx to Tx | 107 us |
| `TSXORX` | STDBY_XOSC to Rx | 84 us |
| `TSXOTX` | STDBY_XOSC to Tx | 76 us |

One "transmit on sub-G, receive on 2.4G" switch costs about `Tx->FS(54) + PLL(32) + FS->Rx(37) =
123 us`, roughly 250 us for the round trip. At a 150 Hz control rate (6.67 ms per frame) that is
about 3.7 %.

The front end follows the frequency automatically: `ral_lr20xx_bsp_get_rx_cfg()` picks the HF
path at `freq >= 1.6 GHz` and the LF path otherwise, so all we do is fill in a frequency.

### Rate tiers

| Tier | FLRC raw rate | Effective rate (CR 3/4) | Used for |
| --- | --- | --- | --- |
| Close-in, fast | 2.6 Mbps | 1.95 Mbps | 2.4G slots (control at `r=0` and the data link) |
| Balanced | 1.3 Mbps | 0.975 Mbps | the control link while mixing bands |
| Long range | 0.65 Mbps | 0.488 Mbps | the control link stepping down |
| Floor | 0.325 Mbps | 0.244 Mbps | narrow-band regions |

LoRa is the tier below those: SF5-12, BW 125/250/500 kHz, CR 4/5-4/8, at tens of milliseconds per
frame. The previous approximately **97 kB/s** estimate was an airtime-only target, not measured
throughput. The current implementation has four equal slots and one packet per data direction
per frame. Its timer period includes native packet airtime, preparation guard and RF startup;
LF is clamped from the configured preparation window and RF budget. The SPI DMA default
reserves 6000 us for preparation and currently runs CN433 at 27 Hz. ARQ and ACK traffic
reduce the payload rate further. Hardware results and remaining acceptance are tracked in
[13-roadmap](13-roadmap.md).

## 5. Software layering

```
include/zlrs/proto/    protocol core headers (frame, hop tables, fragmentation, telemetry, crypto)
core/proto/            protocol core: framing, hop tables, key derivation, tags, airtime model
core/link/             link layer: state machine, pairing and sessions, superframe slots,
                       reliable transfer, rate adaptation and dynamic power
core/crsf/             CRSF codec: RC channels and the link statistics frame
core/mgmt/           management protocol: framing, command dispatch, status encoding
core/region/           region tables, power and duty limits, the control time plan
core/led/              status LED engine: state to colour and cadence
include/zlrs/          public headers for everything in core/
subsys/usb/            USB device stack: usbd context plus every CDC in devicetree
subsys/bootguard/      bootloader-side watchdog handling
drivers/entropy/       the RP2350 TRNG driver we own
app/src/               Zephyr application, one directory per layer:
  main.c                 the application: superframe, link wiring, reporting, debug shell
  platform/              hardware capabilities, RP2350 slot timer, LR2021 driver, watchdog and recovery
  link/                  the links' I/O and tuning: data (passthrough), CRSF, rate
                         adaptation, dynamic power
  mgmt/                  management CDC, session and device commands in mode.c
  state/                 system state broadcast (the zbus channels)
  ui/                    what the user sees: the status LED (state to frame to strip) and the
                         button (its own thread, hold reported on the bus)
  storage/               the flash-backed state: NVS settings and the log ring
boards/                board definitions (devicetree, defconfig, README)
dts/bindings/          bindings for ZLRS-specific nodes
tools/                 host-side helpers (management, recovery, update, throughput, UF32 packaging)
tests/host/            host build of core/ plus the two-node radio simulation
```

The rule: the protocol core and the link layer build and run on the host (the same `core/`
sources, crypto through a soft implementation in `tests/host/crypto_soft.c`); on target the
crypto primitives go through Zephyr's PSA (`core/proto/crypto_psa.c`). `core/` includes no
Zephyr headers, and `app/` never branches on a board name — both are CI gates (`tools/gates.sh`,
see the README's "Gates" section).

The application binds directly to Zephyr and RP2350 for rapid prototyping. There is no
`ports/` layer: `link/data.c` and `link/crsf.c` own their devicetree UART/CDC devices, endpoint
selection, rings and interrupts; `mgmt/mode.c` owns the dedicated management CDC and session.
`platform/radio.c` directly uses the USP RAL / LR20xx driver, while `platform/slot_timer.c`
uses RP2350 TIMER1. The main loop executes requests returned by the portable protocol core;
there is no radio ops table or submit adapter. Only nonce generation is injected into the core.

USP supplies the transceiver RAL and Zephyr HAL. The application owns preparation,
calibration selection and launch scheduling.

Every transmission also passes a **regulatory gate**: `core/region` provides the region's
channel plan, power ceiling and duty cap; a slot with no budget left is given up and counted in
the log. See [09-protocol](09-protocol.md) 5.2.

## 6. Threading model and callback contract

### 6.1 Threads

| Thread | Priority | Responsibility |
| --- | --- | --- |
| Link thread (`main`) | highest cooperative priority, meta-IRQ in data24; `-2` otherwise | link state machines, radio preparation/completion, system state broadcast; management temporarily uses `-2` |
| USB core / controller | upstream cooperative priorities | USB events; vendor source and scheduling are unchanged |
| LED thread (`zlrs_led`) | `7` (preemptive) | renders the system state it subscribes to and writes the WS2812 (`app/src/ui/led.c`) |
| Button thread (`zlrs_button`) | `8` (preemptive) | samples the button, reports a hold on the bus (`app/src/ui/button.c`) |
| Serial | hardware ISR or upstream CDC workqueue | ring-buffered transfers, no radio decisions |
| Serial notifications | system workqueue | enables peripheral TX/RX outside the RF thread |

The link thread owns radio preparation and completion. Data24 resumes at DMA, radio and timer
events even when a cooperative USB worker is active. Serial peripheral notifications use the
system workqueue; CDC callbacks remain the only callers of CDC FIFO operations. Only the
prepared TX command is handed to alarm 3; the owner waits without touching SPI until the ISR
publishes its SRAM result and alarm 2 wakes it. `CONFIG_USP_MAIN_THREAD=n` and
`CONFIG_LORA_BASICS_MODEM_DRIVERS_EVENT_TRIGGER_NO_THREAD=y`: the GPIO callback only timestamps the IRQ and
wakes the owner, which reads status and completes the link transaction. Serial work is bounded
between frames. Flash operations can still delay IRQ handling; delayed boundaries are counted
and the absolute schedule is preserved.

The LED thread keeps the PIO push and WS2812 latch delay out of radio work. It runs while the
link thread is blocked and pushes a frame only when the colour changes.

What the link thread hands over is not a colour but the **system state**, one zbus channel per fact
(`app/src/state/sys_bus.h`): the control link's state and quality, the alerts the links produce,
and an open management session. Three rules make that cheap and safe:

- a publisher writes only when the message changed, so the bus carries events, not a heartbeat,
  and the LED thread's own 20 Hz tick drives the blink phase;
- what the user asked for rides the same bus (a hold of the button), so the link state machine
  stays on the link thread while the button thread owns the timing;
- every channel has one owner, because a publisher writes the whole message: two writers on one
  channel would each blank the other's field;
- publishing costs a channel copy and a notification push, both `K_NO_WAIT`, so the superframe
  never waits for a consumer.

The consumer decides what the facts mean — `core/led` holds the colours and the priority order
(pure C, host-tested) and the LED thread is its only caller, so adding a consumer (a shell, a log
exporter, telemetry) never touches a producer. See [11-ux](11-ux.md).

Management mode does not run on the link thread either: `main()` enters it, stops submitting
transactions for the session and runs the management protocol there (see
[08-firmware](08-firmware.md) 2).

### 6.2 Callback contract

The radio GPIO callback records the TIMER1 timestamp, sets a pending flag and gives a semaphore.
It never reads SPI, validates a packet or submits another transaction. The link thread drains
the IRQ, reads the FIFO and invokes `zlrs_link_complete()`.

### 6.3 Integration constraints with USP

The application instantiates its own `ralf_t` from the chosen transceiver device, then calls
`ral_reset()` and `ral_init()`. It attaches the GPIO callback through the transceiver board API.
USP initialization of RAC, task submission and runtime backend switching are absent. Both links
share cached modem parameters and stay in standby/FS between transactions. Every TX clears its
FIFO before writing: an aborted packet must not contaminate the next packet.

Slot execution has two stages. Before the nominal boundary, the link thread constructs and
authenticates the packet, configures the modem and frequency, loads the TX FIFO and enters FS.
It then waits for the hardware launch alarm. Only `SET_TX` runs at the configured offset;
receivers start listening as soon as their preparation finishes. The previous RX window ends
before this preparation reservation, so early FIFO loading never overlaps an active reception.
The frame alarm publishes the next frame early; protocol decisions still use the planned slot
time and phase correction is included when calculating the final slot deadline.

The final prepared TX command runs in the RAM-resident alarm 3 ISR. It uses only TIMER1,
GPIO and PL022 registers plus its SRAM mailbox, with a 50 us bounded polling transfer. It
calls no kernel, RAL, DMA or logging API and can preempt USB interrupts and kernel IRQ locks.
SPI DMA still performs configuration, FIFO transfers and status reads. Alarm 3 is armed only
after those transfers finish; the owner performs no SPI access until launch completes. The
ISR checks BUSY and controller state before asserting CS and rejects a trigger over 100 us
late. Alarm 2 performs the subsequent kernel notification. Authentication, configuration,
FIFO access, IRQ draining and protocol callbacks stay on the link thread.

The board calibration table contains exactly three entries because the vendor BSP consumes
three; HF pads its unused entry with zero, and LF includes both sub-G coverage and 2.4 GHz.

## 7. Diagnostics

Diagnostics **add no extra channel**: link quality rides the existing CRSF link statistics frame
(`0x14`, see [09-protocol](09-protocol.md) 2.4). The data link's overflow and dropped bytes have
no CRSF field to travel in, so they are counted and exposed through the management protocol's
`STATUS` response instead. Debug images can additionally print a 1 Hz `stats:` / `air:` line
(`CONFIG_ZLRS_APP_DEBUG_STATS_LOG`).
