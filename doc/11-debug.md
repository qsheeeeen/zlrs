# Development and debugging

For developers only: how to build a debug image, which switches exist to isolate a layer, and
how to chase problems on real hardware. User-facing documents never contain this material.

## 1. Debug image

A debug image is a normal build plus two files:

| File | Contents |
| --- | --- |
| `app/debug.conf` | sets `CONFIG_ZLRS_APP_DEBUG=y`, then enables the console CDC, the shell, deferred logging, asserts and the watchdog |
| `app/debug.overlay` | adds a console CDC (`cdc_acm_uart3`) to the devicetree and points `zephyr,console` / `zephyr,shell-uart` at it |

```shell
west build --sysbuild -b "$ZLRS_BOARD" "$ZLRS_APP" -- \
  -DEXTRA_CONF_FILE="$ZLRS_REPO_ROOT/app/debug.conf" \
  -DDTC_OVERLAY_FILE="$ZLRS_REPO_ROOT/app/debug.overlay"
```

Production firmware has no console CDC and no shell; its three CDCs are control, data and
management (see [07-firmware](07-firmware.md) 1).

The debug console prints command replies. Deferred logs stay in RAM during RF and are flushed
to flash at boot or management entry; export them with `tools/zlrs.py logs <directory>`. UART
and shell log backends are disabled so an unopened terminal cannot block log processing.
Console TX uses polling with an 8 KiB CDC FIFO, enough for complete `zlrs link`, `zlrs ports`
and `zlrs radio` replies. It does not wait for an unopened host; output exceeding that FIFO
can be dropped, so export files through management rather than printing them to the shell.

`CONFIG_ZLRS_APP_DEBUG` is the marker for "this image is for the bench": the options named
`ZLRS_APP_DEBUG_*` depend on it, and shipped configurations (`app/prj.conf`, everything under
`boards/`) may not mention it. `tools/gates.sh` enforces that, so a bench-only switch cannot
quietly end up in a released image.

## 2. Debug entries

| Entry | How to use it |
| --- | --- |
| shell `zlrs status` | role, band, region, link states, data port state |
| shell `zlrs ports` | per-endpoint `ready / active / rx_total / rx_dropped` for the data port |
| shell `zlrs mgmt` | enter management mode (EXIT returns) |
| shell `zlrs recovery` | reboot into MCUboot serial recovery |
| shell `zlrs entropy` | entropy self-test (two 32-byte TRNG blocks) |
| `tests/hardware/crypto` | separate Ztest firmware for SHA-256, HMAC tags, HKDF/PBKDF2 and aligned/unaligned tag timing; see section 7 |
| shell `zlrs mcuboot` | image state as MCUboot sees it (slot headers, confirm bit, copy state) |
| `CONFIG_ZLRS_APP_DEBUG_STATS_LOG` | 1 Hz `stats:` line (counters, LQ, duty, dynamic power) and `air:` line (completed transactions per link) |
| `CONFIG_ZLRS_APP_BENCH_RAW_MS` | the first N ms after boot skip the superframe: the TX role sends complete frames back to back, the RX role opens 100 ms receive windows back to back, and both log frames per second |
| `CONFIG_ZLRS_APP_DEBUG_BOOT_DELAY_MS` | wait that long after USB is up before continuing init, so a panic during init is visible on an open console |
| `CONFIG_ZLRS_APP_DEBUG_LED_BRIGHTNESS_PERCENT` | attenuate the status pixel in a debug image (the default is 20%); production images keep the normal LED levels |
| `CONFIG_ZLRS_APP_TX_GUARD_US` | fixed delay from hardware slot boundary to SET_TX; both peers must use the same timing settings |

The shell is gated by `CONFIG_ZLRS_APP_DEBUG_SHELL` and runs on the console CDC; the supported
configuration paths are in [09-security](09-security.md) 9 and [07-firmware](07-firmware.md) 3.

## 3. Switches for isolating a layer

Do not bring everything up at once: turn one layer off at a time, and if the rest still works
the fault is not in that layer. Every disabled isolation switch prints an `isolation:` warning at
boot, and each has a matching host test.

**Isolation (debug images only — a production configuration cannot reach them):**

