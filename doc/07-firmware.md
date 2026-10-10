# Firmware behaviour

## Ports

The application exposes a DATA USB CDC at interface `if00` and a management CDC at `if02`; a debug build also enables the console CDC. The board may expose a DATA UART. USB and UART belong to the same byte-stream service, with the active endpoint selected by the data module. The management port never carries application data. MCUboot recovery has its own `if00` port.

## Configuration

Use `python3 tools/zlrs.py list` to find devices and `status --serial <serial>` to inspect one. Both devices need the same binding phrase and opposite roles:

```shell
python3 tools/zlrs.py set-role tx --serial <TX-serial>
python3 tools/zlrs.py set-role rx --serial <RX-serial>
python3 tools/zlrs.py set-phrase 'shared phrase' --serial <TX-serial>
python3 tools/zlrs.py set-phrase 'shared phrase' --serial <RX-serial>
python3 tools/zlrs.py pair --serial <TX-serial>
python3 tools/zlrs.py pair --serial <RX-serial>
```

`set-band 2.4|subg` selects the DATA radio band; set the same value on both devices and reboot. `set-region` must match the board's LF/HF sub-G SKU. `set-power` stores the regional TX power ceiling. The DATA link adjusts its active power from peer RSSI and link quality while feedback is fresh, then returns to the ceiling on feedback loss. `set-baud`, `set-profile`, `erase-pair`, `new-dev-id` and `factory-reset` are also available. Settings are persistent; `status` reports if a reboot is needed for an active value to change.

The button starts pairing. A management session pauses radio operation; it exits on command or idle timeout. During that session DATA bytes cannot cross the radio.

## Build and updates

Build the matching LF or HF target with `source tools/zephyr-env.sh` and `zlrs-build`. The signed application image is `../build/app/zephyr/zephyr.signed.bin`. Install MCUboot once with its UF2 through BOOTSEL, then send every application image through serial recovery:

```shell
python3 tools/zlrs.py update --no-trigger --image ../build/app/zephyr/zephyr.signed.bin
python3 tools/zlrs.py update --serial <serial> --image ../build/app/zephyr/zephyr.signed.bin
```

The updater triggers recovery, uploads to the inactive slot, verifies the running hash and confirms the image. An unconfirmed image rolls back. The MCUboot partition layout, NVS, log ring and crash partition are defined by the board devicetree. Use `logs` to export persistent logs. `recovery` explicitly reboots into the bootloader.

## Build-time choices

`app/Kconfig` controls the data rate, slot preparation, hopping, session handshake, ARQ, dynamic power and its floor, watchdog and debug features. Production defaults enable authentication, ARQ and dynamic DATA power. `app/debug.conf` and `app/debug.overlay` add console and bench diagnostics. RF capabilities are selected from the board devicetree; an absent endpoint remains absent rather than acquiring a software substitute.

See [security](09-security.md), [protocol](08-protocol.md) and [board definition](../boards/zlrs/rp2350_zero_core2021/README.md).
