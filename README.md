# ZLRS

**CRSF control and telemetry, bidirectional UART data, or both over one wireless link.**

ZLRS supports control, telemetry and bidirectional UART data on the Semtech LR2021. Both ends use
the same firmware: choose TX or RX in configuration, connect over USB or UART, and pair your devices.

## Features

- **Flexible operation** — use CRSF control and telemetry, bidirectional UART data, or both together.
- **Two-way data transfer** — ARQ acknowledgements, retransmissions and reassembly for reliable delivery.
- **Link security** — authenticated pairing, fresh session keys, packet authentication and
  replay protection.
- **Dual-band operation** — sub-GHz and 2.4 GHz, with frequency hopping and configurable band roles.
- **Standalone data mode** — use 2.4 GHz exclusively for pairing and bidirectional serial data.
- **One firmware, two roles** — select TX or RX without rebuilding; settings and pairing survive reboot.
- **Simple pairing** — a shared binding phrase, with pairing available through the button or device tool.
- **USB management and updates** — discover devices, inspect status, change settings and upgrade from one CLI;
  upgrades include image verification, confirmation and rollback support.
- **Visible status** — an on-board LED shows link, pairing and device state.

## Get started

The current prototype uses a **Waveshare RP2350-Zero + Core2021**. See the
[hardware guide](doc/04-prototype.md) for wiring and the [installation guide](doc/03-platform.md)
for your first firmware installation.

### Install and update firmware

First, install MCUboot once: hold **BOOTSEL** while connecting USB, then copy the bootloader
UF2 (`mcuboot/zephyr/zephyr.uf2` in your build output) to the USB drive. The board restarts in
MCUboot recovery mode.

With one device connected, install the signed application image through USB:

```shell
python3 tools/zlrs.py update --no-trigger --image <signed.bin>
```

For later updates, use the running device's serial:

```shell
python3 tools/zlrs.py update --serial <serial> --image <signed.bin>
```

The tool locates the device through reboot, uploads the image, verifies it and confirms the
upgrade. With multiple devices connected, use `--serial <serial>` for the first install too.

### Configure and pair

```shell
python3 tools/zlrs.py list
python3 tools/zlrs.py status --serial <serial>
python3 tools/zlrs.py set-role rx --serial <serial>
python3 tools/zlrs.py set-phrase "my binding phrase" --serial <serial>
```

Set the same phrase on both devices, choose their TX/RX roles and reboot. Pair them with the
button or `python3 tools/zlrs.py pair --serial <serial>`. The
[configuration guide](doc/08-firmware.md) covers UART settings, regions and failsafe settings.

Run `python3 tools/zlrs.py --help` for all commands.

### Build from source

With the Zephyr SDK installed, run from this repository:

```shell
tools/setup-zephyr.sh
export ZLRS_BOARD=rp2350_zero_core2021/rp2350a/m33/lf
source tools/zephyr-env.sh
zlrs-build
```

Use the `hf` target for an HF module. Build requirements and flashing steps are in the
[installation guide](doc/03-platform.md).

## Learn more

- [Product overview](doc/01-product.md) — use cases and operating modes.
- [Hardware reference](boards/zlrs/rp2350_zero_core2021/README.md) — boards, pins and peripherals.
- [Firmware and configuration](doc/08-firmware.md) — device operation and updates.
- [Architecture](doc/07-architecture.md), [protocol](doc/09-protocol.md) and
  [security](doc/10-security.md) — implementation details.
- [Development and debugging](doc/12-debug.md) — developer tools and diagnostics.
- [Roadmap and hardware validation](doc/13-roadmap.md) — current results and remaining work.

ZLRS is under active development and uses its own protocol; it is not compatible with ExpressLRS.
