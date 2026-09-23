# Development and debugging

For developers only: how to build a debug image, which switches exist to isolate a layer, and
how to chase problems on real hardware. User-facing documents never contain this material.

## 1. Debug image

A debug image is a normal build plus two files:

| File | Contents |
| --- | --- |
| `app/debug.conf` | sets `CONFIG_ZLRS_APP_DEBUG=y`, then enables the console CDC, the shell, immediate logging, asserts and the watchdog |
| `app/debug.overlay` | adds a console CDC (`cdc_acm_uart3`) to the devicetree and points `zephyr,console` / `zephyr,shell-uart` at it |

```shell
west build -b "$ZLRS_BOARD" "$ZLRS_APP" -- \
  -DEXTRA_CONF_FILE="$ZLRS_REPO_ROOT/app/debug.conf" \
  -DDTC_OVERLAY_FILE="$ZLRS_REPO_ROOT/app/debug.overlay"
```

Production firmware has no console CDC and no shell; its three CDCs are control, data and
config (see [08-firmware](08-firmware.md) 1).

`CONFIG_ZLRS_APP_DEBUG` is the marker for "this image is for the bench": the options named
`ZLRS_APP_DEBUG_*` depend on it, and shipped configurations (`app/prj.conf`, everything under
`boards/`) may not mention it. `tools/gates.sh` enforces that, so a bench-only switch cannot
quietly end up in a released image.

## 2. Debug entries

| Entry | How to use it |
| --- | --- |
| shell `zlrs status` | role, band, region, link states, data port state |
| shell `zlrs ports` | per-endpoint `ready / active / rx_total / rx_dropped` for the data port |
| shell `zlrs config` | enter config mode (EXIT returns) |
| shell `zlrs recovery` | reboot into MCUboot serial recovery |
| shell `zlrs entropy` | entropy self-test (two 32-byte TRNG blocks) |
| shell `zlrs mcuboot` | image state as MCUboot sees it (slot headers, confirm bit, copy state) |
| `CONFIG_ZLRS_APP_DEBUG_STATS_LOG` | 1 Hz `stats:` line (counters, LQ, duty, dynamic power) and `air:` line (completed transactions per link) |
| `CONFIG_ZLRS_APP_BENCH_RAW_MS` | the first N ms after boot skip the superframe: the TX role sends complete frames back to back, the RX role opens 100 ms receive windows back to back, and both log frames per second |
| `CONFIG_ZLRS_APP_DEBUG_BOOT_DELAY_MS` | wait that long after USB is up before continuing init, so a panic during init is visible on an open console |

The shell is gated by `CONFIG_ZLRS_APP_DEBUG_SHELL` and runs on the console CDC; the supported
configuration paths are in [10-security](10-security.md) 9 and [08-firmware](08-firmware.md) 3.

## 3. Switches for isolating a layer

Do not bring everything up at once: turn one layer off at a time, and if the rest still works
the fault is not in that layer. Every disabled isolation switch prints a `bring-up:` warning at
boot, and each has a matching host test.

**Isolation (debug images only — a production configuration cannot reach them):**

| Switch | With it off | Host test |
| --- | --- | --- |
| `CONFIG_ZLRS_APP_SECURITY_PROFILE_P0=y` | frames carry no tag, so timing and frame format come first | `link: bring-up without tag (P0)` |
| `CONFIG_ZLRS_APP_FHSS=n` | everything stays on the sync channel | `link: bring-up without hopping` |
| `CONFIG_ZLRS_APP_SESSION_HANDSHAKE=n` | the pairing key becomes the session key | `link: bring-up without session handshake` |
| `CONFIG_ZLRS_APP_TELEMETRY=n` | the control link carries downlink RC only (the TX peer-loss check goes too) | `link: bring-up without telemetry` |
| `CONFIG_ZLRS_APP_DATA_ARQ=n` | the data link falls back to best-effort | `link: arq *`, `link: backpressure` |
| `CONFIG_ZLRS_APP_SUB_BAND_MIX=n` | one hop table, no sub-G migration | `link: sub-band mix *` |
| `CONFIG_ZLRS_APP_RADIO=n` | the radio sits out (`app/src/radio_null.c`): no USP/RAC init, every transaction finishes at once as "sent, nothing heard"; ports, config, button, LED and NVS work as usual | compile check plus on-board USB, ports and config |

**Shape (any image may set these — they cut the product):**

| Switch | Effect |
| --- | --- |
| `CONFIG_ZLRS_APP_LINK_CONTROL=n` / `ZLRS_APP_LINK_DATA=n` | one link only (compile check; both SKUs are in CI) |
| `CONFIG_ZLRS_APP_BAND_ROLE_*` | control on sub-G (long range) or on 2.4G (racing) |
| `CONFIG_ZLRS_APP_CONTROL_MODULATION_LORA` | LoRa control tier instead of FLRC |

**Features whose default is still open** (rate adaptation, dynamic power, watchdog) are not
switches to hide but debts: `doc/13-roadmap.md`, "Pending defaults".

## 4. Debugging on the board

- **Turn the radio off first** (`CONFIG_ZLRS_APP_RADIO=n`, debug images only) to check USB,
  ports, config mode and NVS: `python3 tools/zlrs-bringup.py` does the lot in one command
  (ports, config mode, config protocol reads and writes, NVS persistence). With no module
  attached, use the ready-made file:

  ```shell
  west build -b "$ZLRS_BOARD" "$ZLRS_APP" -- \
    -DEXTRA_CONF_FILE="$ZLRS_REPO_ROOT/app/debug.conf;$ZLRS_REPO_ROOT/app/bringup-noradio.conf" \
    -DDTC_OVERLAY_FILE="$ZLRS_REPO_ROOT/app/debug.overlay"
  ```

  **Radio on with no Core2021 attached means a dark board**: the vendored
  `lr20xx_hal_wait_on_busy()` spins waiting for BUSY (600 s default, then `k_oops()`), on the
  main thread at the highest priority, so the USB stack never gets the CPU and you see no LED,
  no USB enumeration and an application reset by the watchdog over and over. Nothing to do with
  the application: attach the module and it works.
- **Leave the watchdog on** while debugging (`CONFIG_ZLRS_APP_WATCHDOG=y`): a bad image resets
  itself and MCUboot's test/revert rolls it back, without touching BOOTSEL.
- **Find ports by name**: the application is `ZLRS`, the bootloader `ZLRS MCUBOOT`; use
  `/dev/serial/by-id/...`, not `ttyACM*`.
- **To see whether the radio really transmits, watch the `air:` line** (`stats:` logging above):
  `ctl_tx` and `data_tx` count only transactions that finished, `fail` counts the failures. With
  a single board the only thing that goes on air is pairing: after `pair` on the config port a
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
- **After every flash, verify both ends**: `zlrs-config.py --serial <s> confirm` and compare
  `image-hash` with the image that was built. An upload that the host never confirms rolls back
  at the next boot, and two boards running different images measure like two boards on different
  bands — the numbers look like a radio fault and mean nothing.
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
- Using `uart_poll_in/out` in config mode answers no command at all; go through the port layer's
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
- Pairing triggered from the config port expires at once if the session's clock is captured
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
