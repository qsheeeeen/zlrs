# Firmware behaviour

What the firmware looks like from outside: USB and ports, operating modes, runtime
configuration, build-time choices, boot and updates, and the error-handling rules.
Architecture: [07-architecture](07-architecture.md); protocol: [09-protocol](09-protocol.md);
security: [10-security](10-security.md); debugging: [12-debug](12-debug.md).

## 1. USB and ports

The firmware presents three CDCs by default:

| CDC | Purpose | Its UART endpoint |
| --- | --- | --- |
| control | CRSF: channels down, telemetry up | UART0 (GP0/GP1, 420000) |
| data | the passthrough stream | UART1 (GP8/GP9, 115200 by default, changeable in NVS) |
| **config** | the config protocol: enter config mode, read and write parameters | - |

Debug builds add one more **console** CDC (kernel log plus shell, see [12-debug](12-debug.md)).
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
the first frame is never lost to that lag (`port_active()` in `app/src/ports.c`).

The two links choose their endpoints independently. The config CDC carries no passthrough
traffic, only the config protocol; a board without that CDC simply has no config mode.

## 2. Operating modes

| Mode | Links | Config CDC | Notes |
| --- | --- | --- | --- |
| **application** | normal traffic | idle | the usual state |
| **config** | **radio stopped entirely** (no RX, no TX) | runs the config protocol | entered by a tool command, left on `EXIT` or after 300 s idle |
| serial recovery | no application | - | bootloader mode, see 5 |

While in config mode:

- the radio neither receives nor transmits and the link state freezes;
- bytes arriving on the data port are **dropped and counted**, never buffered; normal traffic
  resumes on exit;
- the status LED shows white at 1 Hz, after half a second (tool round-trips do not flash);
- the watchdog is disarmed for the session — PBKDF2 on a passphrase takes tens of seconds on
  this MCU and an 8 s watchdog would reset mid-way — and armed again on exit.

## 3. Runtime configuration

**Every user-visible setting is configured at runtime, with no firmware rebuild**:

| Parameter | Notes |
| --- | --- |
| root key / passphrase | the device derives the root key from a passphrase with PBKDF2 (100,000 rounds), or you write 64 hex characters; takes effect at once |
| failsafe values | a 22-byte CRSF channel payload, shipped to the peer during pairing |
| data port baud rate | 9600-4000000, effective on the next boot |
| region | 0 = FCC915, 1 = EU868, 2 = CN470, 3 = CN433; a fresh board starts on its radio design's `default-region`, and a write is checked against that design (band and frequency coverage) and refused on a mismatch; effective on the next boot |
| TX power ceiling | never above the chosen region's limit; effective on the next boot |
| role TX / RX | one firmware runs either end; effective on the next boot |
| device id | generated from the TRNG on the first boot; `new-dev-id` replaces it at once and erases the pairing record |
| security profile | `p1` is the production profile, `p0` (no tag) is refused unless this is a debug image, `p2` is not implemented yet; effective on the next boot, and the peer has to run the same profile |
| pairing | start pairing at once, erase the pairing record (next boot) |
| factory reset | wipes NVS (keys, pairing, failsafe, baud, region, power, role, profile); effective on the next boot |

The command set is in [10-security](10-security.md) 9.

Who decides what:

| Decision | When | How |
| --- | --- | --- |
| Which links a build supports | build time | Kconfig shape switches, gated by capabilities |
| Which port carries which link on a two-port board | build time | devicetree aliases |
| Which link the single port carries *(design, not implemented)* | runtime, default from devicetree | config protocol, stored in NVS |
| Band role and control modulation | build time | Kconfig choices |
| Role TX / RX, security profile | runtime, default at build time | `set-role` / `set-profile`, stored in NVS |
| Region, power ceiling, baud rate, failsafe values, pairing | runtime | config protocol, stored in NVS |
| Rate tier, frame rate, telemetry period | build time today | the ledger in [13-roadmap](13-roadmap.md) tracks the missing commands |
| Hardware presence (ports, config channel, button, LED, watchdog, TRNG, radio design) | devicetree | `app/src/caps.h` |
| Bench isolation (`RADIO=n`, hopping off, P0, …) | build time, debug images only | `ZLRS_APP_DEBUG*`, see [12-debug](12-debug.md) 3 |

Two properties follow and are worth stating explicitly: **one image runs both roles** (a field
is flashed with the same file, roles are assigned during configuration), and **defaults are
build-time while behaviour is field-time** (changing how a deployed device behaves never needs a
new build).

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
| feature enable | ARQ, sub-band mixing, rate adaptation, dynamic power, watchdog | see the ledger in [13-roadmap](13-roadmap.md) |
| bench isolation | debug images only | radio off, hopping off, session handshake off, telemetry off, P0; see [12-debug](12-debug.md) 3 |

**One firmware has to fit several hardware configurations**; that is a hard requirement:

- USB must be trimmable: the single-port variant has no native USB;
- the data UART and the button have to be optional too;
- this is expressed in devicetree, not in `#ifdef`s.

The application only knows aliases (`zlrs-control-uart`, `zlrs-control-cdc`, `zlrs-data-uart`,
`zlrs-data-cdc`, `zlrs-config-cdc`, `zlrs-button`, `zlrs-led`); a missing alias means "this board
has no such port or peripheral" and everything still builds. Whether there is a CRSF port, a
config port, a button or a status LED is therefore a devicetree fact (`app/src/caps.h`), not a
switch — a switch could only disagree with the board. `ZLRS_APP_CORE` enables the pure C
libraries every image carries (`ZLRS_PROTO`, `ZLRS_LINK`, `ZLRS_REGION`, `ZLRS_CRSF`,
`ZLRS_CONFIG`, `ZLRS_LED`).