| Switch | With it off | Host test |
| --- | --- | --- |
| `CONFIG_ZLRS_APP_SECURITY_PROFILE_P0=y` | frames carry no tag, so timing and frame format come first | `link: isolation without tag (P0)` |
| `CONFIG_ZLRS_APP_FHSS=n` | everything stays on the sync channel | `link: isolation without hopping` |
| `CONFIG_ZLRS_APP_SESSION_HANDSHAKE=n` | the pairing key becomes the session key | `link: isolation without session handshake` |
| `CONFIG_ZLRS_APP_TELEMETRY=n` | the control link carries downlink RC only (the TX peer-loss check goes too) | `link: isolation without telemetry` |
| `CONFIG_ZLRS_APP_DATA_ARQ=n` | the data link falls back to best-effort | `link: arq *`, `link: backpressure` |
| `CONFIG_ZLRS_APP_SUB_BAND_MIX=n` | one hop table, no sub-G migration | `link: sub-band mix *` |

**Shape (any image may set these — they cut the product):**

| Switch | Effect |
| --- | --- |
| `CONFIG_ZLRS_APP_LINK_CONTROL=n` / `ZLRS_APP_LINK_DATA=n` | one link only (compile check; both SKUs are in CI) |
| `CONFIG_ZLRS_APP_BAND_ROLE_*` | control on sub-G (long range) or on 2.4G (racing) |
| `CONFIG_ZLRS_APP_CONTROL_MODULATION_LORA` | LoRa control tier instead of FLRC |

**Features whose default is still open** (rate adaptation, dynamic power) are not
switches to hide but debts: `doc/12-roadmap.md`, "Pending defaults".

For a fixed-channel 2.4 GHz data-only bench on the LF module:

```sh
source tools/zephyr-env.sh
ZLRS_BOARD=rp2350_zero_core2021/rp2350a/m33/lf zlrs-build -d build-data24 -- \
  -DEXTRA_CONF_FILE="$ZLRS_REPO_ROOT/app/debug.conf;$ZLRS_REPO_ROOT/app/data24-debug.conf" \
  -DDTC_OVERLAY_FILE="$ZLRS_REPO_ROOT/app/debug.overlay"
```

This profile disables control, sub-band mixing and hopping. P1 authentication, session handshake and ARQ remain enabled. Fresh devices can pair on the data
link in this profile; no previous pairing record is needed. Read the **data** section of
`zlrs link`; management status and the LED follow data when control is disabled. The
profile isolates faults; it is not a validated reliable-transfer configuration (see `12-roadmap.md`).

Measure end-to-end payload throughput between two paired devices with:

```sh
python3 tools/zlrs.py throughput --tx-port <TX-serial> --rx-port <RX-serial>
```

On Linux or WSL, serial fragments select the matching data CDC (`...-if02`). The default runs
three simultaneous bidirectional rounds of 4096 bytes per direction, verifies every byte, and
prints iperf-style one-second transfer and bitrate intervals plus a per-direction summary.
It first drains and reports any old bytes waiting at the host ports, then waits for one quiet
second before measuring. Use `--format json` for machine-readable output. `--interval-ms 50`
paces USB input; set it to `0` to push chunks without an intentional delay. This measures the
complete data path, including radio scheduling and ARQ, rather than raw PHY frame rate.

## 4. Debugging on the board

- **With no Core2021 attached the board resets in a loop**: the vendored
  `lr20xx_hal_wait_on_busy()` waits `..._HAL_WAIT_ON_BUSY_TIMEOUT_MSEC` (1 s here) for BUSY and
  then calls `k_oops()`, on the main thread at the highest priority. The fatal handler reboots
  the board, so the LED stays red and USB never enumerates for long. Nothing to do with the
  application: attach the module and it works.
- **The watchdog is on in every image** (`CONFIG_ZLRS_APP_WATCHDOG`, app/prj.conf): a bad image
  resets itself and MCUboot's test/revert rolls it back, without touching BOOTSEL. Turn it off
  with `-DCONFIG_ZLRS_APP_WATCHDOG=n` only for a bench run that must not be reset.
- **Find ports by name**: the application is `ZLRS`, the bootloader `ZLRS MCUBOOT`; use
  `/dev/serial/by-id/...`, not `ttyACM*`.
- **To see whether the radio really transmits, watch the `air:` line** (`stats:` logging above):
  `ctl_tx` and `data_tx` count only transactions that finished, `fail` counts the failures. With
  a single board the only thing that goes on air is pairing: after `pair` on the management port a
  `PAIR_REQ` leaves every 100 ms through the 30 s window, so `ctl_tx` should climb to about 300
  with `fail=0`.
