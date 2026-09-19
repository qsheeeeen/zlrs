# Development and debugging

For developers only: how to build a debug image, how the layered bench switches work, which provisional entries exist and how to chase problems on hardware. User-facing documents never contain this material (see the writing rules in [README](README.md)).

## 1. Debug overlay

A debug build is a normal build plus two files:

| File | Contents |
| --- | --- |
| `app/debug.conf` | enables the console CDC, the shell, the watchdog and the bench switches (see 3) |
| `app/debug.overlay` | adds a console CDC to the devicetree (`zephyr,console` / `zephyr,shell-uart`) and fills in the nodes the debug entries need |

Attach them at build time:

```shell
west build -b "$ZLRS_BOARD" "$ZLRS_APP" -- \
  -DEXTRA_CONF_FILE=debug.conf -DDTC_OVERLAY_FILE=debug.overlay
```

Production firmware has no console CDC and no shell; its three CDCs are CRSF, data and config (see [08-firmware](08-firmware.md) 1).

## 2. Debug entries

These exist in debug builds only; the supported paths are in [10-security](10-security.md) 10 and [08-firmware](08-firmware.md) 2:

| Entry | How to use it |
| --- | --- |
| shell `zlrs config` / `status` / `ports` / `recovery` | gated by `CONFIG_ZLRS_APP_SHELL`, run on the console CDC |

## 3. Layered bench switches

Do not bring everything up at once: turn one layer off at a time, and if the rest still works the fault is not in that layer. Every disabled switch prints a `bring-up:` warning at boot and has a matching host test (`tests/host`).

| Switch | With it off | Host test |
| --- | --- | --- |
| `CONFIG_ZLRS_APP_SECURITY_PROFILE_P0=y` | frames carry no tag, so timing and frame format come first | `link: bring-up without tag (P0)` |
| `CONFIG_ZLRS_APP_FHSS=n` | everything stays on the sync channel | `link: bring-up without hopping` |
| `CONFIG_ZLRS_APP_SESSION_HANDSHAKE=n` | the pairing key becomes the session key | `link: bring-up without session handshake` |
| `CONFIG_ZLRS_APP_TELEMETRY=n` | the control link carries downlink RC only (the TX peer-loss check goes too) | `link: bring-up without telemetry` |
| `CONFIG_ZLRS_APP_DATA_ARQ=n` | the data link falls back to best-effort | `link: arq *`, `link: backpressure` |
| `CONFIG_ZLRS_APP_RATE_ADAPT=n` | one fixed rate tier | `link: rate adapt` |
| `CONFIG_ZLRS_APP_DYNAMIC_POWER=n` | fixed TX power | `power: *` |
| `CONFIG_ZLRS_APP_LINK_CONTROL=n` / `ZLRS_APP_LINK_DATA=n` | one link only | compile check (both SKUs are in CI) |
| `CONFIG_ZLRS_APP_RADIO=n` | the radio sits out (`app/src/radio_null.c`): no USP/RAC init, no transactions, each one finishes at once as "sent, nothing heard"; ports, config, button, LED and NVS work as usual | compile check plus on-board USB, ports and config |
| `CONFIG_ZLRS_APP_CRSF_PORT=n` | the CRSF port stays uninitialised: the TX sends only the configured failsafe values | compile check |
| `CONFIG_ZLRS_APP_STATUS_LED=n` | no status LED | compile check |

Suggested order: `P0` with hopping, handshake and telemetry off, confirming that we hear our own frames; then turn tags on (`P1`); then hopping; then telemetry and the peer-loss check; rate adaptation and dynamic power come last, since their thresholds wait for measured curves.

## 4. Debugging on the board

- **Turn the radio off first** (`ZLRS_APP_RADIO=n`) to check USB, ports, config mode and NVS: `python3 tools/zlrs-bringup.py` does the lot in one command (three CDCs, config mode, config protocol reads and writes, NVS persistence).
  With no module attached, use the ready-made config file:

  ```shell
  west build -b "$ZLRS_BOARD" "$ZLRS_APP" -- \
    -DEXTRA_CONF_FILE="$ZLRS_REPO_ROOT/app/debug.conf;$ZLRS_REPO_ROOT/app/bringup-noradio.conf" \
    -DDTC_OVERLAY_FILE="$ZLRS_REPO_ROOT/app/debug.overlay"
  ```

  **Radio on with no Core2021 attached means a dark board**: the vendored `lr20xx_hal_wait_on_busy()` spins waiting for BUSY (its `k_usleep` is commented out) with a 600 s default timeout and a `k_oops()` when it expires; it runs on the main thread at the highest priority, so the USB stack never gets the CPU and you see no LED, no USB enumeration and an application reset by the 8 s watchdog over and over. Nothing to do with the application: attach the module and it works.
