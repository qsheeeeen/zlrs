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
export ZLRS_BOARD=rp2350_zero_core2021/rp2350a/m33_0/lf
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

## Bounded trace capture

The trace debug configuration records one bounded interval using Zephyr CTF,
then saves and exports the frozen snapshot separately. Build it with:

```sh
source tools/zephyr-env.sh
zlrs-build -d "$ZLRS_REPO_ROOT/build/trace-debug" -- \
  '-DEXTRA_CONF_FILE=debug.conf;trace-debug.conf' \
  -DDTC_OVERLAY_FILE=debug.overlay
```

The trace configuration allows ten minutes to inspect an unconfirmed test image
before rollback. It does not confirm the image. Ordinary debug and production
images omit tracing and the trace filesystem service.

### Capture contract

Tracing records one explicitly requested interval into RAM. It is disabled at
boot, disabled between captures and never enabled by a saved setting. Production
and ordinary debug images omit tracing; a separate trace debug configuration
includes it. Compiled-in hooks still have a runtime enable check when recording
is off.

A capture reserves 64 KiB of RAM. The default interval is 50 ms and the accepted
range is 10–5000 ms. The default is shorter than the measured capacity window;
high event rates can still fill the buffer before the deadline. Duration bounds
recording time; it does not promise that all events fit in RAM. The existing
Zephyr RAM backend retains the first events and discards
subsequent events after it fills. It does not overwrite earlier events.

A Zephyr kernel timer stops recording at the deadline, independently of the host
connection and RF owner's polling cadence. Manual stop can end it earlier. The
actual start and stop times are recorded because normal interrupt latency can
delay timer dispatch. Disconnecting the console does not extend the interval.
Stopping freezes RAM and leaves RF running. Saving is a separate explicit action
which pauses RF at the existing maintenance boundary.

### Zephyr facilities and application ownership

Use the Zephyr v4.5.0 CTF format, synchronous tracing and built-in RAM backend.
Synchronous output copies records to RAM; it performs no Flash or transport I/O.
Enable the existing start-disabled behavior through `TRACING_HANDLE_HOST_CMD`.
The application uses `tracing_set_enabled()` to control each interval. Leave the
unbounded `TRACING_SHELL` enable command disabled in this configuration.

The configuration requests thread switching, ordinary ISR, semaphore and idle
events. Zephyr v4.5.0's GPIO function hooks do not consult `TRACING_GPIO`.
The trace build therefore uses Zephyr's `sys_port_trace_<type>_is_disabled`
API-type masks for `gpio`, `gpio_pin` and `gpio_port` when GPIO tracing is off.
It applies the `k_thread` mask only to the kernel's `busy_wait.c`, suppressing
busy-wait events while keeping scheduling events in the other kernel sources.
SPI chip-select handling calls `k_busy_wait(0)` twice per transaction; these
events add overhead without measuring a delay. These compile-time filters leave
the native CTF hooks, encoding and RAM backend intact. Add RF phase
boundaries through `sys_trace_named_event()` with frame/slot identifiers. Keep
names within the CTF limit of 19 visible characters. No user DATA, keys or pairing
secrets belong in named event arguments.
GPIO callback events remain available to identify interrupt delivery.

RF annotations check Zephyr's recording state before calling the named-event
function, avoiding its bounded string copy between captures. Compiled-in kernel
hooks still perform their native runtime checks. Compare recording off and on
under the same load before attributing a deadline miss to production firmware.

The application owns only bounded capture control, persistence and its RF phase
annotations. Zephyr owns event encoding, collection and buffering. The host uses
Zephyr's `scripts/tracing/trace_viewer.py` with the matching CTF metadata. No
application tracing backend, binary event format, decoder or histogram engine is
introduced. Compiler instrumentation is a separate diagnostic configuration.

The controller is restricted to the current single-CPU application. It serializes
all start/stop/reset/save operations. A start first disables tracing and waits
for any synchronous writer to finish. It then calls the registered RAM backend's
existing initialization operation to clear and reset the buffer on the
preemptible shell thread, before submitting the start request to the RF owner.
The RF owner can preempt the full-buffer clear to meet its slot deadlines.
The clear must not run with interrupts locked or on the cooperative RF owner.
The owner rechecks capture state and maintenance mode after reset. Enabling
recording, emitting the begin marker and arming the deadline form one bounded
operation after reset; a deadline callback cannot race a pending enable. A stop
disables recording and waits for the last writer before exposing an immutable
snapshot. Repeated capture must not inherit a prior deadline callback.

