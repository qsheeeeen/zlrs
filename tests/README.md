# Testing system

Tests are organized by execution environment. Shared behavior assertions belong
in `contracts/`; reusable implementations and data belong in `support/`.
An environment must not compile sources from another environment. `gates.sh`
enforces this boundary.

| Location | Responsibility | Real code | Replaced boundary | Executor |
| --- | --- | --- | --- | --- |
| `host/` | Protocol vectors, state transitions, delivery invariants and pure policies | `core/`, band and power policies | SHA backend, deterministic radio/clock/entropy | GoogleTest/CTest, ASan/UBSan |
| `tools/` | Host commands, wire decoding, update decisions and device selection | Python device tools | Ports, subprocesses and device responses | Python unittest through CTest |
| `simulation/` | Zephyr component integration and application lifecycle | Application owners, threads, zbus, NVS, UART emulation and protocol peers | Physical radio, flash, GPIO and external devices | Twister on `native_sim/native/64` |
| `hardware/` | Peripheral behavior, physical timing, RF/data transfer and firmware rollback | Target firmware and peripherals | Only the boundary named by each fixture | Explicit board-specific runners |
| `contracts/` | Identical behavior required on native and hardware backends | Persistence and UI components | Backend selected by the including test application | Included by simulation/hardware CMake |
| `support/` | Software SHA backend and deterministic DATA patterns | Test implementations only | No scenario ownership | Linked by consumers |
| `bench/` | Reproducible host cost measurements | Protocol codec, authentication and DATA paths | Software SHA and steady-state link fixture | Standalone release executable, JSON results |

Host link tests use two real protocol peers. Native RF tests use the real RF
owner and protocol peers behind simulated radio/clock/serial edges. Keeping
those levels separate makes a protocol failure distinguishable from an owner
or Zephyr integration failure. Passing either level does not establish RF
performance or hardware timing.

The state and failure contracts include:

| Contract | Scenarios |
| --- | --- |
| Pairing publishes a confirmed record | Both roles; candidate received then cancel, timeout or confirm; delayed abandoned responses; reconnect to the saved peer |
| TX borrows queued bytes until commit | Both roles, ARQ/best effort, failed completions, retry and queue wrap |
| Reliable DATA matches a reference byte stream | P0/P1 and three fixed seeds; duplex writes, packet loss, duplicate frames, bad P1 tags, TX failure, backpressure, queue and clock wrap; eventual drain |
| Destructive reset retires active credentials | Failure before/after clear, remount failure and layout-marker failure; RF refresh despite command failure; recovery through initialization |
| Saved state matches completed writes | Independent byte records, failed key/pair writes, partial settings saves and identity-write failure after pair deletion; remount checks |
| Pairing-save retries preserve RF service | One-to-30-second failure backoff, uptime wrap, replaced/retired candidates, and no rewrite of a committed record; hardware EIO injection during duplex DATA |

The stream test reports its seed, step and generator state on failure. Its oracle
is the accepted application byte stream, independent of packet encoding or ARQ
implementation. Malformed-input fuzzing remains a separate parser/memory check.

## Fast checks

From the repository root, without a Zephyr workspace or connected devices:

```sh
cmake -S tests -B build/tests
cmake --build build/tests --parallel
ctest --test-dir build/tests --parallel 4 --output-on-failure --output-junit results.xml
```

This runs architecture gates, every Python tool module and every host scenario.
GoogleTest cases are discovered individually by CTest from one executable per
module. Every selected case runs in its own process with a timeout and independent
result. Production code remains C11; GoogleTest cases use C++20. ASan and UBSan
are enabled by default; CI additionally enables LeakSanitizer with
`-DZLRS_HOST_LEAKS=ON`.

