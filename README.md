# zlrs

A point-to-point link on Zephyr with a Semtech LR2021 radio: **it carries both telemetry and control**. TX and RX are one firmware with the role chosen in Kconfig.

- Three shapes: all-2.4G fast control (racing), all-2.4G fast data, and dual-band mixing (sub-G control plus 2.4G data) — see [doc/01-product.md](doc/01-product.md).
- One radio time-sharing two bands: sub-GHz for control by default, 2.4 GHz for data.
- One firmware for several boards: role, band and features such as USB are trimmed through Kconfig and devicetree.
- An original protocol, not compatible with ExpressLRS; the relationship is in [doc/02-vs-elrs.md](doc/02-vs-elrs.md).

The radio driver comes from Semtech's USP and is not in this repository. Documentation index and reading paths: [doc/README.md](doc/README.md).

## Hardware

Today's hardware is a two-module prototype: a Waveshare RP2350-Zero (RP2350A, 4 MB flash) and a Waveshare Core2021 (LR2021) wired together, exposing USB-C only, with a jumper from GP12 to ground standing in for the button. See [doc/04-prototype.md](doc/04-prototype.md). The carrier board is in [doc/05-carrier.md](doc/05-carrier.md); pins, SKU calibration and partitions are in the [board README](boards/zlrs/rp2350_zero_core2021/README.md).

The Core2021's sub-G comes in two SKUs, so there are two board variants:

| Target | sub-G | Regions (set at runtime) |
| --- | --- | --- |
| `rp2350_zero_core2021/rp2350a/m33/hf` | HF: 850-930 MHz | FCC915 / EU868 |
| `rp2350_zero_core2021/rp2350a/m33/lf` | LF: 410-510 MHz | CN470 / CN433 |

## Build and flash

```shell
scripts/setup-zephyr.sh        # about 5 GB the first time
source scripts/zephyr-env.sh   # once per new shell
zlrs-build                     # = west build -b $ZLRS_BOARD $ZLRS_APP
cd "$ZLRS_WS" && west flash    # openocd by default, or --runner uf2/probe-rs/jlink
```

MCUboot builds (recommended: later upgrades need no BOOTSEL):

```shell
zlrs-build-mcuboot                # = west build --sysbuild -b $ZLRS_BOARD/mcuboot $ZLRS_APP
python3 scripts/make-signed-uf2.py
```

The first flash needs two BOOTSEL rounds: drag `build/mcuboot/zephyr/zephyr.uf2`, then enter BOOTSEL again and drag `build/app/zephyr/zephyr.signed.uf2`. After that one command upgrades it:

```shell
python3 scripts/zlrs-update.py --port /dev/ttyACM1
```

The two modes announce different USB names (the application `ZLRS`, the bootloader `ZLRS MCUBOOT`) and scripts find ports under `/dev/serial/by-id/...` rather than hard-coding `ttyACM*`. Boot chain, recovery entries and rollback: [doc/08-firmware.md](doc/08-firmware.md) 6.

`scripts/zephyr-env.sh` exports `ZLRS_WS` (the workspace root), `ZLRS_APP`, `ZLRS_BOARD` (hf by default, use `.../lf` for LF) and `ZLRS_SDK_DIR` (`~/zephyr-sdk-1.0.0` by default).

## Configuration

USB shows three CDCs: **CRSF, data and config**. The tool sends the enter-config-mode command on the config CDC; the radio stops entirely until it exits. Every user parameter is set at runtime with no firmware rebuild:

```shell
python3 scripts/zlrs-config.py --self-test
python3 scripts/zlrs-config.py --port /dev/ttyACM2 status
python3 scripts/zlrs-config.py --port /dev/ttyACM2 set-phrase "correct horse battery staple"
python3 scripts/zlrs-config.py --port /dev/ttyACM2 set-region 3
python3 scripts/zlrs-config.py --port /dev/ttyACM2 set-failsafe @default
python3 scripts/zlrs-config.py --port /dev/ttyACM2 exit
```

