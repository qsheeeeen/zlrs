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

The host link test simulates two peers pairing, establishing a session, exchanging DATA in both directions, losing frames and recovering from receive backpressure. The tool tests cover command encoding and update-port discovery. The tree gate checks layering and shipped configurations.

The application loop and boot policy also run under native Zephyr with simulated device operations:

```shell
source tools/zephyr-env.sh
west build -b native_sim/native/64 tests/simulation/application -d build/application-simulation
./build/application-simulation/zephyr/zephyr.exe
```

This executes the real `main.c` and `boot.c`. It checks confirmation failure and retry, rollback deadlines, recovery handoff, management during RF initialization failure, long-press consumption and pairing-save retry. Deleting a saved pairing must not cause the still-active old pairing to be written back.

## Two-board validation

Management `status` includes local and peer RSSI/LQ, configured and active TX power, effective floor, MCU die temperature, hopping position, frame period and frame/error counters. RSSI zero and temperature `INT16_MIN` mean unavailable. Debug shell `zlrs status` reads the zbus snapshot without pausing RF; RF refreshes this snapshot once per second and on state changes. Use the shell for continuous link-quality observation: entering management mode pauses the local RF service and can lower the peer's LQ.

Flash the signed LF or HF image appropriate for both radios. Set the same phrase, the opposite TX/RX roles and the same DATA band. Pair both devices, then confirm `status` reports `connected` and an active session on each. Run:

```shell
python3 tests/hardware/app/link/test_data.py --tx-port <TX-serial> --rx-port <RX-serial> --bytes 8192 --rounds 2
```

The test sends deterministic bytes in both directions and checks exact delivery. Repeat after setting `set-band subg` on both devices and rebooting. On sub-G, allow for regional duty limiting. For dynamic power, use a debug image and a configured ceiling above the floor; inspect `power` on both devices after several seconds of a stable link, then interrupt peer feedback and verify fallback to the ceiling. RSSI/LQ thresholds need calibration against measured packet-error curves.

## Hardware and recovery

The board definition is [here](../boards/zlrs/rp2350_zero_core2021/README.md). `tests/hardware/core` exercises core code under Zephyr; `tests/hardware/app` exercises application owners; `tests/hardware/radio` provides radio-level fixtures. When an application does not boot, BOOTSEL installs MCUboot and its recovery CDC accepts a signed application image. `python3 tools/zlrs.py logs --serial <serial>` exports the persistent log ring from a running image. See [firmware behaviour](07-firmware.md) for update verification and rollback.

An application fault fixture checks that the real USB management and recovery paths remain available when RF initialization fails:

```shell
source tools/zephyr-env.sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf tests/hardware/application -d build/application-fault-sysbuild
python3 tests/hardware/application/run.py --serial <serial> --log build/application-fault.log
```

Only LR2021 initialization and the application's confirmed-image verdict are replaced by test doubles. The fixture stays unconfirmed in flash; the runner reads management status and build information, enters recovery and resets to restore the previous firmware, then verifies its hash. Do not confirm this fixture. Use a normally paired application image for DATA validation and a normal unconfirmed upgrade to validate the actual confirmation timeout.

Settings persistence also runs on simulated flash: `west build -b native_sim/native/64 zlrs/tests/simulation/settings -d zlrs/build/settings-simulation`, then run `cd zlrs/build/settings-simulation && ./zephyr/zephyr.exe`. It verifies layout initialization, settings across remounts and complete partition clearing. The same fixture runs in the hardware unit image; it backs up and restores the complete NVS partition.

UI simulation runs the real UI thread with an emulated GPIO button, real zbus and a test LED driver that records RGB output. Its 1 kHz kernel tick matches the hardware unit image, including the 50 ms connected pulse:

```sh
west build -b native_sim/native/64 tests/simulation/ui -d build/ui-simulation
build/ui-simulation/zephyr/zephyr.exe
```

It covers LED priority/cadence, management-display delay, drop-display expiry, LED write retry, short and long holds, cumulative requests, and recovery after a channel write-lock failure. The hardware unit image runs the same LED-output checks on the real UI thread; the test LED replaces the physical strip only in these fixtures.

