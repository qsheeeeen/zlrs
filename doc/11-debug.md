# Development and debugging

Use the production build to validate normal DATA operation. A debug build adds a console CDC and detailed management reports:

```shell
source tools/zephyr-env.sh
zlrs-build -p auto -- -DEXTRA_CONF_FILE="$ZLRS_REPO_ROOT/app/debug.conf" -DDTC_OVERLAY_FILE="$ZLRS_REPO_ROOT/app/debug.overlay"
```

After loading it, `python3 tools/zlrs.py link|rate|radio|power --serial <serial>` reads copied diagnostics. `power` reports the configured ceiling, active DATA setpoint and raise/lower/fallback counters. Keep both peers on the same wire protocol version and selected band. Debug images may enable bench switches; production images use the normal authenticated session and ARQ.

## Host and tool checks

```shell
cmake -S tests/host -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
python3 tests/tools/run.py
tests/gates.sh
```

Host acceptance must cover two peers pairing, establishing a session, exchanging DATA in both directions, losing frames and recovering from receive backpressure. Tool acceptance must cover command encoding and update-port discovery. Tree checks must enforce layering and shipped configuration constraints.

Application lifecycle acceptance should run under native Zephyr with simulated device operations:

```shell
source tools/zephyr-env.sh
west build -b native_sim/native/64 tests/simulation/application -d build/application-simulation
./build/application-simulation/zephyr/zephyr.exe
```

Application lifecycle acceptance must cover confirmation failure and retry, rollback deadlines, recovery handoff, management during RF initialization failure, long-press consumption and pairing-save retry. Deleting a saved pairing must not cause the still-active old pairing to be written back.

## Two-board validation

Management `status` includes local and peer RSSI/LQ, configured and active TX power, effective floor, MCU die temperature, hopping position, frame period and frame/error counters. Its LR2021 section adds junction/XOSC temperatures, VBAT supply, chip version, maintenance-boundary mode/command/reset/IRQ and decoded chip errors. Chip sensors refresh at boot and on management `status`, on the RF owner while transmission is stopped; reading them does not clear errors or IRQs. RSSI zero and MCU temperature `INT16_MIN` mean unavailable; LR2021 fields have independent validity bits. Debug shell `zlrs status` reads the zbus snapshot without pausing RF; RF refreshes link facts once per second and on state changes. Chip values remain the latest boot/management sample, with its age printed. Use the shell for continuous link-quality observation: entering management mode pauses the local RF service and can lower the peer's LQ.

Flash the signed LF or HF image appropriate for both radios. Set the same phrase, the opposite TX/RX roles and the same DATA band. Pair both devices, then confirm `status` reports `connected` and an active session on each. Run:

```shell
python3 tests/hardware/app/link/test_data.py --tx-port <TX-serial> --rx-port <RX-serial> --bytes 8192 --rounds 2
```

The test sends deterministic bytes in both directions and checks exact delivery. Repeat after setting `set-band subg` on both devices and rebooting. On sub-G, allow for regional duty limiting. For dynamic power, use a debug image and a configured ceiling above the floor; inspect `power` on both devices after several seconds of a stable link, then interrupt peer feedback and verify fallback to the ceiling. RSSI/LQ thresholds need calibration against measured packet-error curves.

## Hardware and recovery

The board definition is [here](../boards/zlrs/rp2350_zero_core2021/README.md). Hardware acceptance is grouped into protocol, application and radio fixtures. When an application does not boot, BOOTSEL installs MCUboot and its recovery CDC accepts a signed application image. `python3 tools/zlrs.py logs --serial <serial>` exports the persistent log ring from a running image. See [firmware behaviour](07-firmware.md) for update verification and rollback.

An application fault fixture checks that the real USB management and recovery paths remain available when RF initialization fails:

```shell
source tools/zephyr-env.sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf tests/hardware/application -d build/application-fault-sysbuild
python3 tests/hardware/application/run.py --serial <serial> --log build/application-fault.log
```

Fault acceptance must cover RF initialization failure while management and recovery
remain reachable. Fixtures must stay unconfirmed, restore the previous firmware
and verify its hash. DATA acceptance uses a paired product image; confirmation
and rollback deadlines require a separate normal upgrade scenario.

Settings acceptance must cover initialization, remounts, invalid records, writes,
identity, pairing and factory reset. Hardware scenarios that modify NVS must first
snapshot the entire partition and restore it after the scenario.

