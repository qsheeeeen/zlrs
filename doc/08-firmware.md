# Firmware behaviour

What the firmware looks like from outside: USB and ports, operating modes, runtime configuration, feature trimming, boot and updates, and the error-handling rules. Architecture: [07-architecture](07-architecture.md); protocol: [09-protocol](09-protocol.md); security: [10-security](10-security.md); debugging: [12-debug](12-debug.md).

## 1. USB and ports

The firmware presents three CDCs by default:

| CDC | Purpose | Its UART endpoint |
| --- | --- | --- |
| CRSF | the CRSF RC link | UART0 (GP0/GP1, 420000) |
| data | the passthrough link | UART1 (GP8/GP9, 115200 by default) |
| **config** | the config protocol: enter config mode, read and write parameters | - |

Debug builds add one more **console** CDC (kernel log plus shell, see [12-debug](12-debug.md)).

The name each mode announces over USB comes from `CONFIG_ZLRS_USB_PRODUCT`: `ZLRS` for the application and `ZLRS MCUBOOT` for the bootloader, and host scripts find ports under `/dev/serial/by-id/...`.

**Each link has two endpoints and picks one**: the CDC is used when the host has it open (DTR asserted) **or** it has ever received a host byte, otherwise the UART. The second rule exists for USB/IP forwarding (usbipd) and virtualised setups where `SET_CONTROL_LINE_STATE` may never arrive: trusting DTR alone would leave the CDC unselected and host bytes would be dropped silently as "not the active endpoint", with nothing logged. The ISR records "a byte arrived" **before** checking the active endpoint, so the first frame is never lost to that lag (`port_active()` in `app/src/ports.c`).

The two links choose their endpoints independently; the config CDC carries no passthrough traffic, only the config protocol.

## 2. Operating modes

| Mode | Links | Config CDC | Notes |
| --- | --- | --- | --- |
| **application** | normal traffic | idle | the usual state |
| **config** | **radio stopped entirely** (no RX, no TX) | runs the config protocol | entered by a tool command, left on `EXIT` or timeout |
| serial recovery | no application | - | bootloader mode, see 6 |

While in config mode:

- the radio neither receives nor transmits and the link state freezes;
- bytes arriving on the data port are **dropped and counted**, never buffered, and normal traffic resumes on exit;
- the status LED shows white at 1 Hz (see [11-ux](11-ux.md));
- the watchdog is disarmed for the session (PBKDF2 on a passphrase takes tens of seconds and an 8 s watchdog would reset it) and armed again on exit.

## 3. Runtime configuration

**Every user-visible setting is configured at runtime, with no firmware rebuild**:

| Parameter | Notes |
| --- | --- |
| root key / passphrase | the device derives the root key from a passphrase with PBKDF2, or you can write a 64-character hex key directly |
| failsafe values | a 22-byte CRSF channel payload, shipped to the peer during pairing |
| data port baud rate | effective on the next boot |
| region | 0 = FCC915, 1 = EU868, 2 = CN470, 3 = CN433; a fresh board starts on its radio design's `default-region`, and writes are checked against that design (band and frequency coverage) and refused on a mismatch (see [09-protocol](09-protocol.md) 5.2); **effective on the next boot** |
| TX power ceiling | never above the chosen region's limit; **effective on the next boot** |
| pairing | start pairing (immediately), erase pairing (next boot) |
| factory reset | wipes NVS (keys, pairing, failsafe, baud, region, power); effective on the next boot |

The role (TX or RX), the band shape and the modulation stay compile-time settings (`CONFIG_ZLRS_APP_ROLE_TX`, `BAND_ROLE_*`, `CONTROL_MODULATION_*`): they decide which timing the firmware runs and are not user parameters.

## 4. Feature trimming

One firmware has to fit several hardware configurations; that is a hard requirement:

- **USB must be trimmable**: the lightweight variant has no native USB;
- the data UART has to be optional too;
- express it with Kconfig and devicetree so the application probes DT instead of drowning in `#ifdef`s.

