# Waveshare RP2350-Zero + Core2021 (board definition)

A Waveshare RP2350-Zero (RP2350A, 4 MB flash, Cortex-M33) carrying a Waveshare Core2021
(Semtech LR2021). The board definition fixes the radio wiring, both UARTs, the USB CDCs, the
button, the status LED and the flash partitions into devicetree; the application only knows
`zlrs-*` aliases and never pin numbers.

The board is `rp2350_zero_core2021` rather than `rp2350_zero` because the Zephyr 4.4 tree
already has `waveshare/rp2350_zero` (the bare board, no radio) and board names have to be
unique; the difference here is a Core2021 plus a full wiring set, so the vendor is our own
`zlrs`.

**The physical build and how it behaves in each mode are in
[doc/03-prototype.md](../../../doc/03-prototype.md)**; this document covers the board definition
only: pins and aliases, SKUs and calibration, partitions, build targets and board defaults.

## 1. Pins and aliases

| Signal | RP2350-Zero | DT alias / node | Notes |
| --- | --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | `lora-transceiver` = `lora_core2021` | the radio bus, `spi-max-frequency` 16 MHz |
| SPI0 CS | GP5 | same node (`cs-gpios`) | a plain GPIO chip select, as on Semtech's own shield |
| BUSY | GP13 | same node (`busy-gpios`) | module busy, active high, internal pull-up |
| NRESET | GP14 | same node (`reset-gpios`) | reset, active low |
| DIO8 | GP15 | same node (`dios/dio@8`) | IRQ, active high, internal pull-down |
| UART0 TX / RX | GP0 / GP1 | `zlrs-control-uart` = `uart0` | the control port, CRSF at 420000 |
| UART1 TX / RX | GP8 / GP9 | `zlrs-data-uart` = `uart1` | the data port, 115200 by default (changeable in NVS) |
| Button | GP12 | `zlrs-button`, `smtc-user-button` | to ground with an internal pull-up; none fitted, for your own switch |
| WS2812B | GP16 | `zlrs-led`, `led-strip` | on the module, driven through PIO0; semantics in [doc/10-ux.md](../../../doc/10-ux.md) |
| Watchdog | - | `watchdog0` = `wdt0` | armed only when `CONFIG_ZLRS_APP_WATCHDOG` is on |

The USB device exposes four CDC nodes; three are enabled by default:

| Node | Alias | Purpose |
| --- | --- | --- |
| `cdc_acm_uart0` | `zlrs-control-cdc` | the control port's USB endpoint |
| `cdc_acm_uart1` | `zlrs-data-cdc` | the data port's USB endpoint |
| `cdc_acm_uart2` | `zlrs-mgmt-cdc` | the management protocol |
| `cdc_acm_uart3` | - | console (kernel log + shell); **disabled**, only `app/debug.overlay` enables it |

The radio bus and the three control lines all sit at **GP15 or below**: GP16 drives the
RP2350-Zero's on-board WS2812B (kept for the status LED) and the pins above it are awkward to
wire on the prototype, so SPI uses GP4-GP7 and the control lines GP13-GP15. `smtc-user-button`
is the alias the vendor USP samples want; with it in place `usp_zephyr/samples/usp/**` builds for
this board as-is.

Each link has two endpoints (UART and USB CDC); the rule that picks one is in
[doc/07-firmware.md](../../../doc/07-firmware.md) 1, and the physical interface list is in
[doc/03-prototype.md](../../../doc/03-prototype.md) 3.

### DMA allocation and peripheral support

The application enables `CONFIG_DMA` and `CONFIG_SPI_PL022_DMA`. SPI DMA channels are fixed
in the board devicetree. The one-pixel PIO status LED writes directly to the PIO FIFO without DMA.

| Peripheral | DMA use in this application |
| --- | --- |
| SPI0 / LR2021 | TX channel 0, RX channel 1; upstream PL022 DMA handles every SPI transaction |
| PIO0 / WS2812 status LED | No DMA; one pixel is written synchronously to the PIO FIFO |
| UART0 / CRSF, UART1 / data | Hardware has DMA request sources, but the current upstream PL011 driver has no DMA/async implementation; retain interrupt-driven FIFO I/O |
| USB / CDC | Upstream RP2350 UDC uses USB DPRAM and interrupts; no system-DMA configuration path |
| Flash | Upstream driver uses ROM programming/erase operations; no DMA configuration path |
| TRNG | Application entropy driver reads registers into its pool; no DMA request path |
| Timer, GPIO, watchdog | Register/event operations; no stream transfer to move with DMA |
| I2C, ADC, other SPI/PIO instances | Not used by this board application; remain disabled |

