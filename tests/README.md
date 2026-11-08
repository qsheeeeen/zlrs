# Testing system

Tests follow the source tree: `tools/` tests host scripts, `app/` tests firmware,
and `drivers/` tests hardware drivers. Within `app/`, each module owns its test
sources and shared assertions. Native fixtures live with the module; physical
fixtures and their runners live in its `hardware/` subdirectory.

| Location | Responsibility | Executor |
| --- | --- | --- |
| `tools/` | Commands, wire decoding, update decisions and device selection | Python unittest |
| `app/proto`, `app/link`, `app/region`, `app/band`, `app/power`, `app/lifecycle` | Protocol vectors, paired-link scenarios and policy | Aggregate Zephyr application in `app/unit` |
| `app/persist`, `app/mgmt`, `app/rf`, `app/serial`, `app/ui`, `app/main`, `app/trace` | Component integration and lifecycle | Twister on `native_sim/native/64` |
| `app/*/hardware` | Peripheral behavior, physical timing, RF/DATA and rollback | Explicit board runners |
| `app/unit/hardware` | Target component acceptance, including driver tests | Zephyr Ztest on the board |
| `app/support` | Software SHA, deterministic DATA, shared bootstrap and helpers | Linked by test applications |
| `app/link/bench` | Protocol and DATA cost measurements | Zephyr native application, JSON measurements |

All C/C++ firmware, protocol scenarios and benchmarks build through Zephyr.
Board fixtures share MCUboot configuration through `app/support/sysbuild.cmake`.
`app/support/peer.h` generates peer frames and drives production link operations;
production link code exposes no test-only bulk-write, combined-slot or RX-injection
entry points. DATA vectors and fuzz cases verify borrowed payload boundaries.
Application protocol sources and their private headers live together under
`app/src/link`, `app/src/proto` and `app/src/region` and compile into the `app`
target. Native protocol tests use two real link instances with controlled radio,
time, entropy and sinks. RF scenarios additionally run the real RF owner. These
fixtures replace physical boundaries while keeping production application code.
Neither native level establishes hardware timing or over-the-air performance.

The state and failure contracts include:

| Contract | Scenarios |
| --- | --- |
| Pairing publishes a confirmed record | Both roles; candidate received then cancel, timeout or confirm; delayed abandoned responses; reconnect to the saved peer |
| TX retains packet-owned bytes until commit | Both roles, ARQ/best effort, failed completions, in-place retries, immutable TX attempts, direct admission and packet-slot wrap |
| Reliable DATA matches a reference byte stream | P0/P1 and three fixed seeds; duplex writes, packet loss, duplicate frames, bad P1 tags, TX failure, backpressure, queue and clock wrap; eventual drain |
| Destructive reset retires active credentials | Failure before/after clear, remount failure and layout-marker failure; RF refresh despite command failure; recovery through initialization |
| Saved state matches completed writes | Independent byte records, failed key/pair writes, partial settings saves and identity-write failure after pair deletion; remount checks |
| Pairing-save retries preserve RF service | One-to-30-second failure backoff, uptime wrap, replaced/retired candidates, and no rewrite of a committed record; hardware EIO injection during duplex DATA |

The stream test reports its seed, step and generator state on failure. Its oracle
is the accepted application byte stream, independent of packet encoding or ARQ
implementation. Malformed-input fuzzing remains a separate parser/memory check.

## Tool and tree checks

Python device-tool checks and source ownership gates need no target hardware:

```sh
python -m pip install -r tools/requirements.txt
bash tests/gates.sh
python tests/tools/run.py
```

Tool modules run independently and test command validation, wire decoding,
transport selection and update decisions. Gates check application ownership,
board independence, debug isolation and firmware/storage constraints. Protocol
modules may use Zephyr APIs directly.

## Zephyr native checks

Set up the workspace as described in the main README, then run from the
repository root:

```sh
source tools/zephyr-env.sh
python -m pip install -r tests/app/requirements.txt
export ZEPHYR_TOOLCHAIN_VARIANT=host
unset ZEPHYR_SDK_INSTALL_DIR
python "$ZEPHYR_BASE/scripts/twister" \
  -T tests/app -p native_sim/native/64 --exclude-tag benchmark \
  --outdir build/simulation --jobs 2 --inline-logs
```

Native tests use the host compiler. Clearing the SDK hint allows this command
to run without an ARM toolchain installation. The C and C++ native compilers
must belong to the same toolchain; add `-x TOOLCHAIN_VARIANT_COMPILER=llvm`
to select Clang.

The `testcase.yaml` files are the sole configuration matrix. CI runs this same
command and does not enumerate roles, bands or endpoints separately. The matrix
contains protocol vectors, paired-link and policy scenarios, application lifecycle,
persistence, management, UI, five serial
endpoint configurations, and six RF configurations: both roles in fixed 2.4 GHz,
fixed SubG and automatic modes. Twister gives every configuration a separate
build and working directory, including its simulated flash file.

Select one configuration or rerun failures with Twister:

```sh
python "$ZEPHYR_BASE/scripts/twister" \
  -T tests/app -p native_sim/native/64 --exclude-tag benchmark \
  --outdir build/simulation -s zlrs.rf.auto_follower --inline-logs
python "$ZEPHYR_BASE/scripts/twister" \
  -T tests/app -p native_sim/native/64 --exclude-tag benchmark \
  --outdir build/simulation --only-failed --inline-logs
```