The approach: the application only knows aliases (`zlrs-control-uart`, `zlrs-control-cdc`, `zlrs-data-uart`, `zlrs-data-cdc`, `zlrs-button`, `zlrs-led`); a missing alias means "this link has no such port" and everything still builds. Library switches follow the application switches: turning off `ZLRS_APP_CONFIG_MODE` or `ZLRS_APP_STATUS_LED` trims `ZLRS_CONFIG` or `ZLRS_LED` with them. `ZLRS_APP_CORE` enables what the application always needs (`ZLRS_PROTO`, `ZLRS_LINK`, `ZLRS_REGION`, `ZLRS_CRSF`); the CRSF codec is compiled in with the port layer, so `ZLRS_APP_CRSF_PORT=n` only skips initialising that port and leaves the code and library in place (a few hundred bytes of pure C).

**Each image builds its own USB stack** (`subsys/usb`, `CONFIG_ZLRS_USB`, enabled separately by the application and the bootloader configs): a `zephyr,cdc-acm-uart` node in devicetree only says how many CDCs exist, it does not bring the device up — the image builds a context, registers the classes and calls `usbd_enable()`. Zephyr's own `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` registers only the first CDC while this firmware has several, so that switch stays off. Lightweight hardware simply deletes `usbd` and the CDC aliases from devicetree.

**One buffer pool per CDC** (`CONFIG_USBD_CDC_ACM_BUF_POOL=y`): several CDCs on a full-speed device fight over the UDC's shared pool, and a failed RX allocation is never retried, so TX works while RX dies without a word.

## 5. Button

The button has one action: **a long press starts pairing** (1 s by default). It is standard equipment, one switch (SW1) on the carrier board, and a jumper from GP12 to GND on the prototype. Details in [11-ux](11-ux.md).

## 6. Boot, recovery and updates

The boot chain and partitions are in [03-platform](03-platform.md) 4.

There are three ways into serial recovery, none of them needing the button:

| Entry | When it applies |
| --- | --- |
| from the application | the recovery command in the config protocol (debug builds can also use `zlrs recovery` in the shell), which sets the boot-mode flag and reboots |
| boot window | a valid mcumgr frame within 2 s of any reset (`CONFIG_BOOT_SERIAL_WAIT_FOR_DFU_TIMEOUT`) |
| no bootable image | a failed signature check or an empty image parks in recovery |

The boot-mode flag lives in 4 bytes at the start of SRAM (retained RAM) and both the application and the bootloader use the same DT. `CONFIG_BOOT_SERIAL_PIN_RESET` is deliberately off: on RP2350 POWMAN's `HAD_RUN` bit is mapped to `RESET_PIN`, which is broader than "the reset button was pressed".

The window is 2 s rather than something shorter: after a confirm timeout or a watchdog reset that window lets the host get back into recovery and reflash on its own. The cost is that every boot waits that long, which is a fair price for a button-free path.

Recovery is **USB only**: in bootloader mode USB shows two CDCs (recovery first, console second) and neither CRSF nor config is present. The application is `ZLRS`, the bootloader `ZLRS MCUBOOT`, and scripts find ports under `/dev/serial/by-id/...`.

**Updates**: after the one-off flash (BOOTSEL for the bootloader plus the first application image), later firmware updates **never need the board touched**, including when the new image does not start, USB never comes up, the host never confirms or the image is corrupt. Four invariants:

1. **The bootloader never updates itself** (only BOOTSEL writes that 128 KB at `0x000000`). It is the fixed recovery entry; replacing it needs the button.
2. **Updates always go to the inactive slot**: MCUboot's serial recovery upload defaults to the **primary** slot (`flash_area_id_from_multi_image_slot()` uses 0 for PRIMARY), which overwrites the running image with no pending flag and no rollback. `CONFIG_MCUBOOT_SERIAL_DIRECT_IMAGE_UPLOAD=y` therefore lets the tool name a physical slot, and the tool works out which one is inactive itself (in `image state-read` the active one is current, the other is the target; slot numbers start at 1).
3. **The tool sets pending, only the application sets permanent**: the application calls `boot_write_img_confirmed()` after the image runs and the host sends its confirm command.
4. **Every unconfirmed path has to reset on its own**: the application resets when it is alive but unconfirmed, and the bootloader's watchdog resets it when it hangs, even before USB comes up.

The update sequence (`tools/zlrs-update.py` runs it end to end):