That header is the whole hardware interface:

```c
/* ports */
#define ZLRS_PORT_CTRL_FIXED   (DT_NODE_EXISTS(DT_ALIAS(zlrs_control_uart)) || DT_NODE_EXISTS(DT_ALIAS(zlrs_control_cdc)))
#define ZLRS_PORT_DATA_FIXED   (DT_NODE_EXISTS(DT_ALIAS(zlrs_data_uart))    || DT_NODE_EXISTS(DT_ALIAS(zlrs_data_cdc)))
#define ZLRS_PORT_CONFIG       DT_NODE_EXISTS(DT_ALIAS(zlrs_config_cdc))

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
| a config CDC | configuration protocol, config mode, recovery entry | using it as a link endpoint |
| a button | long press to pair | — (pairing from the tool always works) |
| a status LED | the LED language ([11-ux](11-ux.md)) | — |
| NVS | keys, pairing, failsafe, baud, region, power, role, profile | **required**: a board without NVS is rejected |
| a hardware watchdog | reset on a hang, rollback assistance | — |

**Each image builds its own USB stack** (`subsys/usb`, `CONFIG_ZLRS_USB`, enabled separately by
the application and the bootloader configs): a `zephyr,cdc-acm-uart` node in devicetree only
says how many CDCs exist; the image builds a context, registers the classes and calls
`usbd_enable()`. Zephyr's `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` registers only the first CDC
while this firmware has several, so that switch stays off.

## 5. Boot, recovery and updates

Partitions and the boot chain are in [03-platform](03-platform.md) 4. There are three ways into
serial recovery, none needing the button:

| Entry | When it applies |
| --- | --- |
| from the application | the recovery command on the config port (debug builds can also use `zlrs recovery` in the shell), which sets the boot-mode flag and reboots |
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
   unconfirmed (60 s after boot by default, `ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS`), and the
   bootloader's watchdog resets it when it hangs. There is no switch for the confirmation
   mechanism: an image that runs under MCUboot is always confirmed by the host or rolled back.

One command updates a device:
`python3 tools/zlrs-update.py --port <application config port>` — trigger recovery, wait for
the recovery port, read the active slot, write the other one and mark it test, wait for the
application, check the image hash and send `confirm`. The tool writes pending but **never
permanent**. A broken *application* is recovered through the bootloader too (serial recovery is
always reachable); only a broken *bootloader* needs BOOTSEL and a UF2 drag
(see [12-debug](12-debug.md)).
Production images use their own signing key; changing that key means reflashing the bootloader.

**An image dragged onto BOOTSEL has no rollback**: it goes straight into slot0 with an empty
trailer, which MCUboot treats as the initial image — it boots it without asking for a
confirmation and there is nothing to revert to; an app that resets during init then simply
loops. **The application is therefore never installed as a UF2**: BOOTSEL carries the bootloader
image only, and every application image — first install included — goes in through the bootloader
(serial recovery on a blank board, the config port afterwards). That is the only path on which
the signature check, the confirmation and the rollback apply.

## 6. Error handling

- **A fatal fault stops the air, not the device**: a region/radio-design mismatch, a PSA
  failure or an entropy self-test failure drops into `fault_loop()`, which shows the fast red
  alarm, keeps feeding the watchdog and keeps the config port serving — so the device can still
  be entered, reconfigured and reflashed instead of needing BOOTSEL.
- **A fault on an unconfirmed image reboots instead**: 2 s after the fault the application
  resets and MCUboot rolls back. A flashable fault state is no reason to keep a broken build.
- **NVS failure is survivable**: store errors are logged and the device comes up unconfigured
  and off the air rather than guessing defaults.
- **Entropy is checked at boot**: a dead TRNG stops the boot before any identity is derived.
- **The watchdog** (debug images today, `CONFIG_ZLRS_APP_WATCHDOG`) is armed in `PRE_KERNEL_1`
  and fed from the blocking loops, so a stuck main loop counts as dead too. It turns a hang into
  a reset; the rollback verdict is MCUboot's confirm bit.
- **Regulatory and security mistakes are visible**: a region the radio design cannot cover, a
  missing root key and a lost peer are all distinct LED states ([11-ux](11-ux.md)).

## 7. The log store

A device that reboots on its own is the one case a console cannot help with: the interesting lines
are the ones before the reset, and a bench board that loses its USB leaves nothing behind. The
firmware therefore keeps a log store in flash, and it is always on (no switch):

- **Zephyr's own pieces, not a home-grown ring**: `LOG_BACKEND_FS` writes every log record to
  LittleFS, alongside the console. Eight 128 KB files, the oldest overwritten
  (`CONFIG_LOG_BACKEND_FS_OVERWRITE`), so the store is a ring that needs no maintenance in the
  application.
- **LittleFS owns the hard parts**: power-fail safety and wear leveling. The application only
  mounts the partition (before the log core enables its backends, see the init priorities in
  `app/src/log_store.c`) and, on a partition nothing has written yet, erases and mounts it once.
- **The partition is a build-time property**: boards without a `log_partition` node still build;
  they log to the console only and say so once at boot.
- **Export is a host-side operation**: the store is read over the config port, so it works on a
  board whose radio and console are both misbehaving. `zlrs-config.py logs` dumps the files to a
  directory; nothing in the firmware has to stop doing its job for the export.

The coredump partition next to it takes the fault case that a log cannot describe (stack, PC,
registers), and `zlrs mcuboot` reports the image state MCUboot will act on.