The zero-latency launch ISR and its RAM call closure remain free of tracing,
logging and instrumentation calls. Its existing hardware timestamps are emitted
later by the RF thread as named event arguments. These timestamps identify the
hardware event; the enclosing CTF timestamp identifies the later emission.

### Commands and state

The trace debug shell exposes these lifecycle operations:

| Command | Behavior |
| --- | --- |
| `zlrs trace start [duration_ms]` | Validate duration, reset RAM and record one interval; omitted duration means 50 ms |
| `zlrs trace stop` | Freeze an active capture; repeated stop on a frozen capture succeeds |
| `zlrs trace status` | Report state, capture ID, requested/actual interval, stop reason, RAM capacity, core drop count, saved generation and last error |
| `zlrs trace save` | Save the frozen snapshot through an RF maintenance boundary; success requires committed files |
| `zlrs trace discard` | Release a frozen RAM snapshot without modifying saved files |

| State | Allowed transitions |
| --- | --- |
| IDLE | Start → CAPTURING |
| CAPTURING | Deadline, stop or accepted maintenance entry → FROZEN |
| FROZEN | Save → SAVING; discard → IDLE |
| SAVING | Commit → SAVED; failure → FROZEN with error |
| SAVED | Start → CAPTURING; discard → IDLE |

Reject a new start while capturing, saving or holding an unsaved frozen snapshot.
Start is also rejected during maintenance, pairing persistence or firmware
installation. Existing owner serialization resolves concurrent requests. An
accepted maintenance request freezes an active capture before log flushing or
Flash operations. The deadline callback performs only the bounded stop/marker
operation and notification; filesystem work runs in thread context.

Start and stop emit `cap_begin` and `cap_end` named events, carrying the capture
ID and requested duration or stop reason. Stop reasons are deadline, manual or
maintenance. Identifiers distinguish successive captures within one boot; saved
metadata also identifies the firmware build and its start/stop clock interval. After reboot the RAM state
is IDLE and committed files remain available. Their inventory is validated on
entering management, before filesystem downloads are accepted. Recording never
resumes itself.

### RAM backend limits and completeness

The stock RAM backend exposes its `ram_tracing` array, but keeps its used length
and full flag private. Its initialization operation resets these values. The
core dropped-packet counter does not account for discards inside the RAM backend.
The application must not access private static variables, infer their layout or
claim that a zero core drop count proves a complete capture.

Save the entire fixed-size array, following Zephyr's documented RAM dump method.
The file therefore contains a valid CTF prefix and may contain zero padding.
Record the file size and capacity; report the exact used length and backend-full
state as unavailable. Do not scan CTF records in firmware to calculate them.

In the standard viewer, matching `cap_begin` and `cap_end` events plus a zero core
drop-count delta establish a complete interval for the selected categories.
The RAM backend stops accepting records after its first overflow, so an overflow
also prevents the end marker from being stored. An absent end marker means the
interval is incomplete; never present that prefix as the requested full window.
Zero padding after a valid end marker is outside the recorded interval. Viewer
handling of padded dumps is part of integration acceptance.

Automatic stop on backend fullness is not available with this stock backend.
The deadline still disables recording after an overflow. If a later requirement
needs exact occupancy or stop-on-full, use an upstream RAM-backend interface
extension rather than implementing another recorder.

### Flash save and export

The shared board description reserves a separate 256 KiB `trace_partition`:
offset `0x300000`, size `0x40000`, within the declared 4 MiB device. Application
and MCUboot use this same partition description.
Existing image, log, coredump and settings partitions retain their offsets and
sizes. Only trace debug images mount this partition as LittleFS at `/trace`.
LF/HF production and trace builds use this shared partition geometry.

