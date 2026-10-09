# Firmware behaviour

What the firmware looks like from outside: USB and ports, operating modes, runtime
configuration, build-time choices, boot and updates, and the error-handling rules.
Architecture: [06-architecture](06-architecture.md); protocol: [08-protocol](08-protocol.md);
security: [09-security](09-security.md); debugging: [11-debug](11-debug.md).

## 1. USB and ports

The firmware presents three CDCs by default:

| CDC | Purpose | Its UART endpoint |
| --- | --- | --- |
| control | CRSF channels, telemetry, configuration and MSP | UART0 (GP0/GP1, TX role 400000; RX role 420000) |
| data | the passthrough stream | UART1 (GP8/GP9, 115200 by default, changeable in NVS) |
| **management** | the management protocol: enter management mode, read and write parameters | - |

Debug builds add one more **console** CDC (kernel log plus shell, see [11-debug](11-debug.md)).
In bootloader mode the device shows two CDCs (recovery and console) instead.

The name each mode announces over USB comes from `CONFIG_ZLRS_USB_PRODUCT`: `ZLRS` for the
application and `ZLRS MCUBOOT` for the bootloader; host scripts find ports under
`/dev/serial/by-id/...`.

**A port is a logical port**: one UART endpoint plus one CDC endpoint, used as a pair. Each
link picks one of them — the CDC while the host has it open (DTR asserted) **or** once it has
ever received a host byte, otherwise the UART. The second rule exists for USB/IP forwarding
(usbipd) and virtualised setups where `SET_CONTROL_LINE_STATE` may never arrive: trusting DTR
alone would leave the CDC unselected and host bytes would be dropped silently as "not the
active endpoint". The ISR records "a byte arrived" **before** checking the active endpoint, so
the first frame is never lost to that lag (selection is local to `app/src/crsf/serial.c` and `app/src/data/data.c`).

For CRSF, the USB/IP fallback lasts one second after a **valid frame** instead of permanently
latching on the first byte. UART becomes active again when USB DTR is off and that interval
expires. Switching endpoints discards queued input and pending output after any frame already
being transmitted. RC uses a separate latest-sample slot so configuration backpressure cannot
hold an old control sample ahead of a new one. CRSF output queues complete frames and expires
queued channel frames after the input timeout. The DATA port retains its existing byte-based
selection rule.

Locally generated channel/statistics/timing frames replace the same type's unsent queued
value, bounding periodic backlog even when the handset's single-wire reply rate is low.
Routed frames and device responses keep complete-frame FIFO ordering and backpressure.
During baud negotiation, new periodic output waits until the old-speed response is drained.

The two links choose their endpoints independently. The management CDC carries no passthrough
traffic, only the management protocol; a board without that CDC simply has no management mode.

## 2. Operating modes

| Mode | Links | Management CDC | Notes |
| --- | --- | --- | --- |
| **application** | normal traffic | idle | the usual state |
| **management** | **radio stopped entirely** (no RX, no TX) | runs the management protocol | entered by a tool command, left on `EXIT` or after 300 s idle |
| serial recovery | no application | - | bootloader mode, see 5 |

While in management mode:

- the radio neither receives nor transmits and the link state freezes;
- bytes arriving on the data port are **dropped and counted**, never buffered; normal traffic
  resumes on exit;
- the status LED shows steady white after half a second (tool round-trips do not flash);
- the watchdog is disarmed for the session — PBKDF2 on a passphrase takes tens of seconds on
  this MCU and an 8 s watchdog would reset mid-way — and armed again on exit.

Incoming management bytes wake the session immediately. Its 50 ms idle wait only bounds
housekeeping latency; it does not limit request/reply throughput. `ENTER` replies after the
session is ready and pending logs have been flushed, including a repeated `ENTER`.

## 3. Runtime configuration

**Every user-visible setting is configured at runtime, with no firmware rebuild**:

| Parameter | Notes |
| --- | --- |
| root key / passphrase | the device derives the root key from a passphrase with PBKDF2 (100,000 rounds), or you write 64 hex characters; takes effect at once |
| data port baud rate | 9600-4000000, effective on the next boot |
| region | 0 = FCC915, 1 = EU868, 2 = CN470, 3 = CN433; a fresh board starts on its radio design's `default-region`, and a write is checked against that design (band and frequency coverage) and refused on a mismatch; effective on the next boot |
| TX power ceiling | never above the chosen region's limit; effective on the next boot |
| role TX / RX | one firmware runs either end; effective on the next boot |
| device id | generated from the TRNG on the first boot; `new-dev-id` replaces it at once and erases the pairing record |
| security profile | `p1` is the production profile, `p0` (no tag) is refused unless this is a debug image, `p2` is not implemented yet; effective on the next boot, and the peer has to run the same profile |
| pairing | start pairing at once, erase the pairing record (next boot) |
| factory reset | wipes NVS (keys, pairing, baud, region, power, role, profile); effective on the next boot |

The command set is in [09-security](09-security.md) 9.

### CRSF device menu and commands

TX module address is `0xEE`, receiver `0xEC`, handset `0xEA`, FC `0xC8`. Both devices answer
directed and broadcast discovery and advertise ten parameters, with root folder zero:

| ID | Parameter | Type | Application |
| --- | --- | --- | --- |
| 1 | Rate target | FLOAT with zero decimals, Hz | 4–1000 requested Hz, saved; actual rate is budget-clamped on reboot |
| 2 | Power | FLOAT with zero decimals, dBm | -17 to the selected region ceiling, saved; applies on reboot |
| 3 | Region | TEXT_SELECTION | SKU and power validated before saving; applies on reboot |
| 4 | Bind | COMMAND | start/cancel/poll the local pairing window |
| 5 | Model ID | FLOAT with zero decimals | 0–255, saved and applied immediately |
| 6 | Model match | TEXT_SELECTION | Off/On, saved; RX cuts channels from a different model |
| 7 | Telemetry | TEXT_SELECTION | Off/On, saved; gates external FC telemetry, preserves link statistics and configuration |
| 8 | Mode | TEXT_SELECTION | Mixed/DATA24, saved; applies on reboot |
| 9 | Firmware | INFO | version and implemented services, read-only |
| 10 | Apply / reboot | COMMAND | START asks for confirmation; CONFIRM replies then reboots after two seconds; CANCEL aborts |

Both peers need matching mode and control-rate targets. Read chunks start at zero and may be
re-requested; accepted FLOAT/TEXT_SELECTION writes echo `0x2D` only after persistence succeeds.
Rejected writes return the unchanged `0x2B` definition. Commands return their updated `0x2B`
status. Model selection via direct `0x32.0x10.0x05` is volatile because the handset sends it
during startup; the menu's Model ID is the persistent fallback.

Direct commands support local bind/cancel, model selection/query and external telemetry
enable/disable. Both ends must enter pairing, through their CRSF menu/command, management
command or button. The TX sends CRSF Shot at 10 Hz, using the actual RC input timestamp and
RF preparation deadline. UART speed proposals accept port zero at 115200, 400000, 416666,
420000, 1000000 or 2000000 baud. The response finishes at the old baud before an idle UART
switches; no valid traffic for one second restores the role's default baud. USB and requests
routed over the radio reject physical baud changes. UART negotiation and handset scheduling
still require electrical interoperability acceptance.

Who decides what:

| Decision | When | How |
| --- | --- | --- |
| Which links a build supports | build time | Kconfig shape switches, gated by capabilities |
| Which port carries which link on a two-port board | build time | devicetree aliases |
| Which link the single port carries *(design, not implemented)* | runtime, default from devicetree | management protocol, stored in NVS |
| Band role and control modulation | build time | Kconfig choices |
| Operating mode (`mixed` / `data24`) | runtime, effective after reboot | `set-mode`, stored in NVS; `data24` uses 2.4 GHz for pairing and data |
| Role TX / RX, security profile | runtime, default at build time | `set-role` / `set-profile`, stored in NVS |
| Region, power ceiling, baud rate, pairing | runtime | management protocol, stored in NVS |
| Control rate target, model id/match, external FC telemetry | runtime | CRSF parameter menu; the rate target applies on reboot and is clamped to the radio budget |
| Rate tier, telemetry period | build time today | the ledger in [12-roadmap](12-roadmap.md) tracks performance work |
| Hardware presence (ports, management channel, button, LED, watchdog, SHA-256, TRNG, radio design) | devicetree | `app/src/platform/caps.h` |
| Bench isolation (`RADIO=n`, hopping off, P0, …) | build time, debug images only | `ZLRS_APP_DEBUG*`, see [11-debug](11-debug.md) 3 |

Two properties follow and are worth stating explicitly: **one image runs both roles** (a field
is flashed with the same file, roles are assigned during configuration), and **defaults are
build-time while behaviour is field-time** (changing how a deployed device behaves never needs a
new build).

### Exclusive 2.4 GHz data mode

Use the same firmware for a standalone wireless serial link:

```sh
python3 tools/zlrs.py set-mode data24 --all
```

Reboot both peers after saving the mode. Assign one TX and one RX, set the same binding phrase,
then run `python3 tools/zlrs.py pair --all` or hold each device's pairing button. No previous
pairing record is needed: pairing, session handshake and bidirectional data all run on the
2.4 GHz data link. RC and periodic control-link statistics are disabled; CRSF device discovery,
parameters, commands and opaque telemetry/MSP use the separate CRSF service on the data link.
Sub-band mixing cannot move data to sub-G.
P1 authentication, FHSS and ARQ keep their normal settings. Status shows the **active** mode;
a saved mode change appears there only after reboot.

`set-mode mixed` restores the build's control/data band roles after reboot. A valid pairing
record survives switching modes. Both peers must use matching modes and compatible firmware.
A build that removes a required link rejects the corresponding mode selection.
In data24 all four timer windows carry 2.4 GHz data traffic, reusing the two logical data slots
A/B twice per frame. Its slot preparation reserve is 5 ms (6 ms in mixed mode), yielding a
246-byte data MTU at the 34 Hz frame plan. On LF hardware, a paced 4 KiB bidirectional transfer
completed byte-exact in 9.23 s (32 bytes / 50 ms), versus 30.95 s before reusing the four timer
windows. A 32 KiB transfer per direction completed byte-exact in 55.52 s at 64 bytes / 100 ms
(about 590 B/s); 16 KiB completed in 25.12 s at 56 bytes / 80 ms (about 652 B/s). At a higher
750 B/s input pace the queues overflowed and delivery stalled, so sustained goodput remains
limited by serial-to-radio backpressure and ARQ recovery.

## 4. Build-time choices and trimming

What a build decides:

| Choice | Where | Notes |
| --- | --- | --- |
| band role: long range or racing | `CONFIG_ZLRS_APP_BAND_ROLE_*` | which link owns sub-G |
| control modulation: FLRC or LoRa | `CONFIG_ZLRS_APP_CONTROL_MODULATION_*` | data is always 2.4G FLRC |
| control and data rate / frame rate | `CONFIG_ZLRS_APP_*_RATE_*` | clamped at boot by the region's duty cap and airtime |
| control frame rate in racing mode | `CONFIG_ZLRS_APP_RACING_CONTROL_RATE_*` | 2.6 Mbps / 500 Hz by default |
| telemetry period, resync period, CRSF timeout, config timeout, pairing hold | `CONFIG_ZLRS_APP_*` | timing parameters |
| default role and default profile | `CONFIG_ZLRS_APP_ROLE_TX`, profile choice | the stored value wins once the tool writes one |
| which links exist | `CONFIG_ZLRS_APP_LINK_CONTROL` / `LINK_DATA` | a deliberate cut for a control-only or data-only product |
| feature enable | ARQ, sub-band mixing, rate adaptation, dynamic power, watchdog | see the ledger in [12-roadmap](12-roadmap.md) |
| bench isolation | debug images only | radio off, hopping off, session handshake off, telemetry off, P0; see [11-debug](11-debug.md) 3 |

