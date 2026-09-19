# System architecture

How the system is put together: how the two links divide the work, how band roles are configured, how time is shared, how the timing budget is computed, how the code is layered and what the threading and callback contracts are. Product: [01-product](01-product.md); platform: [03-platform](03-platform.md); protocol: [09-protocol](09-protocol.md).

## 1. The two links

| Link | Default band | Modulation | Carries | Optimised for |
| --- | --- | --- | --- | --- |
| Control | sub-GHz | FLRC | CRSF (channels down, telemetry up) | distance and stability |
| Data | 2.4 GHz | FLRC | a passthrough byte stream | rate and throughput |

Each link runs its own copy of the same state machine and they are **independent**: losing one does not touch the other. They do share a radio, an MCU and a supply, so they are **not redundant links**, only mutual backup against interference; a radio or supply failure takes both down.

There is no automatic band failover: calibrating the trigger needs real interference data, and two independent link states already cover the backup need at the interference level.

## 2. Bands and sub-band mixing

Each link has one **cross-sub-band hop table**: 2.4G and sub-G frequencies in the same table, interleaved by the mix ratio `r` (four levels: 0 / 25 / 50 / 100%). The TX decides `r` and carries a table version in `SYNC`; the RX hops on the new table from the next sync frame. In essence it is "a frequency hop, just a bigger one", so no extra rendezvous protocol is needed.

The two links (control and data) time-share the radio and stay independent, each with its own `r`; a single-UART board compiles only one of them.

The migration order is fixed: **telemetry, then data, then the RC downlink**. `r` moves the least critical traffic first and the RC last, so latency steps down instead of jittering.

Sync word derivation labels are named after the **role** (`sync/control`, `sync/data`) rather than the band, so sub-band mixing leaves the key structure untouched ([10-security](10-security.md) 3).