Keep two bounded generations. Each contains `channel0_0`, the raw RAM snapshot,
and `capture.json`, containing the capture ID, application and Zephyr revisions,
RF hardware band from devicetree, event selection, RAM capacity and SHA-256,
requested/actual times, stop reason and core drop-count delta. The event mask describes requested
switches: bits 0–4 mean thread, ISR, semaphore, idle and named events. It does not
imply that other hooks are absent. The JSON describes the artifact;
it does not wrap or alter the CTF bytes. Archive the build's CTF `metadata` file
with the exported capture so decoding does not depend on a later Zephyr version.

Write only the inactive generation. Synchronize the data file first and publish
its complete metadata last. A generation is usable only when its metadata and
data size/hash agree. Keep the prior valid generation until the new one commits.
At boot, ignore incomplete generations. A failed save retains the frozen RAM
snapshot for retry and reports the storage error. No saved capture is deleted by
starting or discarding a RAM capture.

The RF owner grants the same maintenance exclusion used for existing Flash
operations. The saver waits for the pause before any mount, erase, program or
metadata write, including cleanup. It feeds the watchdog during bounded storage
work. A save requested during normal operation restores that mode on completion
or failure; a save inside an existing management session leaves that session
open. Storage failure must release the saver-owned pause. Capture expiry alone
does not initiate maintenance or a Flash write.

Export through Zephyr MCUmgr filesystem management over the existing debug shell
CDC using the SMP-over-shell transport. This requires trace-only MCUmgr/FS
configuration and a standard compatible host client. Allow read-only downloads
of committed trace artifacts through the filesystem access hook; file mutation
remains owned by the saver. Select the 4 GiB file-size encoding option because a
65536-byte snapshot exceeds the 16-bit CBOR size range. Do not add trace payloads
to `DEBUG_REPORT`, a new ZLRS file-transfer protocol or a dedicated USB class.
Filesystem downloads require an explicit maintenance session, with tracing off
and RF paused, to isolate transfer load from RF measurements. Serialize saves
against downloads and close active file transfers before leaving maintenance.

Unsaved RAM is volatile. Fatal handlers and watchdog-reset paths do not attempt
Flash saves. Previously committed generations can be exported after reboot;
survival of the current unsaved capture is not promised.

### Migration and acceptance

RF phase profiling uses named events. The application histogram engine, its
P50/P95/P99 text reports and the radio runner's `--timing` option have been
removed. Hardware deadlines, scheduling timestamps, driver health counters and
maintenance totals remain available.

On the console, run `zlrs trace start`, wait for the window, then use
`zlrs trace status`, `zlrs trace save` and `zlrs mgmt`. Close the terminal before
using the same console CDC with the standard client. Set the slot from the
printed status, then export and decode:

```sh
ZLRS_TRACE_SLOT=0
mkdir -p build/trace-capture
smpmgr --port "$CONSOLE" file download \
  "/trace/$ZLRS_TRACE_SLOT/channel0_0" build/trace-capture/channel0_0
smpmgr --port "$CONSOLE" file download \
  "/trace/$ZLRS_TRACE_SLOT/capture.json" build/trace-capture/capture.json
cp "$ZEPHYR_BASE/subsys/tracing/ctf/tsdl/metadata" build/trace-capture/metadata
python "$ZEPHYR_BASE/scripts/tracing/trace_viewer.py" \
  build/trace-capture/channel0_0 --metadata build/trace-capture/metadata --text
python tools/zlrs.py exit --serial <serial>
```

`CONSOLE` identifies the debug CDC (`if04`), not the management CDC (`if02`).
Compare the downloaded file's SHA-256 with `capture.json`. A padded RAM dump can
make the viewer flag undecoded tail bytes; complete coverage requires matching
start/end markers, not the absence of zero padding.

Zephyr's `ctf2perfetto.py` accepts a copy containing the decoded record prefix.
Its strict input check rejects the unused zero padding in a full RAM dump. Use
the stock viewer's decoded record sizes to extract that prefix, retain the
original dump for hash verification, then pass the copy and matching metadata
to the stock converter. This does not change the on-device capture format.

The hardware runner orchestrates the stock client and viewer; it does not
implement a trace decoder. It installs an unconfirmed image, exercises capture
and save/retry, exports artifacts, verifies persistence after reboot and restores
the original firmware hashes. With a peer, it installs the same image on both
boards, which must have the same hardware SKU. The optional peer is a paired
Follower; the traced device is the Leader for this DATA check. The DATA test
requires a complete 50 ms capture without GPIO pin/port or busy-wait events.
It also checks the existing radio deadline counters around the workload:
buffer reset happens before `cap_begin`, so a startup stall can be absent from
the CTF stream itself.

