# zlrs

A point-to-point link on Zephyr with a Semtech LR2021 radio: **it carries control (CRSF) and
data (a byte stream) at the same time** over one time-shared radio. TX and RX are one firmware;
which end a device is, is configuration.

- Two links, one radio: control (CRSF, telemetry, pairing) and data (passthrough), independent
  state machines sharing the superframe — see [doc/01-product.md](doc/01-product.md).
- Two band roles: **long range** (control on sub-G, data on 2.4G) and **racing** (control on
  2.4G, data on sub-G); the sub-band mix `r` moves slots between the bands as quality changes.
- One firmware for several boards: what hardware exists comes from devicetree (ports, button,
  LED, watchdog, radio design) and the deliberate cuts come from Kconfig.
- An original protocol, not compatible with ExpressLRS; the relationship is in
  [doc/02-vs-elrs.md](doc/02-vs-elrs.md).

The radio driver comes from Semtech's USP and is not in this repository. The documentation index
is further down in this file.

## Hardware

Today's hardware is a two-module prototype: a Waveshare RP2350-Zero (RP2350A, 4 MB flash) and a
Waveshare Core2021 (LR2021) wired together, exposing USB-C only, with a jumper from GP12 to
ground standing in for the button. See [doc/04-prototype.md](doc/04-prototype.md); the carrier
board is in [doc/05-carrier.md](doc/05-carrier.md), and pins, calibration and partitions are in
the [board README](boards/zlrs/rp2350_zero_core2021/README.md).

The Core2021's sub-G comes in two SKUs, so there are two board targets:

| Target | sub-G | Regions this design accepts | Boots with |
| --- | --- | --- | --- |
| `rp2350_zero_core2021/rp2350a/m33/hf` | HF: 850-930 MHz | FCC915 / EU868 | FCC915 |
| `rp2350_zero_core2021/rp2350a/m33/lf` | LF: 410-510 MHz | CN470 / CN433 | CN470 |

Both SKUs are bootloader targets: the image and the bootloader are built together (`--sysbuild`)
from the same flash layout, and the application is only ever installed and updated through the
bootloader.

## Build and flash

```shell
tools/setup-zephyr.sh         # about 5 GB the first time
source tools/zephyr-env.sh    # once per new shell
zlrs-build                    # = west build --sysbuild -b $ZLRS_BOARD $ZLRS_APP
```

**The application is never flashed as a UF2**: every installation and every update goes through
the bootloader. BOOTSEL carries the bootloader image only; after that, the tool does the rest.
The first flash of a board:

1. hold BOOTSEL while plugging USB in and drag `build/mcuboot/zephyr/zephyr.uf2` onto the
   `RPI-RP2` volume — the board reboots and parks in MCUboot serial recovery, because there is no
   application yet;
2. upload the application through that recovery port:

   ```shell
   python3 tools/zlrs.py update --serial <serial> \
     --recovery-port /dev/serial/by-id/usb-zlrs_ZLRS_MCUBOOT_<serial>-if00 --no-trigger \
     --image build/app/zephyr/zephyr.signed.bin
   ```

Later updates are one command from the running application, through its management port:

```shell
python3 tools/zlrs.py update --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04
```

The tool finds the bootloader's recovery port by itself, verifies the image hash after the reboot
and only then confirms it — an image that does not come up rolls back. The two modes announce
different USB names (the application `ZLRS`, the bootloader `ZLRS MCUBOOT`), and scripts find
ports under `/dev/serial/by-id/...` rather than hard-coding `ttyACM*`. Boot chain, recovery
entries and rollback: [doc/08-firmware.md](doc/08-firmware.md) 5.

`tools/zephyr-env.sh` exports `ZLRS_WS` (the workspace root), `ZLRS_APP`, `ZLRS_BOARD` (hf by
default, use `.../lf` for LF) and `ZLRS_SDK_DIR` (`~/zephyr-sdk-1.0.0` by default).

## Management

All device tools use one entry point: `python3 tools/zlrs.py`. Run `--help` for the complete
command list and `<command> --help` for its options. Internal Python modules implement management,
recovery, updates and throughput measurement.