- **Do not read the console with `cat`**: it goes quiet after a moment and it turns on tty echo,
  which feeds the device's own log back in as shell input. Read continuously with pyserial.
- **Over USB/IP forwarding** (usbipd) the CDC's DTR may never arrive, so endpoint choice falls
  back to "host bytes seen"; `zlrs ports` shows `ready / active / rx_total` when that needs
  chasing.
- **Under usbipd a re-enumerating device disappears from WSL entirely** (`/dev/serial/by-id/`
  goes away): this happens when the application resets into the bootloader or reboots after a
  slot swap. Start `usbipd attach --wsl --busid <busid> --auto-attach` on the Windows side for
  each board before repeated update/reboot checks. This starts the WSL attach loop with the
  required privileges; launching the Linux helper as an ordinary user silently fails to attach.
  If an update has already stopped at "waiting for the recovery or application port",
  `--recovery-port <port> --no-trigger` resumes without starting over.
- **Radio numbers** (callback latency, frequency-switch cost, packet error rate, throughput) can
  only be measured on the board; Semtech's `packet_error_rate_flrc` sample is a ready-made
  baseline.
- **After every flash, verify both ends**: `python3 tools/zlrs.py confirm --serial <s>` and compare
  `image-hash` with the image that was built. An upload that the host never confirms rolls back
  at the next boot, and two boards running different images measure like two boards on different
  bands — the numbers look like a radio fault and mean nothing.
- **How a bench board is flashed, and why the image must be confirmed**: `smpmgr upgrade --slot 1
  <img>` writes the secondary slot and marks a test swap, MCUboot swaps it in, and the application
  only makes that permanent once the host confirms (`confirm_image()` in `app/src/main.c`). Until
  then it reboots itself after `CONFIG_ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS` (60 s) so MCUboot can
  revert — and with nothing valid in the other slot, MCUboot ends up in serial recovery instead of
  booting the application. From the host that reads as "the board reboots every minute and then
  looks dead", which is easy to mistake for a firmware crash. For bench work either confirm within
  the first minute (`python3 tools/zlrs.py confirm --serial <s>`) or write the primary slot directly and
  permanently: `smpmgr upgrade --slot 0 --confirm <img>`. The second form skips the rollback safety
  net on purpose; it is for benches with serial recovery in reach, not for anything shipped.
- **A debug bootloader shows its USB for 15 s before the application starts**
  (`app/sysbuild/mcuboot-debug.conf`: `CONFIG_BOOT_SERIAL_WAIT_FOR_DFU_TIMEOUT=15000`). Every
  reset therefore has a window in which only the `ZLRS MCUBOOT` ports exist and the application
  has not started; tools that look for the application port have to wait through it.
- **Validate console replies and reboots**: old debug configurations could strand or truncate
  replies after repeated opens over usbipd. Polling TX, an 8 KiB FIFO and flash-only logs pass
  twelve consecutive open/query/close cycles on both LF boards. Hold one connection for a
  measurement and reject missing statistics. A reboot command alone proves nothing: compare
  kernel uptime before and after. USB/IP can preserve device paths through a real reboot.
- **`zlrs rate`** prints the last second per link: receive windows, how many of them heard a
  frame, frames sent and the state. That is the number to compare when a link misbehaves; a
  receiver problem shows up as a hit rate near zero with the window count unchanged.
- **`zlrs rxwin <ms>`** stretches the data link's receive window (0 restores it). With the window
  at tens of milliseconds the receiver is effectively listening continuously, which separates
  "the two ends are not aligned" from "the receiver does not hear frames at all".
- **The raw radio bench separates the radio from the scheduler**: with
  `CONFIG_ZLRS_APP_BENCH_RAW_MS` the TX role blasts complete frames and the RX role listens in
  100 ms windows, no slots and no phases involved. On this bench that reads 142 of 143 frames per
  second at -42 dBm, so a superframe that delivers far less is a timing problem, not a radio,
  band, sync word or framing problem. It runs by itself at boot, which matters because a long
  radio transaction can drop the forwarded console.

## 5. Traps we have hit

One line each; details live in code comments.