```sh
python tests/hardware/trace/run.py --serial <Leader-serial> --peer <Follower-serial> \
  --image build/trace-debug/app/zephyr/zephyr.signed.bin \
  --out build/trace-acceptance
```

Acceptance must cover:

- No recording at boot, between captures or after console disconnection and the
  deadline; production and ordinary debug images omit the trace facilities.
- Invalid durations, overlapping requests, manual/deadline/maintenance stop,
  save failure/retry, explicit discard and repeated captures in one boot.
- A complete capture and a deliberately saturated buffer decoded by the stock
  viewer with matching metadata; missing end markers and core drops are visible.
- No Flash operations during recording or normal RF operation; saving and
  exporting occur under maintenance exclusion and restore the prior mode.
- Interrupted saves, full/unavailable storage and reboot, retaining the prior
  valid generation and preventing partial artifacts from being advertised.
- Standard MCUmgr download of all 65536 bytes, matching SHA-256 and stock viewer
  decoding without an application decoder or text-to-binary reconstruction.
- LF/HF production, ordinary debug and trace debug builds, RAM/Flash budgets,
  the launch RAM closure audit and the existing 10 us launch-jitter bound under
  recording load. Compare DATA behavior with recording enabled and disabled;
  a trace build is not automatically evidence of production performance.

### References