```shell
python3 tools/zlrs.py --help
python3 tools/zlrs.py list
python3 tools/zlrs.py status --serial <serial>
python3 tools/zlrs.py recovery --serial <serial>
python3 tools/zlrs.py throughput --tx-port <TX-serial> --rx-port <RX-serial>
```

USB shows three CDCs: **control (CRSF), data and management** (debug images add a console, see
[doc/12-debug.md](doc/12-debug.md)). The tool sends the enter-management-mode command on the
management CDC; the radio stops entirely until it exits. Every user parameter is set at runtime
with no firmware rebuild:

```shell
python3 tools/zlrs.py --self-test
python3 tools/zlrs.py status --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04
python3 tools/zlrs.py set-phrase --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04 "correct horse battery staple"
python3 tools/zlrs.py set-region --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04 3
python3 tools/zlrs.py set-role --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04 rx
python3 tools/zlrs.py set-profile --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04 p1
python3 tools/zlrs.py set-failsafe --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04 @default
python3 tools/zlrs.py exit --port /dev/serial/by-id/usb-zlrs_ZLRS_<serial>-if04
```

For command details and examples:

```shell
python3 tools/zlrs.py --help
python3 tools/zlrs.py set-baud --help
```

Status is grouped into device, link and configuration fields. Operation results use clear
`[OK]`, `[WARN]` and `[ERROR]` markers. Colour is enabled for terminals and disabled for
redirected output or when `NO_COLOR` is set. Command options go after the command. `--baud`
controls the management connection; `set-baud 420000` changes the
**data UART**. Binding phrases accept 1–32 UTF-8 bytes; both peers need the same phrase.
`--dry-run` prints the operation without opening a port, including log exports.

To inspect an upgrade before rebooting a device:

```shell
python3 tools/zlrs.py update --dry-run --image <build>/app/zephyr/zephyr.signed.bin
ZLRS_BUILD_DIR=build-lf python3 tools/zlrs.py update --serial <serial>
python3 tools/zlrs.py update --help
```

The default image uses `<ZLRS_WS>/<ZLRS_BUILD_DIR>/app/zephyr/zephyr.signed.bin` (`build` when
unset). The updater checks the image content hash and required tools before triggering recovery,
tracks the USB serial through reboot, and uploads to the slot reported by MCUboot. Upload progress
is shown live. `--client none` prepares manual commands; `--no-wait` returns after upload. Neither
mode verifies or confirms the running image. `--no-confirm-check` verifies it and leaves it
unconfirmed for rollback testing. Hash mismatches always fail and prevent confirmation.

The command set is in [doc/10-security.md](doc/10-security.md) 9 and the firmware behaviour in
[doc/08-firmware.md](doc/08-firmware.md) 3.

### Build-time configuration

What a build decides (the full table is in [doc/08-firmware.md](doc/08-firmware.md) 4):

| Switch | Default | Effect |
| --- | --- | --- |
| `CONFIG_ZLRS_APP_ROLE_TX` | y | **default** role: y = TX, n = RX; change it at runtime with `set-role` |
| security profile choice | P1 | **default** profile; change it at runtime with `set-profile` (`P0` is a debug-image profile, `P2` is still to come) |
| `CONFIG_ZLRS_APP_BAND_ROLE_SUBG_CONTROL` / `..._SUBG_DATA` | the former | long range (control on sub-G) or racing (control on 2.4G) |
| `CONFIG_ZLRS_APP_LINK_CONTROL` / `CONFIG_ZLRS_APP_LINK_DATA` | both on | turn one off to build a control-only or data-only product |
| `CONFIG_ZLRS_APP_CONTROL_MODULATION_FLRC` / `..._LORA` | the former | wideband FLRC or the LoRa long-range control tier; the LoRa frame rate is `CONFIG_ZLRS_APP_LORA_CONTROL_RATE_HZ` |
| `CONFIG_ZLRS_APP_CONTROL_RATE_HZ` | 150 | control rate tier (50-500); the regional duty cap and airtime clamp the effective value |
| `CONFIG_ZLRS_APP_RACING_CONTROL_RATE_BPS` / `..._HZ` | 2.6 Mbps / 500 Hz | control link parameters in the racing role |
| `CONFIG_ZLRS_APP_DATA_RATE_BPS` | 2.6 Mbps | data link rate on 2.4G |
| `CONFIG_ZLRS_APP_DEBUG*` | off | debug images only: shell, stats log, boot delay, radio off, hopping off, P0 … see [doc/12-debug.md](doc/12-debug.md) 3 |