SPI still uses the synchronous API: DMA moves bytes while the calling thread waits for
completion. This does not make the SPI API callable from an ISR or bound LR2021 BUSY waits.
Short command DMA setup and completion overhead must be included in launch timing measurements.
The DMA configuration reserves 6000 us for preparation (2800 us on the polling path); the
existing hardware slot budget reduces cadence accordingly. DMA reserves 800 us for command
transfer plus RF startup and uses a 500 us startup estimate for receiver clock alignment.
Both peers must use matching timing.
Upstream module sources remain unchanged.

## 2. Two SKUs, two build targets

The Core2021's sub-G comes in **two different SKUs** with different PA networks and bands, so the
board has two variants (one code base, differing in dtsi and the region a fresh board boots with):

| Target | sub-G hardware | Regions this design accepts | Default region |
| --- | --- | --- | --- |
| `.../rp2350a/m33/hf` | HF: 850-930 MHz | FCC915 (0), EU868 (1) | FCC915 |
| `.../rp2350a/m33/lf` | LF: 410-510 MHz | CN470 (2), CN433 (3) | CN433 |

Both targets build the application and the bootloader together (`--sysbuild`) from the same flash
layout (see 4): an image that cannot be updated through the bootloader has no reason to exist, so
there is no bootloader-less variant and no qualifier that says "with bootloader" either.

```shell
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/hf app   # 868 / 915
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf app   # 470 / 433
```

`west boards` also lists the same SoC's `rp2350a/hazard3` qualifier (the SoC decides those, the
board cannot), but there is no dts for it here and it falls into `stub.dts` as well: this board
does not run the RISC-V cores.

### 2.1 How the SKU is declared

The variant dtsi carries the radio design node (`compatible = "zlrs,rf-design"`, binding in
`dts/bindings/zlrs,rf-design.yaml`):

```dts
zlrs_rf: zlrs-rf {
	compatible = "zlrs,rf-design";
	band = <0>;              /* 0 = LF, 1 = HF */
	low-hz = <410000000>;
	high-hz = <510000000>;
	default-region = <3>;    /* CN433 */
};
```

At boot the application checks the configured region against that design — right band *and* the
region's band edges inside the covered range — and on a mismatch shows the red alarm and keeps
the radio off: regulatory mistakes do not get approximated. The same node is where the
calibration tables hang.

The value must **not** live in `/chosen`: Zephyr's DT macros only export `zephyr,`-prefixed
chosen properties, a custom prefix reads back as nothing and silently falls back to the HF
default, so an LF board would never pass the check (exactly what happened on the first
bring-up, see [doc/11-debug.md](../../../doc/11-debug.md) 5).

## 3. Core2021 and calibration

- The module uses a **crystal**, not a TCXO, so `tcxo-*` stays off; `reg-mode` runs DC/DC and
  `lf-clk` uses the internal RC.
- The common board part sets `tx-power-offset = 0` and `rx-boost-cfg = 7` (maximum RX boost).
- Each variant carries its own sub-G calibration tables:
  - **HF**: `tx-power-cfg-lf` and `tx-dbm-to-ua-reg-mode-dcdc-lf-vreg` use the 902 MHz reference
    tables (measured on a Semtech Wio-LR2021), `calibration-freqs` = 897.5 MHz plus 2.44 GHz.
  - **LF**: the 470 MHz reference tables, `calibration-freqs` = 433.3 / 470.3 / 498.8 MHz plus
    2.44 GHz (LF covers 470 and 433, hence one extra point).
- Both sets are **references** and have to be recalibrated for the Core2021's PA network once it
  is on a board; the acceptance criteria are in [doc/12-roadmap.md](../../../doc/12-roadmap.md).
- The 2.4 GHz path is identical on both SKUs (one LR2021 time-sharing two bands) and both
  calibration sets include 2.441 GHz.

## 4. Partitions and boot

4 MB of flash, one layout (`rp2350_zero_core2021_partitions.dtsi`), included by the application
and the bootloader alike: both are built from the same board target, so the two images cannot
disagree about where a slot lives.

| Partition | Offset | Size | Purpose |
| --- | --- | --- | --- |
| mcuboot | 0x000000 | 128 KB | the bootloader (about 62 KB used, leaving room for USB CDC recovery) |
| image-0 (slot0) | 0x020000 | 832 KB | the application image (signed, `zephyr,code-partition`) |
| image-1 (slot1) | 0x0F0000 | 832 KB | where upgrade images land and MCUboot swaps |
| log | 0x1C0000 | 1 MB | log store: LittleFS, eight 128 KB files, the oldest overwritten |
| coredump | 0x2C0000 | 128 KB | coredump backend target for the fault case |
| storage | 0x2E0000 | 32 KB | the same NVS as above (nine keys, under 100 B of payload) |

