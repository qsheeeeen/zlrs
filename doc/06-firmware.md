# Firmware operation

This document specifies DATA endpoints, configuration, builds, storage and the
signed-image lifecycle. Hardware is described in [prototype](02-prototype.md) and
the [board reference](../boards/zlrs/rp2350_zero_core2021/README.md). Wire behavior
is specified in [air protocol](07-protocol.md).

## DATA endpoints

Production firmware exposes DATA USB CDC at `if00` and management CDC at `if02`.
The debug overlay enables an additional UART console CDC for kernel output and
logs. Native MCUmgr SMP uses management CDC `if02` in all images. No image has an
interactive shell. MCUboot has a separate
recovery CDC at `if00`.
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
and debug-console ports remain USB-only and do not change this DATA policy.

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
| `set-region` | 0 FCC915, 1 EU868, 2 CN470, 3 CN433; must belong to the board’s compiled `regions` array |
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

Each endpoint uses authenticated reverse RSSI to control its own TX. RSSI updates
on every accepted packet; DATA reports the measurement available at finalization.
Normal control filters RSSI in elapsed time (500 ms time constant), rather than
counting packets. The weak boundary follows the active PHY's existing initial
10 dB receive margin; the lowering boundary is 6 dB stronger. SubG uses -95/-89 dBm.
These are initial policy values requiring RF calibration, not measured sensitivity.

| Observation | Decision |
| --- | --- |
| Filtered RSSI below the active weak boundary continuously for 500 ms | Raise 3 dB |
| Filtered RSSI above the lowering boundary continuously for 2 s | Lower 3 dB |
| RSSI between the two boundaries | Hold |
| One newly completed LQ window at 50–94% | Raise once by 3 dB; suppress lowering for 2 s |
| One newly completed LQ window below 50% | Restore maximum; suppress lowering for 5 s |
| LQ at least 95%, or no completed window | Leave normal RSSI control active |
| Missing/invalid RSSI or expired reverse feedback | Restore maximum immediately |

Each actual power change discards the RSSI filter and qualification intervals.
Only feedback received after a settling interval can qualify another RSSI action.
Settling is at least 500 ms and two complete TDD periods, covering feedback
propagation in both directions. Reverse feedback expires after max(1 s, two TDD periods + 500 ms).
Repeated reads of a feedback timestamp never advance the policy. Qualification
and hold timers support clock wraparound. All setpoints clamp to the active range.

LQ uses 128-attempt windows. The LQ byte carries a one-bit update
marker as specified in [the air protocol](07-protocol.md#sequence-delivery-and-quality).
A cached low LQ neither repeats a raise nor restarts a hold or RSSI qualification.
Each new window acts once even when its percentage equals the preceding window.
An ambiguous marker following a feedback gap only establishes a new baseline.
RAM generations distinguish these baselines from window completions; none of
these counters or timestamps adds air bytes. RSSI retains its full 1 dB encoding.

Automatic 2.4 GHz degradation also consumes these observations. Weak RSSI in
either direction restores both endpoints' local TX ceilings before qualification.
After settling and post-boost reception/reverse feedback, weakness must persist
for two seconds before Leader plans a slower PHY. Recovery releases the ceiling.
LQ alone no longer causes an immediate ceiling override on every feedback packet.
An LQ-based downgrade requires local maximum power throughout a complete new
window: the first post-settling boundary is discarded because it can contain
lower-power history, and the next completed window must remain below 95%.
Leaving maximum, a power epoch change or an uncertain quality baseline invalidates
that direction's window evidence. The 60-second healthy upgrade uses valid
bidirectional LQ at least 95% and the next PHY's 6 dB RSSI margin; an isolated
98% window cannot strand a strong link on a slow PHY. Contact-loss searches
remain active. Unknown LQ cannot qualify a PHY upgrade.

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
`power` provide detailed reports. Online management status provides copied
observations without pausing RF. See [development](10-debug.md#observing-a-running-link).

## Build environment

The manifest pins Zephyr v4.5.0 and Semtech `usp_zephyr`/`usp` v1.2.1. ZLRS is the
west manifest repository; the build environment selects Zephyr SDK 1.0.0.

```sh
tools/setup-zephyr.sh
export ZLRS_BOARD=rp2350_zero_core2021/rp2350a/m33_0/lf
source tools/zephyr-env.sh
zlrs-build
```

Use `rp2350_zero_core2021/rp2350a/m33_0/hf` for an HF module. The setup command
installs workspace, build and host-tool dependencies and prints SDK installation
instructions if required. `ZLRS_WS`, `ZLRS_VENV`, `ZLRS_SDK_DIR`, `ZLRS_BOARD` and
`ZLRS_APP` select the workspace, virtual environment, SDK, board and application.
The default workspace is the repository's parent; default output is `../build`.

Sysbuild produces MCUboot and its signed application with matching partitions and
signing type. Runtime parameters come from persistent settings; modem timing,
FHSS, ARQ, dynamic power, watchdog and debug facilities are build-time choices.
Production configuration enables P1, hopping, ARQ, dynamic power and watchdog.
Debug configuration enables UART kernel output, diagnostic reports and a 1% LED
brightness setting. The console has no interactive commands.

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

Application values use Zephyr Settings with its native NVS backend. Keys live
under `zlrs/`; values retain their explicit byte representation.

| Settings key | Stored value | Bytes |
| --- | --- | --- |
| `zlrs/schema` | Schema marker `0x5A4C0002` (reset intent `0x5A4CFFFE`) | 4 |
| `zlrs/band` | Band mode: 0 fixed 2.4, 1 fixed SubG, 2 automatic | 1 |
| `zlrs/baud` | UART baud, little endian | 4 |
| `zlrs/region` | SubG region | 1 |
| `zlrs/role` | Role | 1 |
| `zlrs/profile` | Security profile | 1 |
| `zlrs/key` | Root key | 32 |
| `zlrs/id` | Device ID | 4 |
| `zlrs/pair` | Peer ID and initiator/responder pairing nonces | 12 |

The selected region is stored as one byte under `zlrs/region`; the board's
`regions` array is compiled into the firmware. Management status exposes that
array as a `regions` bit mask. `set-region` rejects an ID outside the array without
changing saved settings. A missing region record uses `default-region`.

Missing optional values use validated defaults. No legacy records or older schema
are imported. A missing/invalid schema, invalid stored settings (including a region
outside `regions`) or read failure clears the complete storage
partition and initializes defaults and a new device identity. Root key and pairing
must then be provisioned again. Factory reset uses the same full erase path.
A durable reset marker prevents an interrupted erase from reactivating credentials.
Only the NVS instance owned by Settings is used; native loading rebuilds its name
metadata after an erase.

Logs use LittleFS with up to eight 128 KiB files and overwrite the oldest file.
Messages remain in RAM while RF runs and flush at boot or maintenance boundaries.
A finite RAM log buffer retains newer records on overflow. The board fstab lets
Zephyr mount the log filesystem at boot; LittleFS owns formatting and recovery.
Log export retries an unavailable mount. Unrecoverable log failure must not
prevent the rest of the device from operating. Fatal faults produce a crash dump
and reboot. Runtime queues and session state are not flash-persistent.