- [Zephyr tracing and named events](https://docs.zephyrproject.org/latest/services/tracing/index.html)
- [RAM dump and standard tracing tools](https://docs.zephyrproject.org/latest/samples/subsys/tracing/basic/README.html)
- [MCUmgr filesystem management and shell transport](https://docs.zephyrproject.org/latest/services/device_mgmt/mcumgr.html)
- The pinned Zephyr tree's `subsys/tracing/tracing_backend_ram.c`,
  `subsys/tracing/include/tracing_backend.h` and `subsys/tracing/tracing_core.c`
  define the backend limits and lifecycle operations used by this design.

## Host and tool checks

```sh
cmake -S tests -B build/tests
cmake --build build/tests --parallel
ctest --test-dir build/tests --parallel 4 --output-on-failure --output-junit results.xml
```

The [test system](../tests/README.md) defines environment ownership, shared
contracts, test selection and result artifacts. Fast checks run without a
Zephyr workspace and use AddressSanitizer/UBSan by default. Required coverage
includes:

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
- Power recovery before automatic 2.4 GHz downgrades, post-boost feedback timing,
  and asymmetric attenuation with actual TX setpoints reflected in simulated RSSI.

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
and RTS to CTS and verifies returned DATA while USB is unconfigured.
Run the complete native matrix through Twister. Each configuration has its own
build and working directory, including simulated flash:

```sh
source tools/zephyr-env.sh
python -m pip install -r tests/simulation/requirements.txt
export ZEPHYR_TOOLCHAIN_VARIANT=host
unset ZEPHYR_SDK_INSTALL_DIR
python "$ZEPHYR_BASE/scripts/twister" \
  -T tests/simulation -p native_sim/native/64 \
  --outdir build/simulation --jobs 2 --inline-logs
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

The RF `testcase.yaml` covers both roles in fixed 2.4 GHz, fixed SubG and
automatic modes. This is the configuration matrix consumed by both developers
and PR CI; adding a variant does not require a separate CI loop. To select RF
checks only, add `-t rf` to the Twister command.

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

Audit the linked timed-launch closure after each production/debug build:

```sh
objdump=$(sed -n 's/^CMAKE_OBJDUMP:FILEPATH=//p' build/app/CMakeCache.txt)
python3 tools/check_ram.py build/app/zephyr/zephyr.elf \
  --objdump "$objdump" --report build/ram-audit.json
```

The gate follows direct calls/tail branches from the alarm ISR, prepared-command
callback and hardware timestamp reader. All reached code must reside in RAM;
missing symbols, unknown targets and unreviewed indirect transfers fail the
check. The alarm callback is an explicit reviewed edge in `CALLBACKS`; update
that declaration when callback binding changes. The default combined relocated
code/ramfunc budget is 32 KiB. The report also lists direct RAM-to-Flash edges
and unresolved indirect transfers outside the gated closure for review.
This check covers code placement and declared callback edges. It does not prove
data/cache placement, bound all RF/SHA/driver work or measure interrupt latency.

The target unit fixture records 128 alarm deadlines while repeatedly hashing
1024-byte buffers with the production SHA backend. Its `LAUNCH_JITTER` line reports
the sample count, hashes, maximum lateness and the radio's 10 us acceptance budget.
It fails on an early/missing alarm or a worst-case lateness beyond that budget.
Save this measurement with the image hash. Run with the normal USB connection;
additional host traffic, logging and storage load require separate measured
runs and should be recorded with the workload. Host benchmarks are described in
the [testing guide](../tests/README.md#performance-measurements).

Before a test changes settings, save the complete settings partition and the
original image hash. Preserve device identity, root key, pairing and configuration.
Unconfirmed fixtures must restore the original partition/image and verify both.
Do not confirm fixtures. Restoration is an independent requirement and cannot
hide failed test assertions. Stop task-owned helper processes and restore the
agreed band/configuration when testing ends.

The hardware unit fixture covers protocol, hardware SHA-256, entropy, persistence,
logs, radio operations and timer behavior:

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33_0/lf \
  tests/hardware/unit -d build/unit
python3 tests/hardware/unit/run.py --serial <serial> \
  --image build/unit/unit/zephyr/zephyr.signed.bin --log build/unit.log
```

For LED-only acceptance without settings or persistence test cases, build the
isolated fixture and use the same runner. The expanded per-band cadence cases
take about 150 seconds, so the runner and hardware harness allow 300 seconds:

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33_0/lf \
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
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33_0/lf \
  tests/hardware/mgmt -d build/mgmt-hardware
python3 tests/hardware/mgmt/run.py --serial <serial>
```

See the [management fixture](../tests/hardware/mgmt/README.md) for restoration
requirements. Initialization-fault acceptance uses a separate image:

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33_0/lf \
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
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33_0/lf \
  tests/hardware/radio -d build/radio-phy -- \
  -Dradio_EXTRA_CONF_FILE="$PWD/build/phy-acceptance.conf"
python3 tests/hardware/radio/run.py --serial <Leader-serial> --peer <Follower-serial> \
  --image build/radio-phy/radio/zephyr/zephyr.signed.bin \
  --profiles --bytes 8193 --rounds 1 --timeout 45 --log build/radio-phy.json
```

Per-phase execution timing uses the [bounded CTF capture](#bounded-trace-capture)
and standard viewer. Capture only the interval of interest. The radio fixture's
DATA acceptance and status-polling load remain independently selectable.

Maintenance pause count/duration and frame catch-up totals span RF initialization.
"Skipped after pause" identifies catch-up immediately following maintenance;
"without pause" identifies catch-up with no preceding maintenance. These labels
describe owner scheduling context. Timer and radio launch IRQ delays remain
separate observations. Compare fixed-PHY rounds on the same image and load:

```sh
python3 tests/hardware/radio/run.py --serial <Leader-serial> --peer <Follower-serial> \
  --image build/radio-phy/radio/zephyr/zephyr.signed.bin \
  --phy 0 --status-poll-hz 0 50 0 --bytes 524289 --rounds 3 \
  --timeout 90 --log build/radio-timing.json
```

The same radio fixture can stall both TRNGs while DATA continues. Use one
sustained round, with enough bytes to cover the entire fault window:

```sh
python3 tests/hardware/radio/run.py --serial <Leader-serial> --peer <Follower-serial> \
  --image build/radio-phy/radio/zephyr/zephyr.signed.bin \
  --bytes 262145 --rounds 1 --entropy-stall-ms 3500 --timeout 60 \
  --log build/radio-entropy-stall.json
```

The RF owner drains cached entropy, stalls hardware collection and probes its
actual nonce callback while transmitting. Delayed work restores sampling. Require
both directions to make byte-exact DATA progress during the active fault,
unavailable nonce probes to return without hardware waits, and original firmware
rollback on both boards. The unit fixture separately covers concurrent entropy
consumers, short reads, IRQ calls, deadline/recovery and other workqueue activity.

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
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33_0/lf \
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