| # | Who | What |
| --- | --- | --- |
| 1 | tool | sends the recovery command to the application's config port (sets the boot-mode flag) |
| 2 | application | answers and reboots |
| 3 | MCUboot | enters serial recovery and brings USB up (two CDCs) |
| 4 | tool | uploads the image to the **inactive slot** (after `image state-read`) and marks it pending (test) |
| 5 | MCUboot | resets, swaps (`swap using move`) and runs the new image, still unconfirmed |
| 6 | tool | waits for the new image's config port and compares `image-hash` with what it just sent |
| 7 | tool to application | sends `confirm`; the application calls `boot_write_img_confirmed()` and the image becomes permanent |
| 8 | tool | reports success plus the build string the device sends back (the `build` command) |

Failure modes:

| Failure | Found by | Automatic action | Button needed? |
| --- | --- | --- | --- |
| upload interrupted or image corrupt | MCUboot's signature check | no swap, parks in serial recovery | no, just resend |
| the new image hangs on boot | bootloader watchdog (30 s) | reset, unconfirmed, rollback | no |
| the new image runs but USB never enumerates | the application's confirm timeout (60 s) | application resets, rollback | no |
| the new image runs, the host never confirms | the application's confirm timeout | application resets, rollback | no |
| step 6 finds the device running something else | the tool | no confirm, waits for the rollback, reports failure | no |
| neither slot boots | MCUboot | parks in serial recovery | no, resend from the tool |
| the bootloader itself is corrupt | ROM | - | **yes**, accepted by design |

The rollback verdict lives in MCUboot's image trailer and the application only "confirms when it should": if `boot_is_img_confirmed()` is true it does nothing, if false it waits for the host (`ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS`, 60 s by default). An image written directly with BOOTSEL has an empty trailer, which MCUboot treats as an initial image and does not require a confirmation for: that is the first flash.

**The watchdog** (`CONFIG_ZLRS_APP_WATCHDOG`, off by default) only turns a hang into a reset: the application arms it at `PRE_KERNEL_1` (before the entropy source, so a hung entropy probe still resets) and feeds it from the main loop and config sessions, while on the bootloader side `subsys/bootguard` (`CONFIG_ZLRS_BOOTGUARD`) clears the hardware enable bit first (the driver's `wdt_disable()` does nothing in the next boot, and without this the previous watchdog would interrupt serial recovery). MCUboot then re-arms it with `BOOT_WATCHDOG_TIMEOUT_MS` (30 s) and leaves it running across the jump into the application, so an application that hangs before arming its own still gets reset and rolled back.

One command updates a device: `python3 tools/zlrs-update.py --port <application config port>` (trigger recovery, wait for the recovery port, read the active slot, write the other one and mark it test, wait for the application, check `image-hash`, send `confirm`). The tool writes pending but **never permanent**: marking an image permanent before it has run cuts off the rollback path. Bricked boards go through BOOTSEL and a UF2 drag (see [12-debug](12-debug.md)). Production images use their own signing key, and changing that key means reflashing the bootloader.

## 7. Error-handling rules

A device that looks alive while some port died long ago is the most expensive trap there is, so:

| Layer | What it has | What it must do |
| --- | --- | --- |
| `core/` (pure C, host-testable) | inputs and outputs only, no devices, no logging | return error codes or booleans and **never print**; crypto always fails closed |
| `app/src/` (the Zephyr glue) | devices, logging, policy | check every return value and pick a response by consequence: degrade (port gone, use the other endpoint), downgrade (no NVS, stay off the air) or fatal (PSA init failed, `fault_loop()`) |
| image-level glue such as `subsys/usb` and `subsys/bootguard` | failure affects whether the whole board works | check them too, but default to non-fatal: USB is optional, so warn and keep running |

Specific rules:

- **Check returns down to the semantics**: `nvs_write()` returns the byte count on a write, 0 when the content already matched and a negative errno on failure, so only `rc == 0 || rc == len` is success.
- **Ignoring a return value needs a stated reason**: best-effort paths (passthrough and CRSF sends, where drops are counted) and cleanup (`psa_destroy_key`, `wdt_disable`).
- **When an API returns void, something else has to surface the failure**: `uart_irq_rx_enable()` cannot be checked, so rely on whether the callback was installed (`uart_irq_callback_user_data_set()` returns int and must be checked) plus timeouts and counters above.
- **Per-frame paths must not flood the log**: report a failure once, or only bump a counter that a status command shows.