- Several CDCs on a full-speed device fight over the UDC's shared pool, and a failed
  `cdc_acm_buf_alloc()` kills RX silently (`CONFIG_USBD_CDC_ACM_BUF_POOL=y`).
- `CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` registers only the first CDC, so logging on the others
  NULL-dereferences; we build our own USB stack.
- Misreading `nvs_write()`'s return value turns success into failure (check `rc == 0 || rc == len`).
- Using `uart_poll_in/out` in management mode answers no command at all; go through the port layer's
  ISR buffers.
- PBKDF2 for a passphrase takes tens of seconds, so the watchdog has to be disarmed for the
  session.
- RP2350's watchdog is still armed after `SYSRESETREQ`, leaving serial recovery alive for about
  1.5 s (the application disarms before resetting, the bootloader clears the hardware enable
  bit).
- A custom devicetree node in `/chosen` cannot be read (only `zephyr,`-prefixed properties are
  exported), so the radio design lives in its own node.
- Long interrupt masking while reading the RP2350 TRNG kills USB; the driver never masks them.
- A radio transaction submits fine and no frame ever leaves: the RAC planner needs someone to
  pump `smtc_rac_run_engine()` in the main loop, and without it the transaction sits in
  `RP_STATUS_TASK_INIT` forever **without reporting an error**; the symptom is `air: ctl_tx=0`
  and `fail=0`.
- `-Dmcuboot_EXTRA_CONF_FILE=<file>` **replaces** the bootloader configuration sysbuild would
  read from `app/sysbuild/mcuboot.conf`, it does not add to it: a bootloader built with only the
  debug file has no serial recovery at all. Always list the normal file first:
  `-Dmcuboot_EXTRA_CONF_FILE="$ZLRS_REPO_ROOT/app/sysbuild/mcuboot.conf;$ZLRS_REPO_ROOT/app/sysbuild/mcuboot-debug.conf"`.
- The bootloader's 2 s recovery window is too short to enumerate through USB/IP forwarding
  (usbipd): the host never sees the port before the bootloader gives up and boots the
  application. A bench board therefore wants the stretched window
  (`app/sysbuild/mcuboot-debug.conf`, 15 s) — or the application has to ask for recovery.
- Pairing triggered from the management port expires at once if the session's clock is captured
  before the session runs: the pairing press is stamped later, the unsigned subtraction
  underflows, and the first tick calls the window expired. Re-read the clock after the session.
- Stretching the bootloader's recovery window (`app/sysbuild/mcuboot-debug.conf`) lets the host
  enumerate the bootloader's USB first, and it will not re-enumerate when the application takes
  over, so the application never comes up and the watchdog resets it; it looks like a broken
  image. The normal 2 s window never hits this.
- A radio port that reports "the transaction finished" is not enough: the completion semaphore is
  shared by both links, so the other link's completion can wake a wait whose own request is still
  outstanding. The link layer now checks its own request after draining and aborts it if it is
  still pending; without that, `req_pending` stayed set forever and every later `submit` was
  refused — the link goes deaf permanently and only a reboot brings it back.
- An ARQ that gives up by throwing the head frame away is a passthrough that silently loses
  hundreds of bytes. Retry exhaustion now restarts the run (the receiver drops its half message,
  the bytes stay queued) so a bad air path costs time, never data.
- A reassembler timeout is data loss once ARQ is on: the retransmit of the frame after the one
  that already arrived can take longer than 100 ms on a lossy slot, and expiring the half run
  drops bytes the ACK already covered. The passthrough symptom is a periodic hole of one frame
  (~248 B) in every ~750 B. ARQ now keeps a half run alive; only the best-effort path expires.
- **A radio transaction costs milliseconds, not microseconds, on this driver**: the RAC submit
  alone measures 1.8-2.8 ms (`submit=` in the `tm:` line) and a whole slot 6-14 ms, against a plan
  whose slot budget is a quarter of a 6.7 ms frame (1667 us). The loop is therefore work bound:
  four slots take ~24-30 ms, the plan asks for 6.7 ms, and with `CONFIG_ZLRS_APP_PACING_SCALE=1`
  neither end ever sleeps. Two free-running loops at ~28 ms drift past each other by ~1.5 ms per
  frame, so a 2 ms receive window rarely lands on the peer's 1 ms frame, the phase lock has almost
  no hits to work with, and an ARQ'd link ends up delivering a few percent of its frames.
  Raising the pacing (`CONFIG_ZLRS_APP_PACING_SCALE`) makes both ends sleep and hold their period
  (measured 53.0 and 55.0 ms at scale 8, i.e. within 2 ms of each other and stable), but a 2 ms
  window per 53 ms frame still needs the phase to be right, and the lock only ever *lengthens* a
  frame, so the end that is behind keeps stretching. What a working link needs is a slot budget
  that covers a whole receive window, which means an honest frame period for this radio port.