GoogleTest 1.18.0 is fetched once during CMake configuration, with a pinned
archive hash. For offline builds, prefetch its source and configure with
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/absolute/path/to/googletest-1.18.0`.

List or select tests using CTest:

```sh
ctest --test-dir build/tests -N
ctest --test-dir build/tests -L link --output-on-failure
ctest --test-dir build/tests -R '^host.link.Link.PairingContext$' --output-on-failure
ctest --test-dir build/tests --rerun-failed --output-on-failure
```

Host labels use `host.<module>`, including `host.proto`, `host.link`, `host.policy`
and `host.fuzz`. CTest label filters are regular expressions: `-L host` selects
all host cases and `-L link` selects the link module. The other labels are `tools`
and `architecture`. The host-only CMake entry at `tests/host` remains available.
Python tool modules are discovered from `tools/test_*.py` and run in separate
processes; they must not depend on another module's import side effects.

The `MalformedInput` cases are deterministic stress tests, with separate fixed
seeds and `ZLRS_HOST_FUZZ_ITERS` configurable in CMake. Inputs reach the maximum
frame length and one byte beyond it; control-frame shapes use current protocol
lengths. These cases check parser robustness;
successful protocol transitions and recovery need named stateful scenarios.
The number of random inputs is not a functional coverage claim.

Link fixtures own both nodes, fake time, packet-loss controls, receive sinks and
wire captures. `SetUp()` resets all of them before every case. Profile and loss
variants use named `TEST_P` parameters. The link binary also supports repeated,
shuffled execution to check for order-dependent state:

```sh
build/tests/host/link_tests --gtest_shuffle --gtest_repeat=3 --gtest_random_seed=2026
```

Use `ASSERT_*` for setup and decoding preconditions, `EXPECT_*` for independent
observations, and `SCOPED_TRACE` for loop context. Existing C callbacks and radio
fakes remain the protocol boundary; GoogleMock is not a dependency.

CTest writes `results.xml` and `Testing/Temporary/LastTest.log` under the build
directory. Use `--output-on-failure` for the failing scenario's assertions.

## Zephyr integration checks

Set up the workspace as described in the main README, then run from the
repository root:

```sh
source tools/zephyr-env.sh
python -m pip install -r tests/simulation/requirements.txt
export ZEPHYR_TOOLCHAIN_VARIANT=host
unset ZEPHYR_SDK_INSTALL_DIR
python "$ZEPHYR_BASE/scripts/twister" \
  -T tests/simulation -p native_sim/native/64 \
  --outdir build/simulation --jobs 2 --inline-logs
```

Native tests use the host compiler. Clearing the SDK hint allows this command
to run without an ARM toolchain installation.

The `testcase.yaml` files are the sole configuration matrix. CI runs this same
command and does not enumerate roles, bands or endpoints separately. The matrix
contains application lifecycle, persistence, management, UI, five serial
endpoint configurations, and six RF configurations: both roles in fixed 2.4 GHz,
fixed SubG and automatic modes. Twister gives every configuration a separate
build and working directory, including its simulated flash file.

Select one configuration or rerun failures with Twister:

```sh
python "$ZEPHYR_BASE/scripts/twister" \
  -T tests/simulation -p native_sim/native/64 \
  --outdir build/simulation -s zlrs.rf.auto_follower --inline-logs
python "$ZEPHYR_BASE/scripts/twister" \
  -T tests/simulation -p native_sim/native/64 \
  --outdir build/simulation --only-failed --inline-logs
