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

Rather than writing a scheduler, the two links register as two RAC clients and priorities let
the radio planner arbitrate:

| Link | RAC priority | How it is scheduled |
| --- | --- | --- |
| Control | `RAC_HIGH_PRIORITY` | the application orders the superframe, RAC runs `ASAP` |
| Data | `RAC_MEDIUM_PRIORITY` | the slots control leaves idle, `ASAP` |

The application guarantees the slot order: RAC schedules in milliseconds while superframe slots
are sub-millisecond, so the application walks the superframe and submits transactions one by one
while RAC only has to run them promptly. The superframe shape is in
[09-protocol](09-protocol.md) 4.

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
frame. Rough numbers for FLRC: 40 % of a 6.67 ms frame for the 2.4G data link is about 650 bytes
per frame, so roughly **97 kB/s**.

## 5. Software layering

```
include/zlrs/proto/    protocol core headers (frame, hop tables, fragmentation, telemetry, crypto)
core/proto/            protocol core: framing, hop tables, key derivation, tags, airtime model
core/link/             link layer: state machine, pairing and sessions, superframe slots,
                       reliable transfer, rate adaptation and dynamic power
core/crsf/             CRSF codec: RC channels and the link statistics frame
core/config/           config protocol: framing, command dispatch, status encoding
core/region/           region tables, power and duty limits, the control time plan
core/led/              status LED engine: state to colour and cadence
include/zlrs/          public headers for everything in core/
subsys/usb/            USB device stack: usbd context plus every CDC in devicetree
subsys/bootguard/      bootloader-side watchdog handling
drivers/entropy/       the RP2350 TRNG driver we own
app/                   Zephyr application: capabilities, ports, radio adaptation, NVS,
                       watchdog, button, system state bus, LED thread, superframe, reporting,
                       debug shell
boards/                board definitions (devicetree, defconfig, README)
dts/bindings/          bindings for zlrs-specific nodes
tools/                 host-side helpers (config, recovery, update, bring-up, UF32 packaging)
tests/host/            host build of core/ plus the two-node radio simulation
```

The rule: the protocol core and the link layer build and run on the host (the same `core/`
sources, crypto through a soft implementation in `tests/host/crypto_soft.c`); on target the
crypto primitives go through Zephyr's PSA (`core/proto/crypto_psa.c`). `core/` includes no
Zephyr headers, and `app/` never branches on a board name — both are CI gates (`tools/gates.sh`,
see the README's "Gates" section).

Everything on the radio side (SPI timing, calibration, band selection, transfer scheduling)
lives in `usp_zephyr`.

Every transmission also passes a **regulatory gate**: `core/region` provides the region's
channel plan, power ceiling and duty cap; a slot with no budget left is given up and counted in
the log. See [09-protocol](09-protocol.md) 5.2.

## 6. Threading model and callback contract

### 6.1 Threads

| Thread | Priority | Responsibility |
| --- | --- | --- |
| RAC thread (from USP) | `-4` | runs the radio planner and dispatches transfer callbacks |
| Link thread (`main`) | `-2` | link state machines, building and submitting transactions, results, system state broadcast |
| LED thread (`zlrs_led`) | `7` (preemptive) | renders the system state it subscribes to and writes the WS2812 (`app/src/led_thread.c`) |
| Serial | interrupt context | ring-buffered transfers, no radio decisions |

Threads are cooperative (`CONFIG_USP_MAIN_THREAD=y`, `CONFIG_USP_THREADS_MUTEXES=n`): RAC calls
go in directly with no mutex cost, and the link logic is I/O driven and yields often. The price
is that a long serial burst delays radio events, so serial transfers are cut into small pieces
that yield promptly. USP's other threading modes are described in usp_zephyr's
`doc/THREAD_MANAGEMENT.md`.

The LED thread is the one preemptive thread the application adds, and it is a deliberate exception
to "the link thread does everything": the strip write (PIO push plus the WS2812 latch delay) is the
slowest step of the LED path, and the rest of the LED path is pure C that has no business
stretching a superframe slot. Being preemptive it can only run while the link thread is blocked —
a cooperative thread is not preempted by it — and it pushes a frame only when the colour changed,
so the strip is written at the rate the display actually moves.

What the link thread hands over is not a colour but the **system state**, one zbus channel per fact
(`app/src/sys_bus.h`): the control link's state and quality, the alerts the links produce, and an
open config session. Three rules make that cheap and safe:

- a publisher writes only when the message changed, so the bus carries events, not a heartbeat,
  and the LED thread's own 20 Hz tick drives the blink phase;
- every channel has one owner, because a publisher writes the whole message: two writers on one
  channel would each blank the other's field;
- publishing costs a channel copy and a notification push, both `K_NO_WAIT`, so the superframe
  never waits for a consumer.

The consumer decides what the facts mean — `core/led` holds the colours and the priority order
(pure C, host-tested) and the LED thread is its only caller, so adding a consumer (a shell, a log
exporter, telemetry) never touches a producer. See [11-ux](11-ux.md).

Config mode does not run on the link thread either: `main()` enters it, stops submitting
transactions for the session and runs the config protocol there (see
[08-firmware](08-firmware.md) 2).

### 6.2 Callback contract

`callback_post_radio_transaction` runs in the RAC thread context and may only:

1. record the `rp_status_t` and the received length;
2. hand the result to the link thread (`k_sem` / `k_msgq`);
3. return.

It does not validate, does not walk the protocol stack, does not touch the serial ports and
**does not submit the next transaction**: doing so is a path the vendor samples never cover.

### 6.3 Integration constraints with USP

The application initialises RAC itself: `SMTC_SW_PLATFORM_INIT()` only waits for the USP thread
and never calls `smtc_rac_init()`; the vendor samples call it from the application too. Since
USP v1.1.2 `usp_zephyr` registers the log module itself, so the application does not.

## 7. Diagnostics

Diagnostics **add no extra channel**: link quality rides the existing CRSF link statistics frame
(`0x14`, see [09-protocol](09-protocol.md) 2.4). The data link's overflow and dropped bytes have
no CRSF field to travel in, so they are counted and exposed through the config protocol's
`STATUS` response instead. Debug images can additionally print a 1 Hz `stats:` / `air:` line
(`CONFIG_ZLRS_APP_DEBUG_STATS_LOG`).
