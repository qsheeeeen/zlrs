# ZLRS

ZLRS carries a transparent, bidirectional USB/UART byte stream over an authenticated,
frequency-hopping LR2021 radio link. Both endpoints run the same firmware: configure
one as Leader, the other as Follower, provision the same root key or binding phrase and pair
them.

## Capabilities

- Bidirectional stop-and-wait ARQ with retransmission and receive backpressure.
- P1 frame authentication, directional session keys and replay protection; user
  DATA remains plaintext.
- Automatic 2.4 GHz/SubG switching with coordinated plans and loss recovery, or
  fixed-band selection; one band is active at a time.
- Peer quality feedback and dynamic transmit power within the active band’s board and regional limits.
- Persistent identity, pairing and configuration.
- Dedicated USB management, LR2021/MCU status, logs and signed firmware updates
  with verification, confirmation and rollback.
- One button for pairing and one LED for role, link and device state.

The prototype combines a Waveshare RP2350-Zero and Core2021. Select the LF or HF
firmware target matching the SubG module. Automatic operation requires usable
antenna paths for both SubG and 2.4 GHz.

## Build and install

Create a west workspace and install its dependencies:

```sh
mkdir -p ~/zlrs-workspace
cd ~/zlrs-workspace
git clone https://github.com/qsheeeeen/zlrs.git
cd zlrs
tools/setup-zephyr.sh
```

Follow the SDK installation commands printed by setup if required, then build:

```sh
export ZLRS_BOARD=rp2350_zero_core2021/rp2350a/m33/lf
source tools/zephyr-env.sh
zlrs-build
```

Use `rp2350_zero_core2021/rp2350a/m33/hf` for the HF module. Default build output
is in the parent workspace's `build` directory. Commands below run from the
repository root.

Install MCUboot by holding BOOTSEL while connecting USB and copying
`../build/mcuboot/zephyr/zephyr.uf2` to the USB drive. Upload the signed application
through the resulting recovery port:

```sh
python3 tools/zlrs.py update --no-trigger --serial <serial> \
  --image ../build/app/zephyr/zephyr.signed.bin
```

Update a running device through its management interface:

```sh
python3 tools/zlrs.py update --serial <serial> \
  --image ../build/app/zephyr/zephyr.signed.bin
```

The updater follows the device through reboot, verifies its running image hash
and confirms the installation. See [firmware operation](doc/06-firmware.md) for
build credentials, storage and rollback.

## Configure and pair

```sh
python3 tools/zlrs.py list
python3 tools/zlrs.py set-role leader --serial <Leader-serial>
python3 tools/zlrs.py set-role follower --serial <Follower-serial>
python3 tools/zlrs.py set-phrase 'shared binding phrase' --serial <Leader-serial>
python3 tools/zlrs.py set-phrase 'shared binding phrase' --serial <Follower-serial>
python3 tools/zlrs.py set-band auto --serial <Leader-serial>
python3 tools/zlrs.py set-band auto --serial <Follower-serial>
```

Set the same compatible SubG region on both devices and reboot to apply ordinary
settings. Then open pairing on both devices with the button or tool:

```sh
python3 tools/zlrs.py pair --serial <Leader-serial>
python3 tools/zlrs.py pair --serial <Follower-serial>
python3 tools/zlrs.py status --serial <Leader-serial>
python3 tools/zlrs.py status --serial <Follower-serial>
```

Use the DATA CDC or the 3.3 V UART for application bytes. Management commands pause
local RF while retaining accepted DATA. `status` exposes active band, acquisition
and pending transition as well as session state. Run `python3 tools/zlrs.py --help`
for the command list.

## Documentation

| Document | Responsibility |
| --- | --- |
| [Product](doc/01-product.md) | Scope, roles, interfaces and delivery guarantees |
| [Prototype](doc/02-prototype.md) | Modules, electrical wiring and hardware capabilities |
| [Carrier](doc/03-carrier.md) | PCB interfaces, power and mechanical requirements |
| [Radio](doc/04-radio.md) | Modem, regional operating policies and transfer timing |
| [Architecture](doc/05-architecture.md) | Ownership, execution and service contracts |
| [Firmware](doc/06-firmware.md) | Configuration, builds, storage, installation and updates |
| [Protocol](doc/07-protocol.md) | Air frames, switching, management framing and status |
| [Security](doc/08-security.md) | Keys, authentication, replay, entropy and trust |
| [User interface](doc/09-ux.md) | Button, LED and tool presentation |
| [Development and acceptance](doc/10-debug.md) | Build/test procedures and evidence requirements |
| [Roadmap](doc/11-roadmap.md) | RF qualification and planned product scope |
| [Board reference](boards/zlrs/rp2350_zero_core2021/README.md) | Pin aliases, calibration and flash partitions |

ZLRS uses its own protocol and is not compatible with ExpressLRS. Source is
licensed under Apache-2.0.