- **Leave the watchdog on** while debugging (`CONFIG_ZLRS_APP_WATCHDOG=y`): a bad image resets itself and MCUboot's test/revert rolls it back, parking in serial recovery without touching BOOTSEL.
- **To catch the boot log** use `CONFIG_ZLRS_APP_BOOT_DELAY_MS` (0 by default): it waits that long after USB is up before continuing init, giving the host time to open the console port. Otherwise a panic during init lands on a port nobody has open and you see nothing.
- **Find ports by name**: the application is `ZLRS`, the bootloader `ZLRS MCUBOOT`; use `/dev/serial/by-id/...`, not `ttyACM*`.
- **To see whether the radio really transmits, watch the `air:` line** (`CONFIG_ZLRS_APP_STATS_LOG=y`): `ctl_tx` and `data_tx` count only transactions that finished, `fail` counts the failures. With a single board the only thing that goes on air is pairing: after `pair` on the config port a `PAIR_REQ` leaves every 100 ms for a 30 s window, so `ctl_tx` should climb to about 300 with `fail` at 0.
- **Do not read the console with `cat`**: it goes quiet after a moment and it turns on tty echo, which feeds the device's own log back in as shell input. Read continuously with pyserial instead (a small tool of your own outside `tools/`).
- **Over USB/IP forwarding** (usbipd) the CDC's DTR may never arrive, so endpoint choice falls back to "host bytes seen"; `zlrs ports` shows `ready/active/rx_total` for both endpoints when that needs chasing.
- **Under usbipd a re-enumerating device disappears from WSL entirely** (`/dev/serial/by-id/` goes away): this happens when the application resets into the bootloader or reboots after a slot swap. Re-attach from the Windows side (detach then attach to WSL) and carry on; the update tool stops at "waiting for the recovery or application port", and once the device is back `--recovery-port <port> --no-trigger` continues without starting over.
- **Radio numbers** (callback latency, frequency-switch cost, packet error rate, throughput) can only be measured on the board, and Semtech's `packet_error_rate_flrc` sample is a ready-made baseline.

## 5. Traps we have hit (one line each; details live in code comments)

- Several CDCs on a full-speed device fight over the UDC's shared pool, and a failed `cdc_acm_buf_alloc()` kills RX silently (`CONFIG_USBD_CDC_ACM_BUF_POOL=y`).
- `CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` registers only the first CDC, so logging on the others NULL-dereferences (we build our own USB stack).
- Misreading `nvs_write()`'s return value turns success into failure (check `rc == 0 || rc == len`).
- Using `uart_poll_in/out` in config mode answers no command at all (go through the port layer's ISR buffers).
- PBKDF2 for a passphrase takes tens of seconds, so the watchdog has to be disarmed for the session.
- RP2350's watchdog is still armed after `SYSRESETREQ`, leaving serial recovery alive for about 1.5 s (the application disarms before resetting, the bootloader clears the hardware enable bit).
- A custom devicetree node in `/chosen` cannot be read (only `zephyr,`-prefixed properties are exported), so the radio design lives in its own node.
- Long interrupt masking while reading the RP2350 TRNG kills USB (off by default; see [13-roadmap](13-roadmap.md)).
- A radio transaction submits fine and no frame ever leaves: the RAC planner needs someone to pump `smtc_rac_run_engine()` in the main loop, and without it the transaction sits in `RP_STATUS_TASK_INIT` forever **without reporting an error**; the symptom is `air: ctl_tx=0` and `fail=0`. Every vendor sample's main loop has that call.
- Pairing triggered from the config port expires at once: the main loop's `now_ms` is taken **before** the config session, and anything stamped inside it (the pairing press) is later, so `now_ms - state_ms` goes negative, the unsigned subtraction underflows and the first tick calls the window expired. Re-read the clock after the session.
- Stretching the bootloader's recovery window (`app/sysbuild/mcuboot-debug.conf`) lets the host enumerate the bootloader's USB first, and it will not re-enumerate when the application takes over: the application never comes up, the watchdog resets it and it looks like a broken image. The normal 2 s window never hits this; the application also holds the USB controller in reset for 100 ms in `subsys/usb/usb_dev.c` to force a fresh enumeration.