- **Where those milliseconds come from** (`zlrs radio`, 2.4G data link, hopping off, back-to-back
  TX from the at-boot raw bench, 142 frames/s = 7.0 ms per transaction):

  | Part | Measured | Why |
  | --- | --- | --- |
  | `drain` (one SPI status read) | **1.1-1.2 ms** | the radio is asleep and this is the wake-up: the same read again costs **59-84 us** |
  | `rac` (submit) | 0.7-0.9 ms | the RAC enqueues a task per transaction |
  | engine passes | 10 per transaction, 70 us average (max 5.6 ms) | the planner needs several passes to program the radio and to close the transaction |
  | `wait` (submit to completion) | 3.0 ms | radio programming, airtime, DIO IRQ and the pass that handles it |

  The planner puts the radio to sleep after **every** transaction
  (`radio_planner.c`, `ral_set_sleep` right before the completion callback) and the HAL adds a
  500 us sleep when it issues that command, so the wake cost is not avoidable from the application
  side: a pending task cannot prevent it, because the sleep happens before the callback that could
  enqueue one. A high-rate link on this chip wants the radio to stay awake and programmed across a
  frame, which is what the vendor's direct-driver-access path is for.
- **The IRQ path is not the problem**: `CONFIG_LORA_BASICS_MODEM_DRIVERS_EVENT_TRIGGER_NO_THREAD`
  (callback straight from the GPIO ISR) changed the back-to-back rate by nothing measurable (139
  vs 138 frames/s). The RAC's IRQ callback only sets a flag and gives a semaphore, so either mode
  is safe, but the delayed workqueue hop is not where the milliseconds are.
- **The console drops log lines in bursts**: the reported `TX/air/rx/win/tm/rd/key` group loses
  lines whenever the host reads in bursts, and the line that carries the numbers under test is
  often one of them. Anything a measurement depends on needs a shell command (`zlrs radio`,
  `zlrs link`) or a counter the tooling reads, never a log line.
- Sending one confirmation for the pairing and session handshakes is a coin flip: the two ends do
  not share a slot phase while they are still finding each other. Both handshakes repeat, and the
  initiator keeps listening where a peer that missed the confirm is still answering.
- Two free-running superframes do not stay aligned, and a phase lock cannot correct anything until
  it hears a frame (which is what it is trying to fix). Links that have heard nothing for 300 ms
  therefore sweep their frame phase (opposite directions per role — the same direction on both
  ends moves nothing) until the first frame lands.
- `tcxo-wakeup-time` must stay 0 on this module: any non-zero value makes the radio wait out the
  TCXO before every transaction, the RAC sleeps the radio between transactions, and the wait
  overruns the slot budget so nothing is transmitted at all (`air: data_tx=0`).
- Blocking inside a shell command (`k_sleep`) stalls the console CDC badly enough that the host
  drops the device over USB/IP; bench commands read snapshots maintained by the report loop
  instead.
- Re-attaching a board through `usbipd` re-enumerates its USB and the application boots again
  (uptime restarts). Detach and attach only when the port is really missing, and expect the first
  few seconds of console output to be boot noise.

## 6. Hardware timer and independent RAL driver

Both links use the exclusive RAL driver described in [06-architecture](06-architecture.md) 3.
`zlrs direct` and runtime `blast` / `listen` commands have been removed; boot-time raw testing
remains available through `CONFIG_ZLRS_APP_BENCH_RAW_MS`, before the slot owner starts.