**The regulatory gate only applies to sub-G slots**: 2.4G has no region table. The ceiling for `r` is derived from the regional duty cap (about 10 frames per second at EU868's 1%), and going over it clamps the tier and warns.

The superframe period follows the slower link: data windows amortise into every frame through `data_every_n`, and the time plan works out how far that drags the control rate down.

## 3. Time sharing and scheduling

Rather than writing a scheduler, the two links register as two RAC clients and priorities let the radio planner arbitrate:

| Link | RAC priority | How it is scheduled |
| --- | --- | --- |
| Control | `RAC_HIGH_PRIORITY` | the application orders the superframe, RAC runs `ASAP` |
| Data | `RAC_MEDIUM_PRIORITY` | the slots control leaves idle, `ASAP` |

The application guarantees the slot order: RAC schedules in milliseconds while superframe slots are sub-millisecond, so the application walks the superframe and submits transactions one by one while RAC only has to run them promptly. The superframe shape is in [09-protocol](09-protocol.md) 4.

Both links default to FLRC. Changing modulation (FLRC to LoRa) costs far more on the radio side than changing frequency, so sharing a modulation keeps every switch to a frequency change. **Modulation is per link**: `CONFIG_ZLRS_APP_CONTROL_MODULATION_FLRC` by default, switching to `CONFIG_ZLRS_APP_CONTROL_MODULATION_LORA` (SF7 / BW125 / CR 4/5) when more range is needed; narrow-band regions use 0.325 Mbps FLRC, and the data link always stays on 2.4G FLRC.

## 4. Timing budget and rate tiers

From LR2021 datasheet Rev 1.1 Table 3-23 (typical values); the full table is in [06-radio](06-radio.md):

| Symbol | Meaning | typ |
| --- | --- | --- |
| `PLL_LOCK` | synthesiser lock | 32 us |
| `TSFSTX1` | FS to Tx | 45 us |
| `TSFSRX2` | FS to Rx (FSK/FLRC) | 37 us |
| `TSTXRX` | Tx to Rx | 91 us |
| `TSRXTX` | Rx to Tx | 107 us |
| `TSXORX` | STDBY_XOSC to Rx | 84 us |
| `TSXOTX` | STDBY_XOSC to Tx | 76 us |

One "transmit on sub-G, receive on 2.4G" switch costs about `Tx->FS(54) + PLL(32) + FS->Rx(37) = 123 us`, roughly 250 us for the round trip. At a 150 Hz control rate (6.67 ms per frame) that is about 3.7%.

The front end follows the frequency automatically: `ral_lr20xx_bsp_get_rx_cfg()` picks the HF path at `freq >= 1.6 GHz` and the LF path otherwise, so all we do is fill in a frequency.

### Rate tiers

| Tier | FLRC raw rate | Effective rate (CR 3/4) | Used for |
| --- | --- | --- | --- |
| Close-in, fast | 2.6 Mbps | 1.95 Mbps | 2.4G slots (control at `r=0` and the data link) |
| Balanced | 1.3 Mbps | 0.975 Mbps | the control link while mixing bands |
| Long range | 0.65 Mbps | 0.488 Mbps | the control link stepping down |
| Floor | 0.325 Mbps | 0.244 Mbps | narrow-band regions |

Rough numbers: 40% of a 6.67 ms frame for the 2.4G data link is about 650 bytes per frame, so roughly **97 kB/s**.

## 5. Software layering

```
include/zlrs/proto/   protocol core headers
lib/proto/            protocol core: framing, reassembly, FHSS hop tables,
                      key derivation and tags. On target the crypto primitives go
                      through PSA (lib/proto/crypto_psa.c); the host tests use
                      an equivalent pure C implementation (tests/host/crypto_soft.c)
lib/link/             link layer: sessions and pairing, key derivation and tags,
                      frames into RAC transactions, reliable retransmit and rate adaptation
lib/crsf/             CRSF codec: RC channel frames and the link statistics frame
lib/config/           config protocol: framing and dispatch (storage goes to the app by callback)
lib/region/           region layer: channel plans, power ceilings, duty accounting
lib/led/              status LED engine: state to colour and cadence
lib/usb/              USB device stack: usbd context plus every CDC in devicetree
app/                  Zephyr application: threads and superframe, ports, radio port, button, storage
tests/host/           host tests (no Zephyr dependency)
```

The rule: the protocol core and the link layer both compile and run on the host (the same `lib/` sources); `lib/link/` only depends on the radio abstraction while `app/` depends on Zephyr and USP. Everything on the radio side (SPI timing, calibration, band selection, transfer scheduling) lives in `usp_zephyr`.

Every transmission also passes a **regulatory gate**: `lib/region` provides the region's channel plan, power ceiling and duty cap; a slot with no budget left is given up and counted in the log. See [09-protocol](09-protocol.md) 5.2.

## 6. Threading model and callback contract

### 6.1 Threads

| Thread | Priority | Responsibility |
| --- | --- | --- |
| RAC thread (from USP) | `-4` | runs the radio planner and dispatches transfer callbacks |
| Link thread (`main`) | `-2` | link state machines, building and submitting transactions, handling results |
| Serial | interrupt context | ring-buffered transfers, no radio decisions |

Threads are cooperative (`CONFIG_USP_MAIN_THREAD=y`, `CONFIG_USP_THREADS_MUTEXES=n`): RAC calls go in directly with no mutex cost, and the link logic is I/O driven and yields often. The price is that a long serial burst delays radio events, so serial transfers are cut into small pieces that yield promptly. USP's other two threading modes are described in usp_zephyr's `doc/THREAD_MANAGEMENT.md`.

Config mode does not run on the link thread: the main loop (`main()`) enters it and the radio stops for the session (see [08-firmware](08-firmware.md) 2).

### 6.2 Callback contract

`callback_post_radio_transaction` runs in the RAC thread context and may only:

1. record the `rp_status_t` and the received length;
2. hand the result to the link thread (`k_sem` / `k_msgq`);
3. return.

It does not validate, does not walk the protocol stack, does not touch the serial ports and **does not submit the next transaction**: under preemption plus mutexes that would re-lock `rac_api_mutex`, a path the vendor samples never cover.

### 6.3 Integration constraints with USP

The application initialises RAC itself: `SMTC_SW_PLATFORM_INIT()` only waits for the USP thread and never calls `smtc_rac_init()`; the vendor samples call it from the application too. Since USP v1.1.2 `usp_zephyr` registers the log module itself, so the application no longer needs `LOG_MODULE_REGISTER(usp, ...)`.

## 7. Diagnostics

Diagnostics **add no extra channel**: link quality rides the existing CRSF link statistics frame (see [09-protocol](09-protocol.md) 2.7). The data link's overflow and dropped bytes have no CRSF field to travel in, so they are not reported over the air, only counted in the config protocol's `STATUS`.
