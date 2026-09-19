# Waveshare RP2350-Zero + Core2021 (board definition)

A Waveshare RP2350-Zero (RP2350A, 4 MB flash, Cortex-M33) carrying a Waveshare Core2021 (Semtech LR2021). The board definition fixes the radio wiring, both UARTs (CRSF and data), the three USB CDCs, the button and the flash partitions into devicetree; the application only knows `zlrs-*` aliases and never pin numbers.

The board is `rp2350_zero_core2021` rather than `rp2350_zero` because the Zephyr 4.4 tree already has `waveshare/rp2350_zero` (the bare board, no radio) and board names have to be unique; the difference here is a Core2021 plus a full wiring set, so the vendor is our own `zlrs`.

**The physical build and how it behaves in each mode are in [doc/04-prototype.md](../../../doc/04-prototype.md)**; this document covers the board definition only: pins and aliases, SKU calibration, partitions and build targets.

## 1. Pins and aliases

| Signal | RP2350-Zero | DT alias / node | Notes |
| --- | --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | `lora-transceiver` = `lora_core2021` | the radio bus, `spi-max-frequency` 16 MHz |
| SPI0 CS | GP5 | same node (`cs-gpios`) | a plain GPIO chip select, as on Semtech's own shield |
| BUSY | GP13 | same node (`busy-gpios`) | module busy, active high, internal pull-up |
| NRESET | GP14 | same node (`reset-gpios`) | reset, active low |
| DIO8 | GP15 | same node (`dios/dio@8`) | IRQ, active high, internal pull-down |
| UART0 TX / RX | GP0 / GP1 | `zlrs-control-uart` = `uart0` | CRSF at 420000, to the handset or flight controller |
| UART1 TX / RX | GP8 / GP9 | `zlrs-data-uart` = `uart1` | passthrough, 115200 by default (changeable in NVS) |
| Button | GP12 | `zlrs-button`, `smtc-user-button` | to ground with an internal pull-up; none fitted, for your own switch |
| WS2812B | GP16 | `zlrs-led`, `led-strip` | on the module, driven through PIO0; semantics in [doc/11-ux.md](../../../doc/11-ux.md) |
| USB-C (native USB) | - | `zephyr_udc0` = `usbd` | three CDCs: `cdc_acm_uart0` (`zlrs-control-cdc`), `cdc_acm_uart1` (`zlrs-data-cdc`), `cdc_acm_uart2` (`zlrs-config-cdc`); the debug overlay adds a fourth, the console `cdc_acm_uart3` |
| Watchdog | - | `watchdog0` = `wdt0` | armed only when `CONFIG_ZLRS_APP_WATCHDOG` is on |

The radio bus and the three control lines all sit at **GP15 or below**: GP16 drives the RP2350-Zero's on-board WS2812B (kept for the status LED) and the pins above it are awkward to wire on the prototype, so SPI uses GP4-GP7 and the control lines GP13-GP15. `smtc-user-button` is the alias the vendor USP samples want; with it in place `usp_zephyr/samples/usp/**` builds for this board as-is.

Each link has two endpoints (UART and USB CDC); the rule that picks one and the config-mode entries are in [doc/08-firmware.md](../../../doc/08-firmware.md) 1, and the physical interface list is in [doc/04-prototype.md](../../../doc/04-prototype.md) 2 and 3.

## 2. Two SKUs, four build targets

The Core2021's sub-G comes in **two different SKUs** with different PA networks and bands, so the board has two variants (one code base, differing in dtsi and default region):

| Target | sub-G hardware | Regions this design accepts | Default (`default-region`) |
| --- | --- | --- | --- |
| `.../rp2350a/m33/hf` | HF: 850-930 MHz | FCC915 (0), EU868 (1) | FCC915 |
| `.../rp2350a/m33/lf` | LF: 410-510 MHz | CN470 (2), CN433 (3) | CN470 |
| `.../rp2350a/m33/hf/mcuboot` | as hf | as hf | FCC915 |
| `.../rp2350a/m33/lf/mcuboot` | as lf | as lf | CN470 |

The two `/mcuboot` variants add a bootloader and two-slot firmware partitions and need `--sysbuild` (see 4). **Those two shapes are all there is**: the bare `rp2350_zero_core2021/rp2350a/m33` has no dts, so building for it falls into `stub.dts` and Kconfig errors out.

```shell
west build -b rp2350_zero_core2021/rp2350a/m33/hf app   # 868 / 915
west build -b rp2350_zero_core2021/rp2350a/m33/lf app   # 470 / 433
```

`west boards` also lists the same SoC's `rp2350a/hazard3` qualifier (the SoC decides those, the board cannot), but there is no dts for it here and it falls into `stub.dts` as well: this board does not run the RISC-V cores.

### Declaring the SKU

The board declares its sub-G radio design through the DT node `zlrs_rf` (`compatible = "zlrs,rf-design"`, binding in `dts/bindings/zlrs,rf-design.yaml`): `band = <0>` for LF, `<1>` for HF, plus the frequency range the matching network actually covers (`low-hz`/`high-hz`, 410–510 MHz on LF, 850–930 MHz on HF) and the region a fresh board boots with (`default-region`, CN470 on LF and FCC915 on HF). At boot the application checks the region against that design — right band *and* the region's edges inside the range — and on a mismatch shows a **red alert and keeps the radio off**: regulatory mistakes do not get approximated.