UI acceptance must cover LED priority and cadence, the 500 ms management delay,
one-second drop display, output retry, short/long presses, cumulative input and
recovery from channel lock failure. Use the hardware tick rate, including the 50 ms
connected pulse:

~~~sh
west build -b native_sim/native/64 tests/simulation/ui -d build/ui-simulation
build/ui-simulation/zephyr/zephyr.exe
~~~

Management acceptance must cover every command and malformed input on native flash
and USB hardware. Inject settings-write and confirmation failure, require the
expected error, and independently verify restoration. See
[management acceptance](../tests/hardware/mgmt/README.md).

RF acceptance must use two P1/ARQ protocol nodes with simulated radio, clock
and serial edges. Run both roles on each band. Required scenarios include bidirectional bytes,
completion failures, preparation/launch failures, absent IRQs, timeout cleanup,
strong-link power reductions, feedback-loss fallback and diagnostic pagination:

```sh
for role in OFF ON; do
  for subg in OFF ON; do
    build_dir="build/rf-${role}-${subg}"
    west build -b native_sim/native/64 tests/simulation/rf -d "$build_dir" -- -DRF_TEST_ROLE_RX="$role" -DRF_TEST_BAND_SUBG="$subg"
    "$build_dir/zephyr/zephyr.exe"
  done
done
```

Protocol acceptance requires a fixed version 10 wire transcript,
tampering, reflection, duplicate DATA, session renewal, LQ window-boundary loss and
prepared-ACK follow-up checks. FIFO preparation must respect the timing policy: a prepared
ACK snapshot can lag, but new DATA leaves an ACK pending until a later frame carries it.
Independent byte vectors must cover all eight typed bodies and exact lengths. DATA tests
must cover inline byte ownership, P1 499-byte/P0 503-byte maximum payloads and four 7-bit sequence wraps
with lost reverse ACKs and receiver backpressure at wrap boundaries.

Radio acceptance must exercise FIFO tails independently of USB pacing:

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf tests/hardware/radio -d build/radio
python3 tests/hardware/radio/run.py --serial <TX-serial> --peer <RX-serial> --log build/radio.json --timeout 60
```

The fixture uses the production preparation window; only frequency hopping is disabled for the fixed-channel measurement. An extra configuration with `CONFIG_ZLRS_APP_FHSS=y` enables the normal hopping plan for comparison. The runner reads radio and link reports through management before and after transfer, and records nonpausing status during each round and at its end. A timeout retains byte counters and radio/link reports, including when the session cannot become ready. A transfer timeout also records running image hashes. The unconfirmed fixture allows five minutes for peer upgrade and bounded transfer rounds; the runner requests rollback and verifies both original hashes.

`test_radio channels` reports cumulative 2.4 GHz results in the 79 current DATA channel bins: completed TX, failed TX, modem-valid RX packets, empty/failed RX windows and CRC errors. The runner records these bins before and after DATA rounds. RX counts precede authentication; empty windows also include peer silence, so they do not measure packet error rate. Opposite-board TX/RX deltas help locate losses by frequency, with observation-edge and management-pause differences at the report boundaries. Sub-G operation leaves these 2.4 GHz bins empty.

For raw radio testing, build in a separate directory with `-Dradio_EXTRA_CONF_FILE="$PWD/tests/hardware/radio/raw.conf"` and pass that signed image with `--image ... --raw`. Select the 2.4 GHz DATA band before testing; the raw fixture uses the configured DATA modulation and paired sync word. `ZLRS_TEST_RAW_FREQ_MHZ` selects the fixed frequency (2441 MHz by default), and `ZLRS_TEST_RAW_PAYLOAD_BYTES` selects a sequence-numbered payload of 6–511 bytes (32 by default). `ZLRS_TEST_RAW_REVERSE` swaps the transmitting and receiving boards without changing saved roles. The report includes successful and failed TX attempts, valid RX packets, expected packets between the first and last valid sequence, damaged payloads, frequency and payload size. `quality_percent` measures reception inside that observed sequence span; startup and tail packets outside the span are excluded. Runner success verifies valid delivery and restoration, and does not assert zero packet loss or zero failed TX attempts.

RAW starts the timer's launch notification before testing, so a long packet's FIFO tail is filled immediately after SET_TX, as in normal RF operation. It stops this test clock before the application starts its superframe. The runner also saves the post-test radio report on delivery failures, including FIFO underflows and launch timing.