**One firmware has to fit several hardware configurations**; that is a hard requirement:

- USB must be trimmable: the single-port variant has no native USB;
- the data UART and the button have to be optional too;
- this is expressed in devicetree, not in `#ifdef`s.

The application only knows aliases (`zlrs-control-uart`, `zlrs-control-cdc`, `zlrs-data-uart`,
`zlrs-data-cdc`, `zlrs-mgmt-cdc`, `zlrs-button`, `zlrs-led`); a missing alias means "this board
has no such port or peripheral" and everything still builds. Whether there is a CRSF port, a
management port, a button or a status LED is therefore a devicetree fact
(`app/src/platform/caps.h`), not a switch — a switch could only disagree with the board.
`ZLRS_APP_CORE` enables the pure C libraries every image carries (`ZLRS_PROTO`, `ZLRS_LINK`,
`ZLRS_REGION`, `ZLRS_CRSF`, `ZLRS_MGMT`).

That header is the whole hardware interface:

```c
/* ports */
#define ZLRS_PORT_CTRL_FIXED   (DT_NODE_EXISTS(DT_ALIAS(zlrs_control_uart)) || DT_NODE_EXISTS(DT_ALIAS(zlrs_control_cdc)))
#define ZLRS_PORT_DATA_FIXED   (DT_NODE_EXISTS(DT_ALIAS(zlrs_data_uart))    || DT_NODE_EXISTS(DT_ALIAS(zlrs_data_cdc)))
#define ZLRS_PORT_MGMT       DT_NODE_EXISTS(DT_ALIAS(zlrs_mgmt_cdc))

/* peripherals */
#define ZLRS_HAS_BUTTON        DT_NODE_EXISTS(DT_ALIAS(zlrs_button))
#define ZLRS_HAS_LED           DT_NODE_EXISTS(DT_ALIAS(zlrs_led))
#define ZLRS_HAS_WDT           DT_NODE_EXISTS(DT_NODELABEL(watchdog0))
#define ZLRS_HAS_NVS           IS_ENABLED(CONFIG_NVS)
#define ZLRS_HAS_USB           IS_ENABLED(CONFIG_ZLRS_USB)

/* radio design */
#define ZLRS_RF_BAND           DT_PROP(DT_NODELABEL(zlrs_rf), band)
#define ZLRS_RF_LOW_HZ         DT_PROP(DT_NODELABEL(zlrs_rf), low_hz)
#define ZLRS_RF_HIGH_HZ        DT_PROP(DT_NODELABEL(zlrs_rf), high_hz)
#define ZLRS_DEFAULT_REGION    DT_PROP(DT_NODELABEL(zlrs_rf), default_region)
```

`ZLRS_PORT_LINK*` arrives with the single-port form; no board has one yet.

The capability header also rejects contradictions at build time: a link enabled without its
port, a watchdog enabled without a watchdog device, or a board without NVS are `#error`s, not
runtime surprises. At boot the firmware prints one line with what it believes the hardware is.

What the hardware provides also bounds what the software may do:

| The hardware provides | The software may do | The software must refuse |
| --- | --- | --- |
| two ports | control link plus data link, or both ports on one link | a third link |
| one port | one link, role chosen by configuration | two simultaneous links |
| a management CDC | configuration protocol, management mode, recovery entry | using it as a link endpoint |
| a button | long press to pair | — (pairing from the tool always works) |
| a status LED | the LED language ([10-ux](10-ux.md)) | — |
| NVS | keys, pairing, baud, region, power, role, profile | **required**: a board without NVS is rejected |
| a hardware watchdog | reset on a hang, rollback assistance | — |