`zlrs radio` reports TIMER1 boundaries, maximum IRQ lateness, missed boundaries and skipped
frames/slots; RAL TX completions, RX packets, setup cost, late launches and chip errors; packet
build cost and RX phase error/correction. Empty RX windows are expected when the peer is idle.
It also separates preparation time from launch command time, reports FIFO readiness lead,
launch jitter and RF startup time. `TX command` isolates the five-byte scheduled trigger
from the general command timing, which also includes RX. `launch wait` now measures IRQ
entry and the subsequent result notification; notification latency occurs after actual TX
and does not delay the trigger. Late launches remain discarded.
The aggregate abort counter includes those windows and is not a TX failure count.
Normal logs wait in the RAM queue while RF runs; enter management mode to flush them to the
console and flash. This keeps flash erase/program operations from masking timer IRQs during
normal traffic. Startup, explicit configuration persistence and panic handling can still delay IRQs.

Scheduled TX requires at least 25 us before the alarm when preparation/arming finishes.
Expired alarms are rejected without a software-forced trigger. The ISR also rejects entry
more than 10 us late before touching SPI. `prepare` includes attempts discarded because
they missed that arming margin; ARQ retains their bytes for a later slot.

The slot budget uses the native FLRC airtime calculation (variable header, CRC and coding),
plus `ZLRS_APP_PREPARE_US`, the configured TX guard, a 500 us RF startup allowance and a
100 us end margin. Packet construction, authentication and FIFO writes belong to the preparation
window; the guard only places the final launch command relative to the RF slot boundary. LF CN433/CN470
clamps control PHY to 325 kbps; its control packets cannot fit four equal slots at the requested
150 Hz with current preparation costs. The application clamps the rate before sizing the data
MTU. Hardware timer scheduling does not by itself establish throughput acceptance; see
[12-roadmap](12-roadmap.md) for the pending radio checks.

The default preparation reservation is 6000 us with SPI DMA (2800 us with polling), and
the launch offset is 200 us. Receiver clock alignment uses a 250 us startup estimate.
The RAM-resident zero-latency alarm 3 ISR executes the prepared `SET_TX` with bounded polling,
while the link thread waits for its result. Data24 raises the RF owner to meta-IRQ priority;
management temporarily restores ordinary priority. USB core/controller threads and CDC work
retain upstream cooperative scheduling. Serial notifications are deferred to the system
workqueue. No vendor source or scheduling changes are needed.
Keeping the USB core above CDC work also preserves the upstream completion ordering for ZLPs.

Exclusive data24 uses a separate maximum-frame budget and
`ZLRS_APP_DATA24_PREPARE_US` (default 1500 us). At 2.6 Mbps, a 511-byte frame needs
2140 us on air; adding preparation, the 200 us launch offset, 500 us startup allowance and
200 us of margins gives a 4540 us slot and 9080 us alternating TX/RX pair. The margins cover
the deadline 100 us before preparation and another 100 us for completion; an exactly full
deadline is rejected by the radio size check. Control cadence and regional limits do not size
data24 slots.

`zlrs radio` splits late attempts into `prepare` (the packet/FIFO missed the launch point) and
`trigger` (the prepared launch call ran late). `RX finish` measures GPIO IRQ to completed link
processing, including SPI, authentication and serial output callbacks; `protocol` measures just
the link-completion call. `IRQ service` includes reading and clearing IRQ status over SPI.
Compare counter deltas during a transfer; boot, management and configuration work can affect
the lifetime maxima.

`RX FIFO` counts chunks and bytes read while an FLRC packet is still on air, plus their read
duration. A 128-byte threshold wakes the same RF owner; no SPI runs in the GPIO ISR. Decoded
prefixes stay private until RX_DONE and authentication, and an error, restart or abort discards
them. RX_DONE reads only the remaining tail after checking the final length. `coalesced` counts
completions reported together with a FIFO IRQ: their earlier GPIO timestamp cannot adjust slot
phase. Short packets keep the normal completion path.

All link timestamps use TIMER1 through one fixed uptime epoch: planned slots, completions,
tick checks, acquisition and management-triggered pairing. `protocol clock` reports that time
alongside kernel uptime and their signed difference. Kernel delays or drift must not change
ARQ ages by mixing the two sources; kernel time still schedules ordinary Zephyr work.

`TX setup` splits standby/IRQ handling, modem configuration, FIFO loading and FS/BUSY waiting.
`TX build` separates serial pumping from protocol construction. The RP2350 build places the
RF application and protocol hot path in SRAM; compare the same schedule when evaluating the
effect of Flash instruction fetches. Relocation does not alter the installation/update path.