The radio and the ports are not in this table on purpose: whether the board has a radio, a
control port, a management CDC, a button or a LED is a devicetree fact (`app/src/platform/caps.h`).

## What this builds

| Artifact | Command | Notes |
| --- | --- | --- |
| application + bootloader | `zlrs-build` | the supported image: `--sysbuild` builds both domains (`<dir>/app/zephyr/zephyr.signed.bin` is what the bootloader installs, `<dir>/mcuboot/zephyr/zephyr.uf2` is the one thing that goes on over BOOTSEL) |
| application only (edit loop) | `zlrs-rebuild-app -d <dir>` | re-runs the `app` domain in an existing sysbuild tree: the bootloader is already built and does not change while the application is being worked on |
| debug image | `zlrs-build` + `app/debug.conf` + `app/debug.overlay` | console CDC, shell, stats logging |
| host tests | `make -C tests/host run` | protocol core, link layer, CRSF, management protocol, LED; no hardware, no workspace |
| management tool | `python3 tools/zlrs.py status` | read status, write parameters, start pairing, exit |
| update tool | `python3 tools/zlrs.py update` | trigger recovery, upload a signed image, wait for the application |
| tree gates | `tools/gates.sh` | core purity, no board names in `app/`, no debug switch in shipped configs, the log ring fits its partition |

