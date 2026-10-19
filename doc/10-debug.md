# Development and acceptance

Design requirements precede implementation. Specify fields, state transitions,
ownership and failure behavior in the relevant design document, then validate
the implementation against those requirements. Test results, image hashes,
bench logs and unresolved observations belong in validation artifacts.

## Build configurations

Production acceptance uses P1, normal hopping, ARQ, automatic or explicitly fixed
band selection, dynamic power and watchdog. Debug builds add a console and
reports without changing the air contract. Both peers require matching protocol,
band mode, SubG region, profile and timing.

Build a debug image from the repository root:

```sh
export ZLRS_BOARD=rp2350_zero_core2021/rp2350a/m33/lf
source tools/zephyr-env.sh
zlrs-build -d build-debug -- \
  -DEXTRA_CONF_FILE="$ZLRS_REPO_ROOT/app/debug.conf" \
  -DDTC_OVERLAY_FILE="$ZLRS_REPO_ROOT/app/debug.overlay"
```

The wrapper builds in the parent workspace; this example's artifacts are under
`../build-debug`. Debug LED output defaults to 1%. Production has no console shell.
Fixed-channel, P0, ARQ-disabled and RAW configurations are isolation fixtures and
must not be presented as product performance.

## Observing a running link

Management `status` reports local/peer RSSI and LQ, active-band power limits and
transmit setpoint, MCU temperature, frequency, frame period, counters and automatic-band
flags. Its LR2021 section provides junction/XOSC temperature, VBAT, version,
mode/command/reset, IRQ and errors with independent validity bits.

Both management `status` and debug `zlrs status` group observations as Link,
Radio, Signal / Power, Traffic / Errors, Device / Configuration, Sensors and
LR2021 snapshot. Radio places the active band and modulation beside frequency,
frame period, capture and transition state. Modulation identifies the current
FLRC rate or LoRa SF/BW. An older firmware's 90-byte status cannot report this
field: the host prints `not reported (upgrade device firmware)` rather than
inferring modulation from frequency or frame period. Upgrade both peers together
to obtain the adaptive profiles and their live status.

Management status pauses RF and refreshes chip measurements at that maintenance
boundary. Repeated reads can affect peer quality or recovery. Use debug shell
`zlrs status` for nonpausing observation: it reads copied state, refreshed once per
second and when LED inputs change. Chip values remain the latest boot or
management sample, and their age is shown. Standby mode at a maintenance boundary
is expected; it is not the preceding live RX/TX mode.