Reports live in `twister.json`, `twister.xml` and `twister_report.xml` under the
output directory. Each configuration retains `build.log`, `handler.log` and
`handler_stderr.log` for build and runtime diagnosis. The tools job installs ripgrep explicitly and uploads gate and unittest logs.
The tool and simulation jobs in
`.github/workflows/build.yml` fail on test errors and upload their reports.
The separate firmware workflow runs on PRs and main pushes, builds signed target
images and fails on build errors or timed-launch RAM audit failures. Its artifacts
include linked maps and placement reports.

RF completion, entropy recovery, pairing outcomes, power recovery, diagnostics
and publication have separate cases. Each creates its own peers and resets the
fake edges. Storage cases start from a fresh partition; the hardware suite saves
the original partition once and restores it after all cases. To audit order
independence, select `-T tests/app/rf -T tests/app/persist
-T tests/app/mgmt` and add `-x CONFIG_ZTEST_SHUFFLE=y` (Zephyr also
repeats cases and suites in this mode). Application boot and asynchronous UI
acceptance use their normal single-boot execution. Protocol scenarios register
role, profile, backpressure and fault-seed combinations as separate Ztest cases.
Each case resets its state and reports the fault seed and step on stream-oracle
failure. Select `-T tests/app/unit` to run that fixture alone.

Protocol checks enable AddressSanitizer and UBSan through Zephyr Kconfig. Use
`zassert_*` for required preconditions and `zexpect_*` for independent observations.
Malformed-input loops use fixed seeds and `CONFIG_ZLRS_TEST_FUZZ_ITERATIONS`
inputs per scenario, exercising maximum and overlong frames;
their iteration count is not a functional coverage claim.

## Performance measurements

The benchmark is a Zephyr native application, selected separately from the
sanitized functional matrix:

```sh
source tools/zephyr-env.sh
python "$ZEPHYR_BASE/scripts/twister" \
  -T tests/app/link/bench -p native_sim/native/64 \
  --outdir build/bench --inline-logs
```

`CONFIG_ZLRS_BENCH_ITERATIONS` selects 100..1000000 iterations (default 20000).
The handler log contains JSON with compiler version, profile and seven samples
per case: median/min/max nanoseconds for in-place DATA header encoding, P1
authentication, read-only selection, finalization and completion at
0/32/128/499 payload bytes. The fixture verifies that selection changes no
sequence or construction counter, checks finalized packets against reference
bytes and checks completion counters before accepting a measurement.

Compare release builds with the same compiler, CPU and load; retain both JSON
files and commits. CI retains measurements without noisy wall-clock thresholds.
These measurements use software SHA on the host. Board SHA, SPI, deadline jitter
and over-the-air throughput require the hardware fixtures below.

## Hardware acceptance

Hardware runners stay explicit because their inputs include board serial
identities, signed fixture images, physical connections and a log destination.
Their platform constraints exclude them from native execution.

| Fixture | Responsibility |
| --- | --- |
| `app/unit/hardware` | Target component image: entropy, SHA, link, persistence, slot timer, UI and log acceptance; original firmware rollback |
| `app/serial/hardware` | Production UART queues with real CTS, RTS pause/resume, hardware loopback and short TX restarts |
| `app/main/hardware` | Application boot, confirmation, watchdog and recovery acceptance |
| `app/mgmt/hardware` | Real CDC management, settings and firmware identity; see its README |
| `app/rf/hardware` | Two-board RF/DATA transfer, diagnostic capture and isolated RAW checks |

Run a fixture's `run.py --help` for its required inputs. Build, wiring and
acceptance procedures are in [Development and acceptance](../doc/10-debug.md).
The unit timer suite includes a loaded alarm-latency measurement. It requires a
board run; compiling the image or passing native tests does not establish its
result.
Save full logs and image hashes with the verdict. Hardware runners must report
restoration/rollback failures separately from the fixture's test verdict.

The UART fixture uses the shared `app/support/ztest_main.c` watchdog/rollback
bootstrap and the unit runner. It forces the real CTS input for the blocked-TX
case and uses UART hardware loopback for DATA and RTS/CTS; external cable wiring
requires a separate check. Build and run it without changing saved settings:

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33_0/lf \
  tests/app/serial/hardware -d build/serial-hardware -- \
  -DSB_APPLICATION_CONFIG_DIR="$PWD/app"
python tests/app/unit/hardware/run.py --serial <serial> \
  --image build/serial-hardware/hardware/zephyr/zephyr.signed.bin \
  --log build/serial-hardware/results.log
```

## Adding a test

Put the test under the production module it exercises. Choose the smallest
fixture that can observe the behavior: native Zephyr for vectors, state and
service interactions; the module's `hardware/` fixture for peripherals and timing.
Keep assertions shared by native and board tests in that same module. Add a new
top-level test directory only for another production source owner.

Name a scenario after the invariant it establishes. Set up its own initial
state, name the injected fault, observe the public result and verify recovery.
Use fatal precondition checks before dereferencing required pointers or using
decoded results. A scenario must be runnable alone and in any execution order.
Keep long workflows together only when their sequence is the behavior under
test, such as reboot and rollback.

For changes to pairing, sessions, queues, persistence or adaptive radio state,
cover the success path, rejected transition, injected failure and later
recovery. Check that rejected input leaves committed state and queued user DATA
unchanged. Test both roles and relevant configuration variants. Do not replace
these observations with assertion counts, fuzz iterations or a wire transcript
hash alone.

Add named `ZTEST` scenarios and Zephyr value-parameterized tests to the owning
fixture, list source files individually in its CMake file, and add configurations
to `testcase.yaml`. Keep each parameter combination individually registered so a
failure identifies its role, profile and fault. Fake only the component's
external boundary; an integration test keeps the real application modules whose
interaction it claims to verify.