The vendor sample (Semtech's `packet_error_rate_flrc`) runs on this board as-is, which is a way to
check the toolchain and radio stack on their own.

## Repository layout

```
app/src/                 Zephyr application, one directory per layer (doc/07-architecture.md 5):
                         main.c plus platform/ ports/ link/ mgmt/ state/ ui/ storage/
boards/zlrs/             board definitions (RP2350-Zero plus Core2021)
core/proto/              protocol core: framing, reassembly, hop tables, key derivation and
                         tags, telemetry payload, modulation tiers and the airtime model
core/link/               link layer: state machine, pairing and sessions, superframe slots,
                         the radio abstraction, reliable retransmit, rate and power control
core/crsf/               CRSF codec
core/mgmt/             management protocol: framing, dispatch, status encoding
core/region/             region tables, power and duty accounting, the control time plan
core/led/                status LED engine
include/zlrs/            public headers
subsys/usb/              USB device stack (usbd context plus every CDC in devicetree)
subsys/bootguard/        bootloader-side watchdog handling
drivers/entropy/         the RP2350 TRNG driver we own
dts/bindings/            bindings for zlrs-specific nodes
tests/host/              host tests and the two-node simulation
tools/                   workspace, unified device CLI, management, recovery, update, throughput, gates
doc/                     design documents
zephyr/module.yml        this repository as a Zephyr module
```

`core/` and `include/` depend on no Zephyr, so the protocol core and link layer compile the same
sources on the host; the target only adds the radio port layer. Everything on the radio side
(SPI timing, calibration, band selection, transfer scheduling) belongs to `usp_zephyr`.

## Documentation

Four parts: **product design** (requirements), **hardware** and **software** (the
implementation), **development** (debugging and planning). The numbering is the reading order;
every document owns one kind of fact and other places link to it.

Reading paths:

- To use it: this file -> [04-prototype](doc/04-prototype.md) -> [11-ux](doc/11-ux.md)
- To change the firmware: [01-product](doc/01-product.md) -> [07-architecture](doc/07-architecture.md) -> [08-firmware](doc/08-firmware.md) -> [09-protocol](doc/09-protocol.md)
- To port it to new hardware: [03-platform](doc/03-platform.md) -> board README
- To understand the trade-offs: [01-product](doc/01-product.md) -> [02-vs-elrs](doc/02-vs-elrs.md) -> [07-architecture](doc/07-architecture.md) -> [09-protocol](doc/09-protocol.md) -> [10-security](doc/10-security.md)

Pins, aliases and partition numbers are defined once in the
[board README](boards/zlrs/rp2350_zero_core2021/README.md); debugging material lives in
[12-debug](doc/12-debug.md) and nowhere else. Documents describe the final shape; when behaviour
is decided but not implemented yet, the design document states the design and
[13-roadmap](doc/13-roadmap.md) carries the gap.

| Document | Contents |
| --- | --- |
| [01-product](doc/01-product.md) | positioning, the two links, band roles and port shapes, distance x rate targets, performance figures, boundaries |
| [02-vs-elrs](doc/02-vs-elrs.md) | the relationship with ExpressLRS: borrowed and different |
| [03-platform](doc/03-platform.md) | hardware platform, build targets, image layout, flashing |
| [04-prototype](doc/04-prototype.md) | the prototype and what each mode looks like |
| [05-carrier](doc/05-carrier.md) | the carrier board |
| [06-radio](doc/06-radio.md) | LR2021 parameter summary |
| [07-architecture](doc/07-architecture.md) | the two links, band mixing, time sharing, timing budget, layering, threads |
| [08-firmware](doc/08-firmware.md) | ports and modes, runtime configuration, build-time choices, updates, error handling |
| [09-protocol](doc/09-protocol.md) | frame format, state machine, pairing and sessions, superframe, ARQ, hopping and regulation |
| [10-security](doc/10-security.md) | identity and keys, pairing, sessions, profiles, threat model, the management protocol |
| [11-ux](doc/11-ux.md) | the button and the status LED |
| [12-debug](doc/12-debug.md) | debug images, isolation switches and on-target debugging (developers) |
| [13-roadmap](doc/13-roadmap.md) | to-dos, pending defaults and acceptance criteria |

## Dependencies

| Dependency | Version | Notes |
| --- | --- | --- |
| Zephyr | v4.4.0 | pinned by [west.yml](west.yml) |
| usp_zephyr / usp | v1.2.1 | Semtech's radio stack (RAC/RALF) |
| Zephyr SDK | 1.0.0 | install the `arm-zephyr-eabi` toolchain |
| Cryptography | Zephyr PSA Crypto | KDF and per-frame tags on target; the host tests use an equivalent pure C implementation |

## Build notes

1. **The board ships with the repository**: `zephyr/module.yml` makes the repository root both
   `board_root` and `dts_root`, so no `--shield` is needed.
2. **New hardware means devicetree only**: the application knows the aliases
   (`lora-transceiver`, `zlrs-control-uart`/`-cdc`, `zlrs-data-uart`/`-cdc`, `zlrs-mgmt-cdc`,
   `zlrs-button`, `zlrs-led`) and a missing alias trims that port or peripheral automatically.
3. **The toolchain is the Zephyr SDK**: a distro `arm-none-eabi-gcc` trips a static assertion.
4. **The image may not grow past its code partition**: overrunning it is a build error.
5. **Host tests need no workspace**: `tests/host` compiles the same sources from `core/`.

## Tests

`tests/host` simulates with a fake radio and a fake clock: frame and management codecs, pairing and
sessions, hop lockstep, replay protection, multi-frame passthrough and loss recovery, ARQ,
rate and power control, the LED language, peer loss and reconnection, several nodes on one
channel, malformed input, timestamp and sequence wrap, and every isolation switch "still works
with this off". ASan and UBSan are on by default.

## Gates

| Gate | Where | What it catches |
| --- | --- | --- |
| `clang-format --dry-run --Werror` over `git ls-files '*.c' '*.h'` | `build.yml` | style drift |
| host tests with ASan + UBSan | `build.yml` | protocol and link regressions |
| tool self-tests (`python3 tools/zlrs.py --self-test`) and `python3 tools/tests/test_cli.py` | `build.yml` | framing drift between tools and firmware |
| build matrix: both SKUs x plain, debug overlay, bench switches, single-link cuts, LoRa, racing, MCUboot | `firmware.yml` | Kconfig and devicetree combinations that do not compile |
| `tools/gates.sh`: `core/` has no `#include <zephyr/`, `app/` names no board, shipped configs carry no `ZLRS_APP_DEBUG_*`, the log ring equals `log_partition` | `build.yml`, or by hand | platform code leaking into the pure C core, board branching, a debug-only switch shipping in an image, a log ring that overruns its partition |

CI runs all of them ([.github/workflows/build.yml](.github/workflows/build.yml),
[firmware.yml](.github/workflows/firmware.yml)).