The command set is in [doc/10-security.md](doc/10-security.md) 10 and the firmware behaviour in [doc/08-firmware.md](doc/08-firmware.md) 3.

### Compile-time configuration

The role and link shape decide which timing the firmware runs and are compile-time settings (every other user parameter is set at runtime):

| Switch | Default | Effect |
| --- | --- | --- |
| `CONFIG_ZLRS_APP_ROLE_TX` | y | off means RX; TX and RX are one firmware |
| `CONFIG_ZLRS_APP_BAND_ROLE_SUBG_CONTROL` / `..._SUBG_DATA` | the former | dual-band mixing (sub-G control plus 2.4G data) or the all-2.4G racing shape |
| `CONFIG_ZLRS_APP_LINK_CONTROL` / `CONFIG_ZLRS_APP_LINK_DATA` | both on | turn one off to run the other alone |
| `CONFIG_ZLRS_APP_CONTROL_RATE_HZ` | 150 | control rate tier (50-500); the regional duty cap and airtime clamp the effective value |
| `CONFIG_ZLRS_APP_RACING_CONTROL_RATE_BPS` / `..._HZ` | 2.6 Mbps / 500 Hz | control link parameters in the racing shape |
| `CONFIG_ZLRS_APP_CONTROL_MODULATION_FLRC` / `..._LORA` | the former | wideband FLRC or the LoRa long-range tier for the control link; the LoRa rate is `CONFIG_ZLRS_APP_LORA_CONTROL_RATE_HZ` |
| `CONFIG_ZLRS_APP_DATA_ARQ` | see [doc/13-roadmap.md](doc/13-roadmap.md) | reliable retransmit on the data link; the target is on by default |
| `CONFIG_ZLRS_APP_RATE_ADAPT` | n | move rate tiers by LQ; thresholds await measurement |
| `CONFIG_ZLRS_APP_DYNAMIC_POWER` | y | dynamic power; the floor is `CONFIG_ZLRS_APP_DYNAMIC_POWER_FLOOR_DBM` |
| `CONFIG_ZLRS_APP_CONFIG_MODE` | y | config mode; debug builds also have a shell entry ([doc/12-debug.md](doc/12-debug.md)) |
| `CONFIG_ZLRS_APP_ROOT_KEY_HEX` | empty | for bench work: build a root key in and skip the config step (the normal path still uses the tool) |

## What this builds

| Artifact | Command | Notes |
| --- | --- | --- |
| firmware (TX or RX) | `zlrs-build` | target `.../m33/hf` by default (use `.../lf` for LF) |
| firmware plus MCUboot | `zlrs-build-mcuboot` | adds `--sysbuild`, a bootloader and two-slot partitions |
| host tests | `make -C tests/host run` | protocol core, link layer, CRSF and the config protocol; no hardware, no workspace |
| bench self-test | `python3 scripts/zlrs-bringup.py` | three CDCs, config mode, config protocol reads and writes, NVS persistence |
| config tool | `python3 scripts/zlrs-config.py` | read status, write parameters, start pairing, exit |
| update tool | `python3 scripts/zlrs-update.py` | trigger recovery, upload a signed image, wait for the application |

