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
management (see [08-firmware](08-firmware.md) 1).

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
| shell `zlrs mcuboot` | image state as MCUboot sees it (slot headers, confirm bit, copy state) |
| `CONFIG_ZLRS_APP_DEBUG_STATS_LOG` | 1 Hz `stats:` line (counters, LQ, duty, dynamic power) and `air:` line (completed transactions per link) |
| `CONFIG_ZLRS_APP_BENCH_RAW_MS` | the first N ms after boot skip the superframe: the TX role sends complete frames back to back, the RX role opens 100 ms receive windows back to back, and both log frames per second |
| `CONFIG_ZLRS_APP_DEBUG_BOOT_DELAY_MS` | wait that long after USB is up before continuing init, so a panic during init is visible on an open console |
| `CONFIG_ZLRS_APP_DEBUG_LED_BRIGHTNESS_PERCENT` | attenuate the status pixel in a debug image (the default is 20%); production images keep the normal LED levels |
| `CONFIG_ZLRS_APP_TX_GUARD_US` | fixed delay from hardware slot boundary to SET_TX; both peers must use the same timing settings |

The shell is gated by `CONFIG_ZLRS_APP_DEBUG_SHELL` and runs on the console CDC; the supported
configuration paths are in [10-security](10-security.md) 9 and [08-firmware](08-firmware.md) 3.

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
switches to hide but debts: `doc/13-roadmap.md`, "Pending defaults".

For a fixed-channel 2.4 GHz data-only bench on the LF module:

```sh
source tools/zephyr-env.sh
ZLRS_BOARD=rp2350_zero_core2021/rp2350a/m33/lf zlrs-build -d build-data24 -- \
  -DEXTRA_CONF_FILE="$ZLRS_REPO_ROOT/app/debug.conf;$ZLRS_REPO_ROOT/app/data24-debug.conf" \
  -DDTC_OVERLAY_FILE="$ZLRS_REPO_ROOT/app/debug.overlay"
```

This profile disables control, sub-band mixing and hopping. P1 authentication, session
handshake and ARQ remain enabled. Both devices need an existing pairing record: pairing still
runs on the control link; pairing requests are rejected in this profile. Read the **data**
section of `zlrs link`; management status and the LED follow data when control is disabled. The
profile isolates faults; it is not a validated reliable-transfer configuration (see `13-roadmap.md`).

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
  slot swap. Re-attach from the Windows side and carry on; the update tool stops at "waiting for
  the recovery or application port", and `--recovery-port <port> --no-trigger` resumes without
  starting over.
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
- **Do not chatter the console**: opening and closing the console CDC over usbipd a few times in a
  row leaves the stream delivering tens of bytes per second while the device stays enumerated, and
  it does not recover by reading harder. The board is not necessarily dead — its radio work and its
  management port are separate code paths — but the console is the wrong instrument from then on.
  Hold one connection open for the whole session, and reboot the board (or re-attach it) before
  measuring anything that depends on console output.
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

Both links use the exclusive RAL driver described in [07-architecture](07-architecture.md) 3.
`zlrs direct` and runtime `blast` / `listen` commands have been removed; boot-time raw testing
remains available through `CONFIG_ZLRS_APP_BENCH_RAW_MS`, before the slot owner starts.

`zlrs radio` reports TIMER1 boundaries, maximum IRQ lateness, missed boundaries and skipped
frames/slots; RAL TX completions, RX packets, setup cost, late launches and chip errors; packet
build cost and RX phase error/correction. Empty RX windows are expected when the peer is idle.
It also separates preparation time from launch command time, reports FIFO readiness lead,
launch jitter and RF startup time. A ready FIFO does not imply the hardware alarm woke the
owner on time; late launches remain discarded.
The aggregate abort counter includes those windows and is not a TX failure count.
Normal logs wait in the RAM queue while RF runs; enter management mode to flush them to the
console and flash. This keeps flash erase/program operations from masking timer IRQs during
normal traffic. Startup, explicit configuration persistence and panic handling can still delay IRQs.

The slot budget uses the native FLRC airtime calculation (variable header, CRC and coding),
plus `ZLRS_APP_PREPARE_US`, the configured TX guard, a 500 us RF startup allowance and a
100 us end margin. Packet construction, authentication and FIFO writes belong to the preparation
window; the guard only places the final launch command relative to the RF slot boundary. LF CN470
clamps control PHY to 325 kbps; its control packets cannot fit four equal slots at the requested
150 Hz with current preparation costs. The application clamps the rate before sizing the data
MTU. Hardware timer scheduling does not by itself establish throughput acceptance; see
[13-roadmap](13-roadmap.md) for the pending radio checks.

The default preparation reservation is 2800 us and the launch offset is 200 us. A dedicated
meta-IRQ thread executes the prepared `SET_TX` as the hardware alarm's bottom half, while the
link thread waits for its result. USB core/controller threads and CDC work retain upstream
cooperative scheduling. The radio bottom half can preempt them without changing vendor code.
Keeping the USB core above CDC work also preserves the upstream completion ordering for ZLPs.

Previous RAC cost measurements in section 5 are historical measurements of the former backend.