This value must **not** live in `/chosen`: Zephyr's DT macros only export `zephyr,`-prefixed chosen properties, a custom prefix reads back as nothing and silently falls back to the HF default, so an LF board would never pass the check (exactly what happened on the first bring-up, see [doc/12-debug.md](../../../doc/12-debug.md) 5).

## 3. Core2021 and calibration

- The module uses a **crystal**, not a TCXO, so `tcxo-*` stays off; `reg-mode` runs DC/DC and `lf-clk` uses the internal RC.
- The common board part sets `tx-power-offset = 0` and `rx-boost-cfg = 7` (maximum RX boost).
- Each variant carries its own sub-G calibration tables:
  - **HF**: `tx-power-cfg-lf` and `tx-dbm-to-ua-reg-mode-dcdc-lf-vreg` use the 902 MHz reference tables (measured on a Semtech Wio-LR2021), `calibration-freqs` = 897.5 MHz plus 2.44 GHz.
  - **LF**: the 470 MHz reference tables, `calibration-freqs` = 433.3 / 470.3 / 498.8 MHz plus 2.44 GHz (LF covers 470 and 433, hence one extra point).
- Both sets are **references** and have to be recalibrated for the Core2021's PA network once it is on a board; the acceptance criteria are in [doc/13-roadmap.md](../../../doc/13-roadmap.md).
- The 2.4 GHz path is identical on both SKUs (one LR2021 time-sharing two bands) and both calibration sets include 2.441 GHz.

## 4. Partitions and boot

4 MB of flash, two layouts:

**hf and lf without a bootloader** (`rp2350_zero_core2021_partitions_1m.dtsi`):

| Partition | Offset | Size | Purpose |
| --- | --- | --- | --- |
| code | 0x000000 | 1 MB | the application image (`zephyr,code-partition`) |
| storage | 0x100000 | 3 MB | NVS: root key, device id, pairing record, failsafe values, baud rate |

Boot chain: RP2350 bootrom to the application (drag `zephyr.uf2` onto the ROM's `RPI-RP2` volume).

**hf/mcuboot and lf/mcuboot with MCUboot** (`rp2350_zero_core2021_partitions_mcuboot.dtsi`):

| Partition | Offset | Size | Purpose |
| --- | --- | --- | --- |
| mcuboot | 0x000000 | 128 KB | the bootloader (61.6 KB measured, leaving room for USB CDC recovery) |
| image-0 (slot0) | 0x020000 | 832 KB | the application image (signed, `zephyr,code-partition`) |
| image-1 (slot1) | 0x0F0000 | 832 KB | where upgrade images land and MCUboot swaps |
| storage | 0x1C0000 | 2.25 MB | the same NVS as above |

Boot chain: RP2350 bootrom, MCUboot (checking slot0's signature), the application. Slot swaps use `swap using move` (no scratch partition needed, rollback works; `swap using offset` goes in but never comes back on this board). The boot chain, image confirmation and recovery entries are described in [doc/08-firmware.md](../../../doc/08-firmware.md) 6.

Variants without a bootloader flash through BOOTSEL plus a UF2 (`west flash --runner uf2`) or SWD (openocd, probe-rs, jlink). MCUboot variants need **two BOOTSEL rounds** the first time:

```shell
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/hf/mcuboot app
python3 tools/make-signed-uf2.py
```

1. hold BOOTSEL while plugging USB in and drag `build/mcuboot/zephyr/zephyr.uf2`;
2. hold BOOTSEL and plug in again, then drag the application's `zephyr.signed.uf2`.

Once the bootloader runs, the ROM's mass-storage mode does not come back, so both rounds are needed. **Note**: the `zephyr.uf2` in the application directory is unsigned and MCUboot rejects it (its base is the flash start, not slot0), so bootloader builds only ever drag `zephyr.signed.uf2`.

Later upgrades go through MCUboot serial recovery and **USB only** (the data port CDC, see [doc/08-firmware.md](../../../doc/08-firmware.md) 6); the GP8/GP9 UART stays the application's own data port and is not involved. In bootloader mode USB shows two CDCs (recovery first, console second) and `app/sysbuild/mcuboot.overlay` deletes the CRSF one, so scripts find ports by `/dev/serial/by-id/...MCUBOOT...` and cannot reuse the application's `ttyACM1`:

```shell
python3 tools/zlrs-recovery.py --port /dev/ttyACM1   # trigger recovery from the application
python3 tools/zlrs-update.py --port /dev/ttyACM1     # trigger, upload, wait for the application
```

## 5. Board defaults

Defaults that `Kconfig.defconfig` sets for every variant:

- `CONFIG_BUILD_OUTPUT_HEX` and `BUILD_OUTPUT_UF2`: produce a hex file and a drag-and-drop uf2.
- The console sits on USB's third CDC, so the board enables `USB_DEVICE_STACK_NEXT` and `USBD_CDC_ACM_CLASS` by default. **`CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` stays off**: that helper registers only the first CDC while the application and bootloader need `subsys/usb` to build its own context and register all of them (see [doc/08-firmware.md](../../../doc/08-firmware.md) 4).
- `USE_DT_CODE_PARTITION=y`: the image links into the devicetree code partition, so growing past 1 MB is a build error rather than a surprise that has eaten the storage partition.
- Peripherals such as `CONFIG_UART_LINE_CTRL`, `UART_INTERRUPT_DRIVEN`, `GPIO`, `RESET`, `CLOCK_CONTROL`, `SERIAL` and `UART_CONSOLE` stay on.