**Each image builds its own USB stack** (`subsys/usb`, `CONFIG_ZLRS_USB`, enabled separately by
the application and the bootloader configs): a `zephyr,cdc-acm-uart` node in devicetree only
says how many CDCs exist; the image builds a context, registers the classes and calls
`usbd_enable()`. Zephyr's `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` registers only the first CDC
while this firmware has several, so that switch stays off.

## 5. Boot, recovery and updates

Partitions and the boot chain are in [02-platform](02-platform.md) 4. There are three ways into
serial recovery, none needing the button:

| Entry | When it applies |
| --- | --- |
| from the application | the recovery command on the management port (debug builds can also use `zlrs recovery` in the shell), which sets the boot-mode flag and reboots |
| boot window | a valid mcumgr frame within 2 s of any reset (`CONFIG_BOOT_SERIAL_WAIT_FOR_DFU_TIMEOUT`) |
| no bootable image | a failed signature check or an empty image parks in recovery |

None of the three is a switch: every build target runs under MCUboot and has all of them (there is
no bootloader-less target, for the reason in section 5).

The boot-mode flag lives in 4 bytes at the start of SRAM (retained RAM), shared by the
application and the bootloader. `CONFIG_BOOT_SERIAL_PIN_RESET` is deliberately off: on RP2350
POWMAN's `HAD_RUN` bit is mapped to `RESET_PIN`, which is broader than "the reset button was
pressed".

The window is 2 s rather than something shorter: after a confirm timeout or a watchdog reset
that window lets the host get back into recovery and reflash on its own. The cost is that every
boot waits that long.

**Updates**: after the one-off flash (BOOTSEL for the bootloader plus the first application
image), later firmware updates never need the board touched, including when the new image does
not start, USB never comes up, the host never confirms or the image is corrupt. Four invariants:

1. **The bootloader never updates itself** (only BOOTSEL writes that 128 KB at `0x000000`).
2. **Updates always go to the inactive slot**: MCUboot's serial recovery upload defaults to the
   primary slot, which would overwrite the running image with no rollback, so
   `CONFIG_MCUBOOT_SERIAL_DIRECT_IMAGE_UPLOAD=y` lets the tool name a physical slot and the tool
   works out which one is inactive itself.
3. **The tool sets pending, only the application sets permanent**: the application calls
   `boot_write_img_confirmed()` after the image runs and the host sends its confirm command.
4. **Every unconfirmed path resets on its own**: the application resets when it is alive but
   unconfirmed (60 s after boot by default, `ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS`), the
   bootloader's watchdog resets it when it hangs, and the application reboots on a fatal error
   instead of halting (app/src/platform/watchdog.c). A radio that never comes up is one of those
   fatal errors: the vendored HAL panics once BUSY stays inactive for
   `..._HAL_WAIT_ON_BUSY_TIMEOUT_MSEC` (1 s here). An upgrade that leaves the board without a
   working radio therefore rolls back like any other failed image. There is no switch for the
   confirmation mechanism: an image that runs under MCUboot is always confirmed by the host or
   rolled back.

One command updates a device:
`python3 tools/zlrs.py update --port <application management port>` — trigger recovery, wait for
the recovery port, read the active slot, write the other one and mark it test, wait for the
application, check the image hash and send `confirm`. The tool writes pending but **never
permanent**. A broken *application* is recovered through the bootloader too (serial recovery is
always reachable); only a broken *bootloader* needs BOOTSEL and a UF2 drag
(see [11-debug](11-debug.md)).
Production images use their own signing key; changing that key means reflashing the bootloader.

