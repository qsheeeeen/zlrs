# Firmware operation

This document specifies DATA endpoints, configuration, builds, storage and the
signed-image lifecycle. Hardware is described in [prototype](02-prototype.md) and
the [board reference](../boards/zlrs/rp2350_zero_core2021/README.md). Wire behavior
is specified in [air protocol](07-protocol.md).

## DATA endpoints

Production firmware exposes DATA USB CDC at `if00` and management CDC at `if02`.
Debug firmware adds a console CDC. MCUboot has a separate recovery CDC at `if00`.
The DATA UART uses the board's assigned pins and stored baud rate.

USB and UART are alternative endpoints of one byte stream. UART is the default,
including startup and charge-only USB power. USB takes priority only after a
host has enumerated and selected a nonzero device configuration and the bus is
not suspended. Opening the DATA port, asserting DTR or sending initial bytes is
not required. Receive counters never determine endpoint selection.

USB reset, removal, deconfiguration or an unrecoverable USB error returns DATA
to UART. Bus suspension also selects UART because a powered board may lose its
USB host without losing supply power or receiving a VBUS-removal event. Resume
restores USB priority only if its device configuration remains valid. Management
and console ports remain USB-only and do not change this DATA policy.

If UART is unavailable and USB has no active host, no DATA sink is selected and
radio reception withholds its DATA ACK. Endpoint availability is independent of
current selection. Input on the inactive endpoint is discarded and counted;
accepted output remains assigned to its original endpoint and is never duplicated
onto another endpoint when selection changes.

Each available serial endpoint has a 4096-byte input ring and a 4096-byte output
ring. USB stops accepting more input when its ring fills and resumes when space
returns. A UART with `hw-flow-control` uses the same pause/resume path: leaving
its hardware FIFO unread deasserts RTS at the FIFO watermark, and peer CTS gates
transmission. Both peers must connect and enable RTS/CTS. UARTs without flow
control continue receiving and count overflow.
Incoming radio DATA is accepted atomically into the selected output ring;
insufficient space withholds the radio ACK and allows retransmission.

## Configuration and pairing

Use the management tool from the repository root:

```sh
python3 tools/zlrs.py list
python3 tools/zlrs.py status --serial <serial>
```

Configure both endpoints with a common root key or phrase and opposite roles:

```sh
python3 tools/zlrs.py set-role leader --serial <Leader-serial>
python3 tools/zlrs.py set-role follower --serial <Follower-serial>
python3 tools/zlrs.py set-phrase 'shared binding phrase' --serial <Leader-serial>
python3 tools/zlrs.py set-phrase 'shared binding phrase' --serial <Follower-serial>
python3 tools/zlrs.py set-band auto --serial <Leader-serial>
python3 tools/zlrs.py set-band auto --serial <Follower-serial>
```

Reboot both endpoints after changing role, band, region, profile or
UART baud, then start pairing on both:

```sh
python3 tools/zlrs.py pair --serial <Leader-serial>
python3 tools/zlrs.py pair --serial <Follower-serial>
```

Holding the button for one second opens the same 30-second pairing window. In
automatic mode pairing uses 2.4 GHz; fixed mode uses the selected band. Matching
protocol version, band mode, SubG region, profile and timing are required.

| Setting or command | Accepted value / effect |
| --- | --- |
| `set-band` | `auto`, `2.4`, `subg`; default `auto` |
| `set-role` | `leader`, `follower`; default Leader |
| `set-region` | 0 FCC915, 1 EU868, 2 CN470, 3 CN433; must fit the module SKU |
| `set-baud` | 9600–4000000; default 115200 |
| `set-profile` | `p1` in production; debug also accepts `p0`; `p2` is unsupported |
| `set-phrase` | 1–32 UTF-8 bytes, derived on the device |
| `set-key-hex` | 64 hexadecimal characters for a 32-byte root key |
| `erase-pair` | Delete the stored pairing and invalidate active pairing |
| `new-dev-id` | Generate a new identity and erase its pairing |
| `factory-reset` | Clear settings, identity, root key and pairing; reboot to provision identity and apply defaults |

Root-key writes and credential deletion invalidate active credentials immediately.
Ordinary settings are saved for the next boot. Status combines saved settings and
live link observations; after configuration, reboot before treating every reported
value as active. Region power ceilings are operating policy; the physical PA and
antenna determine achievable RF output.

