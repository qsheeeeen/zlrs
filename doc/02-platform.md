# Hardware platform

What ZLRS runs on: target shapes, board, build targets, image layout, toolchain and flashing.
Ports and operating modes are in [07-firmware](07-firmware.md); the physical builds in
[03-prototype](03-prototype.md) and [04-carrier](04-carrier.md).

## 1. Target shapes

| Shape | What it is |
| --- | --- |
| **Prototype** | a Waveshare RP2350-Zero and a Waveshare Core2021 wired together, exposing USB-C only; see [03-prototype](03-prototype.md) |
| **Carrier board** | the same wiring turned into a PCB exposing USB-C, a button and one 6P DATA port; see [04-carrier](04-carrier.md) |
| **Single-port** | a smaller board with one DATA UART and no native USB; a future variant |

All three shapes share one firmware. What differs is devicetree (which ports and peripherals
exist) and Kconfig choices.

## 2. Board

| Item | Choice |
| --- | --- |
| Radio | Semtech LR2021 (QFN32), SPI + BUSY + DIO; parameters in [05-radio](05-radio.md) |
| MCU board | Waveshare RP2350-Zero: RP2350A (Cortex-M33), 4 MB QSPI flash, native USB, on-board WS2812B |
| Radio module | Waveshare Core2021, with HF (850-930 MHz) and LF (410-510 MHz) sub-G SKUs |
| Board definition | `boards/zlrs/rp2350_zero_core2021/`: pins, aliases, SKU calibration tables, flash partitions |
| Zephyr | v4.4.0 |
| Radio stack | `usp_zephyr` plus `usp` v1.2.1 (the RAC/RALF layers) |
| Repository topology | T2: ZLRS is the manifest repository and holds the application |

Pins, aliases and partitions are defined in the
[board README](../boards/zlrs/rp2350_zero_core2021/README.md).

### 2.1 How the software sees that hardware

```text
hardware = MCU (A) × radio chip (B) × radio design (LF/HF, band, coverage, calibration) × ports
```

| Axis | What it fixes | Where it is declared |
| --- | --- | --- |
| MCU | core, memory, available Zephyr devices (SPI, UART, USB, NVS, watchdog, TRNG) | board devicetree plus SoC definitions |
| Radio chip | register interface, modulation set, driver | `compatible` on the radio node, driver under `drivers/` or the vendor module |
| Radio design | frequency coverage, matching network, power and calibration tables, default region | properties on the `zlrs_rf` node of the board |
| Ports | how many logical ports exist and what each is wired to | aliases and nodes in the board devicetree |

The DATA port can have a UART endpoint, a USB CDC endpoint, or both. The management CDC is independent and never carries application bytes. The radio band is selected at runtime configuration and is unrelated to which serial endpoint is active.

| Port | Devicetree aliases | Purpose |
| --- | --- | --- |
| DATA | `zlrs-data-uart` / `zlrs-data-cdc` | Bidirectional byte stream. |
| Management | `zlrs-mgmt-cdc` | Configuration, status and update commands. |

## 3. Build targets

Two targets, one per sub-G SKU, and both are bootloader targets (an image that cannot be updated
through the bootloader has no reason to exist, see [07-firmware](07-firmware.md)):

| Target | Notes |
| --- | --- |
| `rp2350_zero_core2021/rp2350a/m33/hf` | HF SKU (868 / 915), application and bootloader, built with `--sysbuild` |
| `rp2350_zero_core2021/rp2350a/m33/lf` | LF SKU (470 / 433), the same |

There is no qualifier for the bootloader in the target name: the bootloader is part of every image
this board builds, not a variant of it.

`west boards` also lists the same SoC's `rp2350a/hazard3` qualifier; there is no dts for it here
(no RISC-V cores).

## 4. Image layout

There is one layout, `rp2350_zero_core2021_partitions.dtsi`, and both images are built from it: the
application and the bootloader include the same file, from the same board target, so the two can
never disagree about where a slot is. MCUboot (128 KB), slot0 (832 KB at `0x020000`), slot1
(832 KB at `0x0F0000`), the log store (1 MB at `0x1C0000`), a coredump partition (128 KB at
`0x2C0000`) and 32 KB of NVS. The signed application lives in slot0 and upgrades land in slot1.
Boot chain: RP2350 bootrom -> MCUboot (signature check) -> slot0, with `swap using move` for slot
swaps (`swap using offset` goes in but never comes back on this board) and rollback after a failed
upgrade.

NVS keeps the root key, device id, pairing record, baud rate, region, power
ceiling, role, security profile and DATA band — ten keys, under 100 B in total, which is why NVS is small
and the flash around it goes to the log store ([07-firmware](07-firmware.md)).

Offsets and sizes: [board README]
(../boards/zlrs/rp2350_zero_core2021/README.md#4-partitions-and-boot).
Upgrade flow and recovery entries: [07-firmware](07-firmware.md).

## 5. Toolchain and build

The workspace comes from this repository's `west.yml` (Zephyr v4.4.0, usp_zephyr, usp) and the
toolchain is pinned to Zephyr SDK 1.0.0 (a distro `arm-none-eabi-gcc` has newlib without
retargetable locking and trips a static assertion):

```shell
tools/setup-zephyr.sh        # about 5 GB the first time
source tools/zephyr-env.sh   # once per new shell
zlrs-build                   # = west build --sysbuild -b $ZLRS_BOARD $ZLRS_APP
```

The host tests need no workspace. Configure and build them with CMake, then run them through
CTest; they compile the same `core/` sources against a host shim. The full command and dependency
list is in the [README](../README.md).

## 6. Flashing

- The bootloader goes on once: hold BOOTSEL and drag its UF2, or use SWD (openocd, probe-rs,
  jlink). It never updates itself afterwards.
- With MCUboot: BOOTSEL carries the **bootloader** image only. The board then parks in serial
  recovery (no application yet) and the application goes in through that recovery port; every
  later update is the same command aimed at the running application's management port:

```shell
python3 tools/zlrs.py update --serial <serial> \
  --recovery-port /dev/serial/by-id/usb-zlrs_ZLRS_MCUBOOT_<serial>-if00 --no-trigger \
  --image build/app/zephyr/zephyr.signed.bin        # first flash
python3 tools/zlrs.py update --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if02
# later updates
```

The application is never dragged on as a UF2: that path has no rollback
([07-firmware](07-firmware.md)).

Recovery ports, image confirmation and rollback are described in [07-firmware](07-firmware.md).

## 7. Adding hardware

1. Copy the closest board directory under `boards/<vendor>/<board>/`.
2. Fill in the devicetree: MCU references, the radio node, the `zlrs_rf` design node (band,
   coverage, `default-region`, calibration tables) and the aliases for the ports and peripherals
   this hardware has.
3. Write the board README with the pin and alias table and the capability line the firmware
   prints at boot.
4. Add the variant to the CI matrix (`.github/workflows/firmware.yml`).
5. If the radio chip or the MCU is new, add the driver and binding it needs; `core/` does not
   change.

For the existing MCU/radio, describe hardware differences in devicetree. Application modules
read their own properties and aliases directly, and assemble the domain values they need.
Add a new property to the relevant binding if a module needs another hardware fact;
keep required-device checks with the module that uses that device.