The persist fixture backs up the entire settings partition and restores it in suite teardown. It exercises the original record bytes, enum validation, identity and pairing, factory reset, and an injected NVS write error. Native runs can use `--flash_in_ram` to avoid leaving a flash file.

Management commands run with the real app command switch and real NVS in `tests/simulation/mgmt` (native simulator, `--flash_in_ram`). The hardware counterpart `tests/hardware/mgmt` uses the real app, USB, RF and flash. Its fixture snapshots NVS before main; recovery restores that snapshot and MCUboot rolls back the unconfirmed fixture. Never confirm or ship this fixture. Confirmation and one NVS write are explicitly fault-injected; the normal upgrade/confirmation path is separately verified on product images.

RF result simulation runs real RF scheduling and two real P1/ARQ protocol nodes,
with only LR2021, clock and serial edges simulated. Run both roles on each band.
It covers bidirectional bytes,
completion failures, preparation/launch failures, absent IRQs, timeout cleanup,
strong-link power reductions, feedback-loss fallback and diagnostic pagination:

```sh
for role in OFF ON; do
  for subg in OFF ON; do
    build_dir="build/rf-${role}-${subg}"
    west build -b native_sim/native/64 tests/simulation/rf -d "$build_dir" -- -DRF_TEST_ROLE_RX="$role" -DRF_TEST_BAND_SUBG="$subg"
    "$build_dir/zephyr/zephyr.exe" --flash_in_ram
  done
done
```

The host link regression includes a fixed version 8 wire transcript,
tampering, reflection, duplicate DATA, session renewal, LQ window-boundary loss and
prepared-ACK follow-up checks. It keeps the FIFO preparation timing policy: a prepared
ACK snapshot can lag, but new DATA leaves an ACK pending until a later frame carries it.

The radio fixture fills DATA packets directly, exercising FIFO tails independently of USB pacing:

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf tests/hardware/radio -d build/radio
python3 tests/hardware/radio/run.py --serial <TX-serial> --peer <RX-serial> --log build/radio.json --timeout 60
```

The fixture uses the production preparation window; only frequency hopping is disabled for the fixed-channel measurement. An extra configuration with `CONFIG_ZLRS_APP_FHSS=y` enables the normal hopping plan for comparison. The runner reads radio and link reports through management before and after transfer, and records nonpausing status during each round and at its end. A timeout retains byte counters and radio/link reports, including when the session cannot become ready. A transfer timeout also records running image hashes. The unconfirmed fixture allows five minutes for peer upgrade and bounded transfer rounds; the runner requests rollback and verifies both original hashes.

`test_radio channels` reports cumulative 2.4 GHz results in the 79 current DATA channel bins: completed TX, failed TX, modem-valid RX packets, empty/failed RX windows and CRC errors. The runner records these bins before and after DATA rounds. RX counts precede authentication; empty windows also include peer silence, so they do not measure packet error rate. Opposite-board TX/RX deltas help locate losses by frequency, with observation-edge and management-pause differences at the report boundaries. Sub-G operation leaves these 2.4 GHz bins empty.

For raw radio testing, build in a separate directory with `-Dradio_EXTRA_CONF_FILE="$PWD/tests/hardware/radio/raw.conf"` and pass that signed image with `--image ... --raw`. Select the 2.4 GHz DATA band before testing; the raw fixture uses the configured DATA modulation and paired sync word. `ZLRS_TEST_RAW_FREQ_MHZ` selects the fixed frequency (2441 MHz by default), and `ZLRS_TEST_RAW_PAYLOAD_BYTES` selects a sequence-numbered payload of 6–511 bytes (32 by default). `ZLRS_TEST_RAW_REVERSE` swaps the transmitting and receiving boards without changing saved roles. The report includes successful and failed TX attempts, valid RX packets, expected packets between the first and last valid sequence, damaged payloads, frequency and payload size. `quality_percent` measures reception inside that observed sequence span; startup and tail packets outside the span are excluded. Runner success verifies valid delivery and restoration, and does not assert zero packet loss or zero failed TX attempts.

RAW starts the timer's launch notification before testing, so a long packet's FIFO tail is filled immediately after SET_TX, as in normal RF operation. It stops this test clock before the application starts its superframe. The runner also saves the post-test radio report on delivery failures, including FIFO underflows and launch timing.