## Automatic band operation and power

Automatic mode starts on 2.4 GHz FLRC 2.6 Mbps. Sustained poor bidirectional
quality first steps through 650 kbps FLRC, 325 kbps FLRC, LoRa SF6 and LoRa SF7
on 2.4 GHz. Only the final profile falls back to SubG. Continuously healthy
feedback qualifies a faster profile after 60 seconds; SubG first probes 2.4 GHz
LoRa SF7. Complete loss invokes asymmetric searches across these profiles.
Switches preserve session and DATA progress, with transmission paused during
capture. Exact thresholds, announcement and recovery rules are specified in
[automatic band switching](07-protocol.md#automatic-band-switching).

Each endpoint controls its own transmit power from authenticated peer feedback.
The active band's board capability and operating policy define the range; there
is no saved user power ceiling. The prototype supports −17 to +12 dBm on 2.4 GHz
and −10 to +22 dBm on SubG, whose maximum is additionally bounded by the selected
region. Status reports this effective range and the transmit setpoint.

Pairing uses −10 dBm, bounded by the active range. Leaving pairing restores the
maximum before session establishment. Startup, disconnection, invalid session,
radio capture and feedback loss restore the active band's maximum before the
next transmission. A band change starts at the new band's maximum.

Automatic 2.4 GHz degradation takes priority over normal power saving. Either
direction crossing the current profile's poor-quality boundary immediately
restores local TX to its allowed maximum on both roles. Reverse DATA carries
the weak-direction RSSI/LQ to the other endpoint, which applies the same rule.
The Leader waits for reception and reverse feedback newer than the boost, then
requires two continuous seconds of poor feedback at maximum before planning a
slower profile. Recovery cancels that qualification and resumes normal power
saving. Firmware on both endpoints must implement this policy; no new air fields
are required.

Power decisions run at each radio preparation boundary, independently of logging.
Each normal adjustment requires three consecutive distinct fresh observations
and changes the setpoint by 3 dB, bounded by the active range:

| Observation | Decision | Minimum wait |
| --- | --- | --- |
| LQ below 99%, or RSSI below −95 dBm | Raise toward the maximum | 2 s since the last change |
| LQ at least 99% and RSSI above −70 dBm | Lower toward the minimum | 40 s of continuously healthy feedback since the last change |
| Other fresh feedback | Hold | — |
| Missing/invalid feedback, or feedback at least 1 s old | Restore the maximum | Before the next transmission |

The saving threshold tolerates one lost packet in a 128-attempt quality window;
isolated loss must not hold one strong direction at maximum while its peer saves power.
Repeated reads of one observation never advance qualification. Any non-healthy
observation or feedback gap of at least one second resets the lowering interval.
Recovery bypasses the normal raising delay and step size. Millisecond timers
support clock wraparound. Thresholds require RF calibration against measured loss.

Debug images provide `zlrs power <dBm>` on the console to hold a fixed setpoint
within the active range while connected, and `zlrs power auto` to resume automatic
control at maximum power. The override is volatile and clears on disconnection,
feedback loss, pairing, retuning, automatic 2.4 GHz power recovery or reboot.
Pairing and recovery take priority.

## Management and diagnostics

The tool normally enters management, executes one command and exits. `--serial`
selects a device; `--port` selects its management port. `--keep` leaves the session
open, and `exit` restores RF. Idle timeout is 300 seconds.

Management pauses RF at a frame boundary. Successful `ENTER` confirms readiness
and synchronous log flushing; the timeout starts after that flush. Repeated
`ENTER` flushes again. Accepted DATA remains queued, with USB backpressure when
capacity fills. A pause can make the peer search or reduce its observed LQ. Short
pauses without a pending plan retain capture; long pauses and interrupted plans
require reacquisition.

`status` reports local/peer RSSI and LQ, power, active band, capture, transition,
frame/error counters, hopping, MCU temperature and LR2021 sensors/status.
`logs [directory]` exports flash logs. Debug-only `link`, `rate`, `radio` and
`power` provide detailed reports. The debug shell provides copied observations
without pausing RF. See [development](10-debug.md#observing-a-running-link).

## Build environment

The manifest pins Zephyr v4.4.0 and Semtech `usp_zephyr`/`usp` v1.2.1. ZLRS is the
west manifest repository; the build environment selects Zephyr SDK 1.0.0.

```sh
tools/setup-zephyr.sh
export ZLRS_BOARD=rp2350_zero_core2021/rp2350a/m33/lf
source tools/zephyr-env.sh
zlrs-build
```

Use `rp2350_zero_core2021/rp2350a/m33/hf` for an HF module. The setup command
installs workspace, build and host-tool dependencies and prints SDK installation
instructions if required. `ZLRS_WS`, `ZLRS_VENV`, `ZLRS_SDK_DIR`, `ZLRS_BOARD` and
`ZLRS_APP` select the workspace, virtual environment, SDK, board and application.
The default workspace is the repository's parent; default output is `../build`.

Sysbuild produces MCUboot and its signed application with matching partitions and
signing type. Runtime parameters come from persistent settings; modem timing,
FHSS, ARQ, dynamic power, watchdog and debug facilities are build-time choices.
Production configuration enables P1, hopping, ARQ, dynamic power and watchdog.
Debug configuration adds the console and a 1% LED brightness setting.

## Installation, updates and rollback

The boot chain is ROM → MCUboot → signed primary application. ECDSA-P256 signatures
protect image authenticity. MCUboot uses swap-using-move and an inactive upload
slot. Wireless root keys and firmware signing keys serve separate purposes.
Development builds use MCUboot's bundled test key; release builds require a private
signing key and matching bootloader trust anchor.

Install MCUboot once by holding BOOTSEL while connecting USB and copying
`../build/mcuboot/zephyr/zephyr.uf2` to `RPI-RP2`, or through SWD. Then upload the
signed application to recovery:

```sh
python3 tools/zlrs.py update --no-trigger --serial <serial> \
  --image ../build/app/zephyr/zephyr.signed.bin
```

For subsequent updates:

```sh
python3 tools/zlrs.py update --serial <serial> \
  --image ../build/app/zephyr/zephyr.signed.bin
```

The updater triggers recovery, identifies the inactive slot, uploads and marks a
test image, then verifies the running MCUboot image hash and confirms it. Upload
completion alone does not prove installation. The application remains a test
image until confirmation succeeds. A failed confirmation remains retryable.

The default confirmation deadline is 60 seconds of application uptime. An
unconfirmed image resets for rollback when that deadline is checked, or after an
initialization fault. Management sessions defer ordinary main-loop checks until
they end. The application watchdog uses an 8-second timeout outside management;
management disarms it for synchronous work and rearms it on exit. MCUboot uses a
30-second watchdog.

`recovery` sets a retained boot-mode flag and reboots into MCUboot serial recovery.
MCUboot also offers a 2-second serial-entry window after reset and stays in recovery
when no application is bootable. BOOTSEL installs only MCUboot; every application
must use signed serial recovery. Changing the signing trust anchor requires
reinstalling MCUboot.

## Persistent storage and logs

The [board reference](../boards/zlrs/rp2350_zero_core2021/README.md#flash-partitions)
defines image slots, the 1 MiB log filesystem, 128 KiB crash area and 32 KiB NVS.

| NVS record | Stored value | Bytes |
| --- | --- | --- |
| 0 | Layout marker `0x5A4C0002` | 4 |
| 1 | Band mode: 0 fixed 2.4, 1 fixed SubG, 2 automatic | 1 |
| 2 | UART baud, little endian | 4 |
| 3 | SubG region | 1 |
| 4 | Role | 1 |
| 5 | Security profile | 1 |
| 6 | Root key | 32 |
| 7 | Device ID | 4 |
| 8 | Peer ID and initiator/responder pairing nonces | 12 |

Missing settings use validated defaults. Invalid record lengths or values do not
become active settings. A missing or invalid layout marker reinitializes the
settings area; factory reset clears it and writes a fresh marker. An unreadable
NVS mount may require partition reinitialization, so corrupt storage cannot
promise retention of credentials.

Logs use LittleFS with up to eight 128 KiB files and overwrite the oldest file.
Messages remain in RAM while RF runs and flush at boot or maintenance boundaries.
A finite RAM log buffer retains newer records on overflow. Log filesystem mount
failure is retried before reinitialization; unrecoverable log failure must not
prevent the rest of the device from operating. Fatal faults produce a crash dump
and reboot. Runtime queues and session state are not flash-persistent.