The vendor sample (Semtech's `packet_error_rate_flrc`) runs on this board as-is, which is a way to check the toolchain and radio stack on their own.

## Repository layout

```
app/                  Zephyr application: USP/RAC port, ports, superframe main loop, NVS storage
boards/zlrs/          board definitions (RP2350-Zero plus Core2021)
lib/proto/            protocol core: framing, reassembly, hop tables, key derivation and tags,
                                            telemetry payload, modulation tiers and the airtime model (FLRC / LoRa)
lib/link/             link layer (pure C): state machine, pairing and sessions, superframe slots,
                                            the radio abstraction, reliable retransmit and rate adaptation
lib/crsf/             CRSF codec (pure C)
lib/config/           config protocol (pure C): framing plus command dispatch
lib/region/           region layer (pure C): channel plans, power ceilings, duty accounting
lib/led/              status LED engine (pure C)
lib/usb/              USB device stack (usbd context plus every CDC in devicetree)
include/zlrs/         public headers
tests/host/           host tests and the two-node simulation
scripts/              workspace, config, update and self-test scripts
doc/                  design documents (index in doc/README.md)
zephyr/module.yml     this repository as a Zephyr module
```

`lib/` and `include/` depend on no Zephyr, so the protocol core and link layer compile the same sources on the host; the target only adds a radio port layer. Everything on the radio side (SPI timing, calibration, band selection, transfer scheduling) belongs to `usp_zephyr`.

## Documentation

Index and reading paths: [doc/README.md](doc/README.md). Four parts: product design (requirements), hardware, software and development.

| Document | Contents |
| --- | --- |
| [01-product](doc/01-product.md) | positioning, the three shapes and scenarios, distance x rate targets, performance figures against ELRS, boundaries |
| [02-vs-elrs](doc/02-vs-elrs.md) | the relationship with ExpressLRS: borrowed and different |
| [03-platform](doc/03-platform.md) | hardware platform, build targets, image layout, flashing |
| [04-prototype](doc/04-prototype.md) | the prototype and what each mode looks like |
| [05-carrier](doc/05-carrier.md) | the carrier board |
| [06-radio](doc/06-radio.md) | LR2021 parameter summary |
| [07-architecture](doc/07-architecture.md) | the two links, time sharing, timing budget, layering, threads |
| [08-firmware](doc/08-firmware.md) | ports and modes, runtime configuration, trimming, updates, error handling |
| [09-protocol](doc/09-protocol.md) | frame format, state machine, sessions, sync, reliable transfer, hopping and regulation |
| [10-security](doc/10-security.md) | identity and keys, pairing, threat model, the config protocol |
| [11-ux](doc/11-ux.md) | the button and the status LED |
| [12-debug](doc/12-debug.md) | debug overlay, bench switches and on-target debugging (developers) |
| [13-roadmap](doc/13-roadmap.md) | to-dos and acceptance criteria |

## Dependencies

| Dependency | Version | Notes |
| --- | --- | --- |
| Zephyr | v4.4.0 | pinned by [west.yml](west.yml) |
| usp_zephyr / usp | v1.1.2-feature-202604 | Semtech's radio stack (RAC/RALF) |
| Zephyr SDK | 1.0.0 | install the `arm-zephyr-eabi` toolchain |
| Cryptography | Zephyr PSA Crypto | KDF and per-frame tags on target; the host tests use an equivalent pure C implementation |

## Build notes

1. **The board ships with the repository**: `zephyr/module.yml` makes the repository root both `board_root` and `dts_root`, so no `--shield` is needed.
2. **New hardware means devicetree only**: the application knows aliases (`lora-transceiver`, `zlrs-crsf-uart`/`-cdc`, `zlrs-data-uart`/`-cdc`, `zlrs-button`, `zlrs-led`) and a missing alias trims that port automatically.
3. **The toolchain is the Zephyr SDK**: a distro `arm-none-eabi-gcc` trips a static assertion.
4. **The image may not grow past its code partition**: overrunning it is a build error.
5. **Host tests need no workspace**: `tests/host` compiles the same sources from `lib/`.

## Tests

`tests/host` simulates with a fake radio and a fake clock: pairing and sessions, hop lockstep, replay protection, multi-frame passthrough and loss recovery, peer loss and reconnection, several nodes on one channel, malformed input, timestamp and sequence wrap, and every bench switch "still works with this off". ASan and UBSan are on by default.

CI runs the host tests and every script's self-test ([.github/workflows/build.yml](.github/workflows/build.yml)); firmware builds have their own pipeline ([firmware.yml](.github/workflows/firmware.yml)).
