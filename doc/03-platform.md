# Hardware platform

What zlrs runs on: target shapes, board, build targets, toolchain, image layout and flashing. Ports and operating modes are in [08-firmware](08-firmware.md); the physical builds in [04-prototype](04-prototype.md) and [05-carrier](05-carrier.md).

## 1. Target shapes

| Shape | What it is |
| --- | --- |
| **Prototype** | a Waveshare RP2350-Zero and a Waveshare Core2021 wired together, exposing USB-C only; see [04-prototype](04-prototype.md) |
| **Carrier board** | the same wiring turned into a PCB exposing USB-C, a button and two 6P serial ports; see [05-carrier](05-carrier.md) |
| **Lightweight** | no native USB; one UART port, control or data by configuration; see [08-firmware](08-firmware.md) 4 |

All three shapes share one firmware and one board definition; only devicetree and Kconfig differ.

## 2. Board

| Item | Choice |
| --- | --- |
| Radio | Semtech LR2021 (QFN32), SPI + BUSY + DIO; parameters in [06-radio](06-radio.md) |
| MCU board | Waveshare RP2350-Zero: RP2350A (Cortex-M33), 4 MB QSPI flash, native USB, on-board WS2812B |
| Radio module | Waveshare Core2021, with HF (850-930 MHz) and LF (410-510 MHz) sub-G SKUs |
| Board definition | `boards/zlrs/rp2350_zero_core2021/`: pins, aliases, SKU calibration tables, flash partitions |
| Zephyr | v4.4.0 |
| Radio stack | `usp_zephyr` plus `usp` v1.1.2-feature-202604 (the RAC/RALF layers) |
| Repository topology | T2: zlrs is the manifest repository and holds the application |

Pins, aliases and partitions are defined in the [board README](../boards/zlrs/rp2350_zero_core2021/README.md).

## 3. Build targets

Four targets: two SKUs times with or without a bootloader.

| Target | Notes |
| --- | --- |
| `rp2350_zero_core2021/rp2350a/m33/hf` | HF SKU (868 / 915), the application sits directly in flash |
| `rp2350_zero_core2021/rp2350a/m33/lf` | LF SKU (470 / 433), the application sits directly in flash |
| `.../m33/hf/mcuboot` | the same plus MCUboot and two-slot partitions; build with `--sysbuild` |
| `.../m33/lf/mcuboot` | the same |

`west boards` also lists the same SoC's `rp2350a/hazard3` qualifier; there is no dts for it here (no RISC-V cores).

## 4. Image layout

**Without a bootloader**: 1 MB code plus 3 MB storage (NVS: root key, device id, pairing record, failsafe values, baud rate).

**With MCUboot**: mcuboot 128 KB, slot0 832 KB, slot1 832 KB and 2.25 MB storage; the signed image lives in slot0 (flash address `0x10020000`). Boot chain: RP2350 bootrom -> MCUboot (signature check) -> slot0, with `swap using move` for slot swaps (`swap using offset` goes in but never comes back on this board) and rollback after a failed upgrade.

Offsets and sizes: [board README](../boards/zlrs/rp2350_zero_core2021/README.md#4-partitions-and-boot). Upgrade flow and recovery entries: [08-firmware](08-firmware.md) 6.

## 5. Toolchain and build

The workspace comes from this repository's `west.yml` (Zephyr v4.4.0, usp_zephyr, usp) and the toolchain is pinned to Zephyr SDK 1.0.0 (a distro `arm-none-eabi-gcc` has newlib without retargetable locking and trips a static assertion):

```shell
tools/setup-zephyr.sh        # about 5 GB the first time
source tools/zephyr-env.sh   # once per new shell
zlrs-build                     # = west build -b $ZLRS_BOARD $ZLRS_APP
```

The host tests need no workspace: `make -C tests/host run` compiles the very same sources from `core/`. The full command and dependency list is in the [README](../README.md).

## 6. Flashing

- Without a bootloader: BOOTSEL plus UF2 (`west flash --runner uf2`) or SWD.
- With MCUboot: two BOOTSEL rounds the first time (the bootloader, then `zephyr.signed.uf2`); afterwards serial recovery does it in one command, `python3 tools/zlrs-update.py --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04` (the config port).

Recovery ports, image confirmation and rollback are described in [08-firmware](08-firmware.md) 6.