`TX FIFO` reports fallback packets whose tail is written after SET_TX, tail bytes, I/O
errors and hardware underflows. FLRC preparation writes the first 128 bytes (or the whole
packet if shorter); after hardware launch the owner waits for BUSY and appends the remaining
authenticated bytes through the public RAL. `ready` measures launch to tail-write completion,
not pre-launch readiness. A matching RX-time preload still uses the complete packet. LoRa
retains full pre-launch loading. Abort and completion discard the prefix count, and a failed
tail write or underflow fails the transaction for ARQ to retry.

`SPI DMA` reports the actual peripheral clock/dividers, successful transfer count/bytes and
errors. Timings split buffer/configuration/RX-channel setup, TX-channel start to RX completion
callback (`active`), callback to owner resumption (`wake`), final SPI idle/copy work, and the
preceding BUSY wait. Active time includes driver start and IRQ dispatch, so it is not a logic
analyzer measurement of SCK alone. Divide total counter deltas by transfer deltas for mean
costs; these combine TX writes and RX FIFO reads of different lengths.

ZLRS's RP2350 Crypto driver supports multipart SHA-256. HMAC feeds its word-aligned key prefix
and the frame buffer directly through update/compute; the application keeps no concatenation
buffer. The separate `tests/hardware/crypto` application verifies tags for a 507-byte frame
at all four byte offsets, checks driver session reuse and malformed inputs, and reports
64-call means/maxima for aligned and one-byte-offset inputs. These wall-time measurements
include driver calls, session setup/free and interrupt/thread preemption. A standalone test
runs without the RF/application threads, so its timings are not a radio scheduling budget.
The earlier `zlrs crypto` and `zlrs kdf` test commands have been removed from the application.

The 2026-10-03 LF check used the previous direct Pico SDK backend. At 2441 MHz, P1 and -10 dBm
it reported approximately 84-87 us for an aligned 507-byte tag versus 123 us at a one-byte
offset. Both devices pass all 38 vectors and
retain at least 692 bytes of shell stack. With the retained 1 ms preparation window, six
262145-byte RF duplex rounds pass at 55.1-60.9 kB/s per direction. Three 64 KiB USB rounds
pass in 1.609/1.557/1.561 s; after uptime-proven simultaneous reboots, another three pass in
1.561/1.556/1.559 s. Transfers add no rejected preparations, late triggers, missed boundaries
or slots; launch jitter is at most 2 us and FIFO tail I/O/underflow counters stay zero. This
does not accept a USB throughput gain. Initial configuration still rejects preparations at
boot, and 500/750 us experiments show occasional preparation/arming rejections.

`TX preload` reports packets staged during RX, packets used by the following TX, and FIFO
write duration. During a sustained stream, compare counter deltas to verify that preparation
overlaps RX. A changed session, timeout or periodic SYNC may discard a staged packet; the
normal path then reloads its own bytes. The current RX IRQ and completion semaphore are kept
intact throughout staging.

`zlrs link` also reports successful DATA packets, their user bytes (including retransmissions),
frames at the modem's maximum length, and empty DATA replies carrying only an ACK. Use transfer
counter deltas: `user / DATA packets` gives the mean application bytes per packet. `full` counts
511-byte FLRC or 255-byte LoRa frames; a smaller configured MTU or a frame without an ACK extension
can make that count low even when the application fills its configured packet budget.

The `ACK` line counts DATA packets carrying an ACK, staged packets whose ACK was already
behind receive progress when activated, and transmit windows blocked by a full ARQ window.
`retry_age` is the last/maximum elapsed time used to retry the oldest frame. `delay` measures
first transmission to cumulative acknowledgment for each released window entry, including
retries; `total / entries` gives mean confirmation delay. Use counter deltas to compare runs.
A stale staged ACK remains valid; the newer receive position stays pending for a later packet.

Previous RAC cost measurements in section 5 are historical measurements of the former backend.

### DATA source and sink inside the boards

In exclusive data24, `zlrs stream arm <bytes>` enables the debug-only source/sink (1 byte to
16 MiB), `zlrs stream start` starts this board's source, and `zlrs stream status` reports queued,
received and incorrect bytes plus receive goodput. Arm both boards before starting either.
Each direction uses a different deterministic byte pattern; the receiver checks every byte,
including its position. This retains the actual frame, authentication, ARQ, FIFO and RF paths
while replacing the USB/UART input and output. It does not measure end-to-end serial throughput.

