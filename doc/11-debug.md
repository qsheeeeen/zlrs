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

Settings persistence also runs on simulated flash: `west build -b native_sim/native/64 zlrs/tests/simulation/settings -d zlrs/build/settings-simulation`, then run `cd zlrs/build/settings-simulation && ./zephyr/zephyr.exe`. It verifies layout initialization, settings across remounts and complete partition clearing. The same fixture runs in the hardware unit image and resets NVS; use a reset test board and configure it again after the tests.