Boot chain: RP2350 bootrom, MCUboot (checking slot0's signature), the application. Slot swaps
use `swap using move` (no scratch partition needed, rollback works; `swap using offset` goes in
but never comes back on this board). Image confirmation and recovery entries are described in
[doc/07-firmware.md](../../../doc/07-firmware.md) 5, the log store in section 7.

BOOTSEL carries the **bootloader image only**; the application is always installed and updated
through the bootloader:

```shell
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/hf app
```

1. hold BOOTSEL while plugging USB in and drag `build/mcuboot/zephyr/zephyr.uf2`; the board
   reboots and parks in MCUboot serial recovery (there is no application yet);
2. upload the application through that recovery port; on a board that already runs an
   application, the same command is aimed at its management port instead:

```shell
python3 tools/zlrs-update.py --serial <serial> \
  --recovery-port /dev/serial/by-id/usb-zlrs_ZLRS_MCUBOOT_<serial>-if00 --no-trigger \
  --image build/app/zephyr/zephyr.signed.bin        # first flash
python3 tools/zlrs-update.py --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04
# later updates
```

**The application is never dragged on as a UF2**: an image written that way lands in slot0 with
an empty trailer, which MCUboot treats as the initial image — no confirmation, no rollback. Only
the bootloader image goes through BOOTSEL. (The `zephyr.uf2` in an application build directory is
the unsigned image based at the flash start: MCUboot rejects it.)

Later upgrades go through MCUboot serial recovery and **USB only**, triggered from the
application's management port:

```shell
python3 tools/zlrs-recovery.py --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04   # trigger only
python3 tools/zlrs-update.py  --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04   # trigger, upload, wait, confirm
```

In bootloader mode USB shows two CDCs (recovery first, console second) and
`app/sysbuild/mcuboot.overlay` removes the link CDCs, so scripts find the recovery port under
`/dev/serial/by-id/...MCUBOOT...` and cannot confuse it with the application's.

## 5. Board defaults

Defaults that `Kconfig.defconfig` sets for every variant:

- `CONFIG_BUILD_OUTPUT_HEX` and `BUILD_OUTPUT_UF2`: produce a hex file and a drag-and-drop uf2.
- `CONFIG_USE_DT_CODE_PARTITION=y`: the image links into the devicetree code partition, so
  growing past it is a build error rather than a surprise that has eaten the storage partition.
- `USB_DEVICE_STACK_NEXT` and `USBD_CDC_ACM_CLASS`: the device stack that carries all CDCs.
  **`CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` stays off**: that helper registers only the first CDC,
  while `subsys/usb` builds its own context and registers every one of them
  (see [doc/07-firmware.md](../../../doc/07-firmware.md) 4).
- No console by default: the `cdc_acm_uart3` node is disabled and only
  `app/debug.overlay` enables it, so a production image has no shell.
- Peripherals such as `CONFIG_UART_LINE_CTRL`, `UART_INTERRUPT_DRIVEN`, `GPIO`, `RESET`,
  `CLOCK_CONTROL` and `SERIAL` stay on.
- `FLASH_RPI_PICO_READ_UNTRANSLATED`: read flash around the XIP cache, or a read after a swap
  returns a line cached before the write (MCUboot then calls a valid image "signature check
  failed"). Both images read flash, so it is set here once.
- `ENTROPY_RPI_PICO_RNG=n`: this board's TRNG is the ZLRS driver (`drivers/entropy`); Zephyr's own
  goes through pico-sdk's interrupt-off busy wait and kills USB.

The upgrade path adds nothing per SKU: both images are built under MCUboot from the same target and
the same flash layout (`rp2350_zero_core2021_partitions.dtsi`), and the settings they share live in
this board's `Kconfig.defconfig`, written once:

- `CONFIG_RETAINED_MEM` / `CONFIG_RETENTION` / `CONFIG_RETENTION_BOOT_MODE`: the boot-mode flag the
  application sets and MCUboot reads to stay in serial recovery across a reboot;
- `CONFIG_STREAM_FLASH` / `CONFIG_IMG_MANAGER` are the *application's* participation (confirm this
  build, roll back one that never came up) and live in `app/prj.conf`.