**An image dragged onto BOOTSEL has no rollback**: it goes straight into slot0 with an empty
trailer, which MCUboot treats as the initial image — it boots it without asking for a
confirmation and there is nothing to revert to; an app that resets during init then simply
loops. **The application is therefore never installed as a UF2**: BOOTSEL carries the bootloader
image only, and every application image — first install included — goes in through the bootloader
(serial recovery on a blank board, the management port afterwards). That is the only path on which
the signature check, the confirmation and the rollback apply.

## 6. Error handling

- **A fatal configuration fault stops the air, not the device**: a region/radio-design mismatch
  drops into `fault_loop()`, which shows the 2 Hz theme/red failsafe pattern, keeps feeding the
  watchdog and keeps the management port serving — so the device can still
  be entered, reconfigured and reflashed instead of needing BOOTSEL.
- **A fault on an unconfirmed image reboots instead**: 2 s after the fault the application
  resets and MCUboot rolls back. A flashable fault state is no reason to keep a broken build.
- **An unexpected fatal error reboots**: an assert, an exception or the vendored radio HAL's
  `k_oops()` (BUSY never arrives) is not something to sit in on a device whose only other way in
  is the bootloader. `k_sys_fatal_error_handler()` flushes the log and resets; the coredump is
  already in flash by then (`z_fatal_error()` writes it before the handler runs). On an
  unconfirmed image that reset is MCUboot's rollback, which is what makes "the radio did not come
  up after an upgrade" an upgrade failure.
- **NVS failure is survivable**: store errors are logged and the device comes up unconfigured
  and off the air rather than guessing defaults.
- **Entropy is checked at boot**: a dead TRNG stops the boot before any identity is derived.
- **The watchdog** (`CONFIG_ZLRS_APP_WATCHDOG`, on in every image) is armed in `PRE_KERNEL_1`
  and fed from the blocking loops, so a stuck main loop counts as dead too. It turns a hang into
  a reset; the rollback verdict is MCUboot's confirm bit.
- **Regulatory and security mistakes are visible**: a region the radio design cannot cover, a
  missing root key and a lost peer are all distinct LED states ([10-ux](10-ux.md)).

## 7. The log store

A device that reboots on its own is the one case a console cannot help with: the interesting lines
are the ones before the reset, and a bench board that loses its USB leaves nothing behind. The
firmware therefore keeps a log store in flash, and it is always on (no switch):

- **Zephyr's own pieces**: `LOG_BACKEND_FS` writes processed log records to
  LittleFS, alongside the console. Eight 128 KB files, the oldest overwritten
  (`CONFIG_LOG_BACKEND_FS_OVERWRITE`), so the store is a ring that needs no maintenance in the
  application.
- **Flush outside radio slots**: logging is deferred into an 8 KiB RAM queue, with the newest
  messages retained on overflow. The application drains it before starting TIMER1 and while
  management mode pauses radio work. RP2350 flash writes mask IRQs, so a background flash logger
  cannot run during normal slots. Debug console logs are deferred too; direct shell diagnostics
  remain available. An unexpected power loss or watchdog reset can lose the unflushed queue;
  fatal handling attempts a panic flush and coredump.
- **LittleFS owns the hard parts**: power-fail safety and wear leveling. The application only
  mounts the partition (before the log core enables its backends, see the init priorities in
  `app/src/storage/log.c`) and, on a partition nothing has written yet, erases and mounts it
  once.
- **The partition is a build-time property**: boards without a `log_partition` node still build;
  they log to the console only and say so once at boot.
- **Export is a host-side operation**: the store is read over the management port, so it works on a
  board whose radio and console are both misbehaving. `zlrs.py logs` dumps the files to a
  directory; management mode pauses radio work and flushes pending logs before export.
  CDC warnings and errors remain enabled, while its per-packet INFO dumps are disabled:
  exporting bytes must not flood the same rotating store being read.

The coredump partition next to it takes the fault case that a log cannot describe (stack, PC,
registers), and `zlrs mcuboot` reports the image state MCUboot will act on.
