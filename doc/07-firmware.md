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

`set-band auto|2.4|subg` selects automatic switching or a fixed DATA band; set the same mode and SubG region on both devices and reboot. New devices default to `auto`; saved fixed-band choices remain valid. Automatic mode prefers 2.4 GHz, falls back to SubG after sustained poor quality or loss, and probes 2.4 GHz after 60 seconds on SubG. DATA queues and the session survive a band change. Switching and recovery timing are specified in [protocol](08-protocol.md#自动跨频段切换). `set-region` must match the board's LF/HF sub-G SKU. `set-power` stores the regional TX power ceiling. The DATA link adjusts its active power from peer RSSI and link quality while feedback is fresh, then returns to the ceiling on feedback loss. `set-baud`, `set-profile`, `erase-pair`, `new-dev-id` and `factory-reset` are also available. Settings are persistent; band, role, region, power ceiling, profile and UART baud changes take effect after reboot.

The button starts pairing. A management session pauses radio operation; it exits on command or idle timeout. During that session DATA bytes cannot cross the radio.

Dynamic power aims for 99–100% measured LQ. Fresh feedback below 99%, or RSSI below -95 dBm, raises power; 99% holds it. Lowering requires 100% LQ and RSSI above -70 dBm. Three consecutive fresh feedback observations qualify each 3 dB step. Raises wait at least 2 seconds between changes; lowers wait at least 40 seconds, approximately one idle quality-window duration on the shipped bands. The lowering wait starts when feedback becomes available and restarts after feedback loss. Missing feedback for 1 second restores the ceiling. These thresholds are product policy; RF calibration and controlled interference tests remain necessary to characterize performance.

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

NVS record 0 stores the layout marker; records 1–9 are the explicit band, baud, region, power, role, profile, key, identity and pairing record IDs in the storage schema. A missing or invalid marker clears the settings area before loading defaults. Factory reset erases the whole settings area and writes a fresh marker.