Sensor labels identify their source: `MCU chip temp` is the main MCU's internal
temperature, `LR2021 chip temp` is the radio chip's internal junction temperature,
and `LR2021 XOSC temp` is its crystal-oscillator-area temperature. These measure
different locations inside the chips; none is an ambient-temperature reading.
The console places `LR2021 sample age` beside these values in milliseconds.
Management `status` refreshes the radio measurements for that request. Printing
debug `zlrs status` only reads the previous radio snapshot; its MCU temperature
is read when the command runs. The LR2021's two internal sensor sources are
specified in [datasheet §6.5.2, GetTemp](https://www.mouser.com/pdfDocs/61979758LR2021_V1_1_datasheet.pdf#page=109).

Debug management reports are available with:

```sh
python3 tools/zlrs.py link --serial <serial>
python3 tools/zlrs.py rate --serial <serial>
python3 tools/zlrs.py radio --serial <serial>
python3 tools/zlrs.py power --serial <serial>
python3 tools/zlrs.py logs --serial <serial>
```

Automatic-mode 2.4 GHz profiles are shown by management `status`, debug
`zlrs status` and the `rate` report. On the Leader debug console, `zlrs phy 0`
through `zlrs phy 4` request one coordinated profile change for bench acceptance:
0 FLRC 2.6 Mbps, 1 FLRC 650 kbps, 2 FLRC 325 kbps, 3 LoRa SF6, 4 LoRa SF7.
The request is volatile and normal quality policy continues after the change.
Follower and fixed-band requests are rejected. Test both directions with queued
DATA, lost ACKs, missed announcements and a PHY mismatch; a simulated radio must
reject packets with mismatched rate/modulation, even at the same frequency.
Air version 12 requires both endpoints to be upgraded together. The new host
tool retains decoding of the previous 90-byte management status for rollback.

The debug console accepts `zlrs power <dBm>` for a temporary fixed setpoint
within the active range while connected. `zlrs power auto` resumes automatic
control at maximum power. Pairing, retuning and recovery clear the override.

Delivery, PHY reception and session readiness are separate observations. An exact
ARQ-delivered stream does not prove zero physical loss; LQ=100 is a finite
sequence-span observation. CONNECTED alone does not prove bidirectional DATA
capture.

## Host and tool checks

```sh
cmake -S tests/host -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
python3 tests/tools/run.py
tests/gates.sh
```

Host checks run without a Zephyr workspace and use AddressSanitizer/UBSan by
default. Required coverage includes:

- Independent byte vectors for all eight air types, first-byte bit allocation,
  common u32 sequences, P0/P1 exact lengths, empty DATA and 511-byte frames.
- Inline DATA ownership, fixed-FLRC P1/P0 payloads of 499/503 bytes, automatic-mode
  MTUs of 243/247 bytes and encoded tags.
- LoRa SF6/SF7 airtime vectors, profile identifiers and rejected PHY mismatches.
- Independent SHA/HMAC/HKDF/PBKDF2 vectors, multipart hashing and tag verification.
- Both peers pairing and establishing sessions; duplicate requests, nonce zero,
  changed IDs, wrong tuple confirmations, response/confirmation loss and repeats.
- Common sequence boundaries across 32768 and 65536, forged high sequences,
  wrong context, reflection, stale ACK, exhaustion and no within-context wrap.
- DATA 127→0 and multiple wraps, lost reverse ACK, sink backpressure, unequal
  directional speeds and prepared-ACK follow-up.
- Confirmed-session LQ windows and gaps across boundaries, FHSS indices in every
  domain, rate/power validation and duty deferral.
- Entropy failure, nonce repetition, blocked proofs and successful recovery while
  pending user DATA remains undelivered.
- Automatic policy hysteresis, planned transitions, missed announcements,
  one-way/total loss, failed/successful probes and preserved replay/ARQ progress.

Tool checks validate commands, accepted lengths/values, status decoding, device
selection, bootloader-port discovery and hash/confirmation verdicts. Tree gates
enforce pure protocol boundaries, board-independent application services, debug
isolation, signed application installation and log-partition sizing.

## Native Zephyr acceptance

Use `native_sim/native/64` for lifecycle, settings, management, serial, UI and RF fixtures.
Serial acceptance covers UART output before any input, charge-only USB power,
USB configuration without DTR or initial data, deconfiguration, reset, suspension,
resume and input discard. Whole-packet output acceptance and USB input
backpressure must hold across endpoint changes. Hardware acceptance includes USB
enumeration and bidirectional DATA, plus UART output routing while USB is
unconfigured. Endpoint diagnostics distinguish readiness, selection, cumulative
input/output bytes, queued bytes and drops. This allows output routing to be
checked before any UART input. Physical loopback acceptance shorts UART TX to RX
and verifies returned DATA while USB is unconfigured.
Settings and management runs use simulated flash:

```sh
source tools/zephyr-env.sh
west build -b native_sim/native/64 tests/simulation/application -d build/sim-application
build/sim-application/zephyr/zephyr.exe
west build -b native_sim/native/64 tests/simulation/settings -d build/sim-settings
build/sim-settings/zephyr/zephyr.exe --flash_in_ram
west build -b native_sim/native/64 tests/simulation/mgmt -d build/sim-mgmt
build/sim-mgmt/zephyr/zephyr.exe --flash_in_ram
west build -b native_sim/native/64 tests/simulation/serial -d build/sim-serial
build/sim-serial/zephyr/zephyr.exe
west build -b native_sim/native/64 tests/simulation/ui -d build/sim-ui
build/sim-ui/zephyr/zephyr.exe
```

Lifecycle acceptance covers initialization failure with management reachable,
confirmation failure/retry, rollback deadlines, recovery handoff, button requests
and pairing-save retry. Settings acceptance covers loading defaults, remounts,
invalid records, partial write failure, identity, pairing deletion and reset.
Deleted pairing must not return through a stale active result.

UI acceptance covers all priorities and cadence, the 500 ms management delay,
one-second drop indication, the 150/300 ms connected pulses and 3/2/1-beat cadence
on 2.4 GHz/SubG, active-band changes in automatic mode, per-band LQ boundaries,
output retry, cumulative button input and missed/locked channel reads.

Run the production RF owner for both roles and fixed bands:

```sh
for role in OFF ON; do
  for subg in OFF ON; do
    rf_build="build/sim-rf-${role}-${subg}"
    west build -b native_sim/native/64 tests/simulation/rf -d "$rf_build" -- \
      -DRF_TEST_ROLE_FOLLOWER="$role" -DRF_TEST_BAND_SUBG="$subg"
    "$rf_build/zephyr/zephyr.exe"
  done
done
```

Run automatic-mode scenarios for each role:

```sh
for role in OFF ON; do
  rf_build="build/sim-auto-${role}"
  west build -b native_sim/native/64 tests/simulation/rf -d "$rf_build" -- \
    -DRF_TEST_ROLE_FOLLOWER="$role" -DRF_TEST_BAND_AUTO=ON
  "$rf_build/zephyr/zephyr.exe"
done
```

RF scenarios must exercise real P1/ARQ peers behind simulated radio/clock/serial
edges: completion and preparation failures, missing IRQs, timeout cleanup,
periodic synchronization, pacing, power changes and diagnostic pagination.
Automatic capture must reject user DATA until phase proof, retain acquisition
through a large valid offset, and resume only after reverse DATA proof.

## WSL USB forwarding

On WSL 2, forward the boards from Windows before running hardware tools. Use
usbipd-win 5.x and a WSL kernel with USB/IP VHCI and CDC ACM support. The Windows
`usbipd` service must be running, and its installed firewall rule must allow the
WSL connection. Current usbipd-win supplies its WSL client; installing Ubuntu
`linux-tools-*` or finding a `usbip` binary matching the Microsoft kernel is not
required. See [Microsoft's USB guide](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)
and [usbipd-win's WSL instructions](https://github.com/dorssel/usbipd-win/wiki/WSL-support).

For a new Windows installation, run in administrator PowerShell:

```powershell
winget install --interactive --exact dorssel.usbipd-win
Get-Service usbipd
usbipd list
```

Identify the ZLRS boards before sharing them. The current application and MCUboot
use `2fe3:0004`; do not share unrelated devices. Run `usbipd bind --busid <BUSID>`
once per board from administrator PowerShell. `Shared` persists across Windows
reboots; it does not mean the device is attached to WSL. Bus IDs belong to the
current USB topology and can change when moving boards between ports.

Keep WSL running. Attach each shared board from a normal WSL terminal; `attach`
does not require Windows administrator privileges:

```sh
usbipd.exe list
usbipd.exe attach --wsl --busid 1-2
usbipd.exe attach --wsl --busid 4-1
lsusb -d 2fe3:0004
ls -l /dev/serial/by-id
python3 tools/zlrs.py list
```

Replace the example bus IDs with those from `usbipd.exe list`, and verify that
both change from `Shared` to `Attached`. Use each board's USB serial and
`/dev/serial/by-id` names for tests; `/dev/ttyACM*` numbering can change at every
enumeration. Management is application interface `if02`, the debug/unit-test
console is `if04`, and MCUboot recovery is `if00`. These modes have different
product strings and preserve the board's USB serial.

Flashing and rollback reset USB. Before a hardware run, keep one foreground
auto-attach command per board running in separate terminals:

```sh
usbipd.exe attach --wsl --busid 1-2 --auto-attach --unplugged
```

Run the same command with the other board's bus ID in a second terminal. This
loop reattaches after a reset or unplug/replug on that port; it must remain
running throughout application → MCUboot → fixture → rollback. Use bus IDs
instead of `--hardware-id` when both boards have the same VID/PID. After testing,
stop the loops with Ctrl+C. To return a board to Windows, stop its loop first,
then run `usbipd.exe detach --busid <BUSID>`; otherwise it immediately reattaches.
Start the loops again after restarting WSL or Windows.

Forwarded USB can take several seconds to re-enumerate. For manual updates use
`python3 tools/zlrs.py update --serial <serial> --image <signed.bin> --timeout 45`;
the hardware unit runner already allows 45 seconds for recovery discovery.

For permission failures, check `id -nG` and `ls -l /dev/ttyACM*`. Ubuntu serial
nodes normally require the `dialout` group. If needed, run
`sudo usermod -aG dialout "$USER"` and start a new login session (or use
`newgrp dialout`). If Windows lists a board as attached but no serial nodes
appear, inspect `lsmod` for `vhci_hcd`, `usbip_core` and `cdc_acm`, and inspect
`dmesg` for enumeration errors. A subprocess sandbox can block Windows interop
with `UtilBindVsockAnyPort: socket failed 1` or hide device access; retry from a
normal WSL terminal or an explicitly approved process outside that sandbox.

## Hardware acceptance

Build LF/HF production and debug configurations with matching MCUboot images.
Inspect ELF placement for critical RF, launch, GPIO timestamp, timer and crypto
paths in SRAM. Verify polling SPI and absence of DMA allocation on radio/LED paths.

Before a test changes settings, save the complete settings partition and the
original image hash. Preserve device identity, root key, pairing and configuration.
Unconfirmed fixtures must restore the original partition/image and verify both.
Do not confirm fixtures. Restoration is an independent requirement and cannot
hide failed test assertions. Stop task-owned helper processes and restore the
agreed band/configuration when testing ends.

The hardware unit fixture covers protocol, hardware SHA-256, entropy, persistence,
logs, radio operations and timer behavior:

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf \
  tests/hardware/unit -d build/unit
python3 tests/hardware/unit/run.py --serial <serial> \
  --image build/unit/unit/zephyr/zephyr.signed.bin --log build/unit.log
```

For LED-only acceptance without settings or persistence test cases, build the
isolated fixture and use the same runner. The expanded per-band cadence cases
take about 150 seconds, so the runner and hardware harness allow 300 seconds:

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf \
  tests/hardware/unit -d build/unit-ui -- -Dunit_ZLRS_TEST_UI_ONLY=ON
python3 tests/hardware/unit/run.py --serial <serial> \
  --image build/unit-ui/unit/zephyr/zephyr.signed.bin --log build/unit-ui.log
```

The LED fixture records UI output timestamps and forwards accepted frames to the
board's physical LED strip when available. Connected acceptance checks 150 ms
pulses and 300/600/900 ms rising-edge intervals on 2.4 GHz, and 300 ms pulses and
600/1200/1800 ms intervals on SubG. It covers both role colors, automatic and fixed
band modes, the 95%/99% quality boundaries on automatic 2.4 GHz and 50%/80% on
fixed 2.4 GHz and both SubG modes.

Entropy acceptance includes successive and partial-block reads, health-check
failure, bounded timeout with USB available, propagation to handshake refusal and
successful reads after recovery. Timer acceptance includes physical period changes
and the reserved transition guard without resetting frame count.

Management acceptance covers malformed commands, values and lengths, write
failure, confirmation retry, identity generation, pairing deletion and reset:

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf \
  tests/hardware/mgmt -d build/mgmt-hardware
python3 tests/hardware/mgmt/run.py --serial <serial>
```

See the [management fixture](../tests/hardware/mgmt/README.md) for restoration
requirements. Initialization-fault acceptance uses a separate image:

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf \
  tests/hardware/application -d build/application-fault
python3 tests/hardware/application/run.py --serial <serial> \
  --image build/application-fault/application/zephyr/zephyr.signed.bin \
  --log build/application-fault.log
```

Confirm the fault image exposes real management/recovery and rolls back to the
original hash. Normal upgrade scenarios separately exercise confirmation and
rollback deadlines.

### Bidirectional DATA and transitions

For a 2.4 GHz profile sweep, build the radio fixture with hopping enabled and
keep both boards in saved automatic-band mode. The runner negotiates profiles
0→1→2→3→4→3→2→1→0 and verifies DATA in both directions at every step. It leaves
the fixture unconfirmed and verifies rollback to the original image hashes:

```sh
mkdir -p build
cat >build/phy-acceptance.conf <<'EOF'
CONFIG_ZLRS_APP_FHSS=y
CONFIG_ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS=600000
EOF
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf \
  tests/hardware/radio -d build/radio-phy -- \
  -Dradio_EXTRA_CONF_FILE="$PWD/build/phy-acceptance.conf"
python3 tests/hardware/radio/run.py --serial <Leader-serial> --peer <Follower-serial> \
  --image build/radio-phy/radio/zephyr/zephyr.signed.bin \
  --profiles --bytes 8193 --rounds 1 --timeout 45 --log build/radio-phy.json
```

Use paired product images and deterministic bytes:

```sh
python3 tests/hardware/app/link/test_data.py \
  --leader-port <Leader-serial> --follower-port <Follower-serial> --bytes 8192 --rounds 2
```

Test both fixed bands and automatic mode, with short and maximum-length packets,
continuous duplex load, slow host consumption and source backpressure. Require
byte-exact, ordered delivery without duplicate bytes.

Automatic acceptance covers sustained poor quality, one-way/total loss, lost
announcements, a lost ACK with queued DATA, preferred-band probe success/failure,
staggered startup, reboot of either peer, short and long management pauses.
Observe without pausing RF during measurement. Require actual DATA before,
through and after a transition, preserved identity/pairing, continuous session
counters for band-only changes and an unchanged duty budget. A pending plan alone
does not satisfy transition coverage; observation must include its execution and
bidirectional recovery.

### One-sided DATA loopback

For a walk test with one board attached to the host, connect the TX board's DATA CDC to the
host and short DATA TX to DATA RX on the remote peer. Send a deterministic pseudorandom byte
stream and verify the exact returned stream with:

```sh
python3 tools/zlrs_loopback.py --device <TX-serial> --duration 120 \
  --chunk-size 32 --interval-ms 50
```

The runner saves transmitted and received binary files, a byte-count timeline and a summary
under `artifacts/loopback-*`. If the TX board exposes the debug console CDC, it also samples
non-pausing link and DATA-port status. Use `--no-status` for data-only capture or `--console`
to select a specific debug console port. Management status is not polled during the run.

### PHY and FIFO isolation

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf \
  tests/hardware/radio -d build/radio
python3 tests/hardware/radio/run.py --serial <Leader-serial> --peer <Follower-serial> \
  --log build/radio.json --timeout 60
```

The radio fixture uses the normal preparation window with hopping disabled for
fixed-channel measurement. A separate hopping-enabled configuration provides a
comparison. The runner captures counters and reports on success and timeout,
including image hashes, and verifies rollback to both original images.

The `test_radio channels` report bins 2.4 GHz completed/failed TX, modem-valid RX,
empty/failed windows and CRC errors across 79 DATA channels. Counts precede
authentication; empty windows include peer silence and are not packet-error rate.
SubG operation leaves those bins empty.

RAW testing requires a separate build with
`-Dradio_EXTRA_CONF_FILE="$PWD/tests/hardware/radio/raw.conf"` and runner options
`--image <signed-image> --raw`. Select fixed 2.4 GHz first. RAW uses the configured
modem and paired sync word; its defaults are 2441 MHz and a 32-byte
sequence-numbered payload, configurable from 6–511 bytes. A reverse fixture swaps
physical direction without changing stored roles.

RAW reports valid RX, failed TX, damaged payloads and expected packets between
its first/last valid sequence. Quality excludes startup and tail outside that
span. Use the hardware launch notification for prompt FIFO tail service and
record underflows and timing on failure. Successful restoration or some valid
packets do not establish zero PHY loss.

## Evidence requirements

Acceptance proceeds through host checks, native scenarios, SKU/build checks and
hardware scenarios. Record the tested source revision, resolved configuration,
image file hash, MCUboot payload hash, device identities, commands, observations,
byte comparisons and failures in a separate result directory. Report coverage
limits explicitly; a build or simulation cannot substitute for RF qualification
on the corresponding SKU and region.