```

Reports live in `twister.json`, `twister.xml` and `twister_report.xml` under the
output directory. Each configuration retains `build.log`, `handler.log` and
`handler_stderr.log` for build and runtime diagnosis. Both test jobs in
`.github/workflows/build.yml` fail on test errors and upload their reports.
The separate firmware workflow runs on PRs and main pushes, builds signed target
images and fails on build errors or timed-launch RAM audit failures. Its artifacts
include linked maps and placement reports.

RF completion, entropy recovery, pairing outcomes, power recovery, diagnostics
and publication have separate cases. Each creates its own peers and resets the
fake edges. Storage cases start from a fresh partition; the hardware suite saves
the original partition once and restores it after all cases. To audit order
independence, select `-T tests/simulation/rf -T tests/simulation/settings
-T tests/simulation/mgmt` and add `-x CONFIG_ZTEST_SHUFFLE=y` (Zephyr also
repeats cases and suites in this mode). Application boot and asynchronous UI
acceptance use their normal single-boot execution. Host link cases can be audited in one
process with `link_tests --gtest_shuffle --gtest_repeat=3 --gtest_random_seed=2026`.

## Performance measurements

Build benchmarks separately so sanitizer instrumentation and GoogleTest do not
affect the measurements:

```sh
cmake -S tests/bench -B build/bench -DCMAKE_BUILD_TYPE=Release
cmake --build build/bench --parallel
build/bench/link_bench > build/bench/link.json
```

The optional positional argument selects 100..1000000 iterations (default 20000).
Results include compiler version and seven samples per case, with median/min/max
nanoseconds for encoding, P1 authentication, prepared DATA and TX completion at
0/32/128/499 payload bytes. Completion replays a prepared request to isolate its
bookkeeping cost. Prepare measurements exclude empty packets. The fixture checks
packet sizes and completion counters before accepting a measurement.

Compare release builds with the same compiler, CPU and load; retain both JSON
files and commits. CI retains measurements without noisy wall-clock thresholds.
These measurements use software SHA on the host. Board SHA, SPI, deadline jitter
and over-the-air throughput require the hardware fixtures below.

## Hardware acceptance

Hardware runners stay explicit because their inputs include board serial
identities, signed fixture images, physical connections and a log destination.
They are not part of CTest discovery or the native test root.

| Fixture | Responsibility |
| --- | --- |
| `hardware/unit` | Target component image: entropy, SHA, link, persistence, slot timer, UI and log acceptance; original firmware rollback |
| `hardware/serial` | Production UART queues with real CTS, RTS pause/resume, hardware loopback and short TX restarts |
| `hardware/application` | Application boot, confirmation, watchdog and recovery acceptance |
| `hardware/mgmt` | Real CDC management, settings and firmware identity; see its README |
| `hardware/radio` | Two-board RF/DATA transfer, diagnostic capture and isolated RAW checks |

Run a fixture's `run.py --help` for its required inputs. Build, wiring and
acceptance procedures are in [Development and acceptance](../doc/10-debug.md).
The unit timer suite includes a loaded alarm-latency measurement. It requires a
board run; compiling the image or passing native tests does not establish its
result.
Save full logs and image hashes with the verdict. Hardware runners must report
restoration/rollback failures separately from the fixture's test verdict.

The UART fixture uses the shared `hardware/ztest_main.c` watchdog/rollback
bootstrap and the unit runner. It forces the real CTS input for the blocked-TX
case and uses UART hardware loopback for DATA and RTS/CTS; external cable wiring
requires a separate check. Build and run it without changing saved settings:

```sh
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/lf \
  tests/hardware/serial -d build/serial-hardware -- \
  -DSB_APPLICATION_CONFIG_DIR="$PWD/app"
python tests/hardware/unit/run.py --serial <serial> \
  --image build/serial-hardware/serial/zephyr/zephyr.signed.bin \
  --log build/serial-hardware/results.log
```

## Adding a test

Choose the lowest environment that can observe the behavior. Pure calculations
and protocol state belong in `host/`; Zephyr ownership and interactions belong
in `simulation/`; peripheral and timing claims require `hardware/`. Put a shared
contract in `contracts/` only when multiple backends run the same assertions.

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

Add host `TEST`, `TEST_F` or `TEST_P` cases to the owning module; discovery is
automatic. Add a new source file to that module in `host/CMakeLists.txt` and native
configurations to their `testcase.yaml`. Add a named regression to the owning level when fixing a
defect. Fake only the component's external boundary; an owner integration test
must keep the real components whose interaction it claims to verify.