`goodput_Bps` uses bytes after the first delivered packet divided by the time between the first
and last deliveries. Completion requires `rx == target`, `queued == target` and `bad == 0` on
both boards. `zlrs stream stop` restores serial forwarding once the source queue is empty;
arm/stop returns busy when queued bytes still need delivery. Only the RF owner mutates link
queues and benchmark state; shell commands wait for that owner to apply them.


## 7. Hardware test applications

Keep host tests in `tests/host` and Zephyr device tests in `tests/hardware`. A hardware test
application has its own CMake/Kconfig configuration, test source and Ztest manifest. It links
production sources and calls public interfaces, without embedding vectors, test hooks or
benchmarks in `app` or `drivers`. The SHA-256 application is `tests/hardware/crypto`.

Build it in the workspace's usual `build` directory. Reuse the application's sysbuild
configuration so signing, partitions and MCUboot recovery match the installed bootloader:

```sh
export ZLRS_BOARD=rp2350_zero_core2021/rp2350a/m33/lf
export ZLRS_APP="$PWD/tests/hardware/crypto"
source tools/zephyr-env.sh
zlrs-build -p auto -- -DSB_APPLICATION_CONFIG_DIR="$ZLRS_REPO_ROOT/app"
```

The signed image is `../build/crypto/zephyr/zephyr.signed.bin`. Start from confirmed working
firmware and install it through MCUboot; do not use a test application UF2. The test runner
uses the existing update tool with `--no-wait`, opens the test console CDC to start Ztest, saves
the full output, and checks that MCUboot restores the previous firmware hash:

```sh
python3 tests/hardware/crypto/run.py --serial 425030433435300D \
  --log /tmp/zlrs-crypto-rx.log
python3 tests/hardware/crypto/run.py --serial 4250304334353014 \
  --log /tmp/zlrs-crypto-tx.log
```

The test firmware leaves its image unconfirmed, changes no pairing/settings records, and
reboots three seconds after the tests finish. Without a host connection it reboots after
45 seconds. Its watchdog remains armed. On WSL, USB forwarding must follow re-enumeration,
including the `ZLRS CRYPTO TEST` device, during upgrade and rollback.

Coverage includes SHA padding/block boundaries, one-shot and 1/3/64-byte chunks, multiple
digests per session, competing/invalid sessions, invalid input and overflow, long HMAC keys,
unaligned full-frame tags, multi-block HKDF and the production 100000-round PBKDF2 rule.
The adapter and driver run from SRAM, matching the application. The test uses Zephyr's minimal
libc with byte copies: the SDK picolibc `memcpy` faults on a halfword store while the RP2350 UDC
copies enumeration replies into USB DPRAM. Startup banners stay disabled until the USB context
exists, logs are deferred, and Ztest's `printk` output goes directly to the console. These are
test application settings; upstream UDC/library sources are unchanged.

Both LF boards pass all six cases on 2026-10-05, including 32 SHA variants and 13 authentication
tag vectors. Production 100000-round passphrase derivation takes 4307/4308 ms. Standalone
aligned 507-byte tags average 73 us and one-byte-offset tags 112 us. Those measurements use the
test libc and have no RF/application threads; they are not a measured application speedup.
Duplex acceptance uses the actual ZLRS application image after the embedded tests are removed:
three USB rounds deliver 65536 bytes per direction exactly at 42.1-48.3 kB/s; three RF-only
rounds deliver 262145 bytes per direction with zero bad bytes at 45.2-46.5 kB/s. Both LF boards
use DATA24 at 2441 MHz, -10 dBm and P1. Timer misses, skipped frames and late-launch counts
do not increase during either run, with zero SPI errors or TX FIFO underflows. CRC failures
and ARQ retries remain; these checks do not establish a speedup over the previous backend.

To return the build directory to the normal LF debug data image:

```sh
export ZLRS_APP="$ZLRS_REPO_ROOT/app"
zlrs-build -p auto -- \
  -DEXTRA_CONF_FILE="$ZLRS_REPO_ROOT/app/debug.conf;$ZLRS_REPO_ROOT/app/data24-debug.conf" \
  -DDTC_OVERLAY_FILE="$ZLRS_REPO_ROOT/app/debug.overlay"
```
