# Testing system

Tests are organized by execution environment. Shared behavior assertions belong
in `contracts/`; reusable implementations and data belong in `support/`.
An environment must not compile sources from another environment. `gates.sh`
enforces this boundary.

| Location | Responsibility | Real code | Replaced boundary | Executor |
| --- | --- | --- | --- | --- |
| `host/` | Protocol vectors, state transitions, delivery invariants and pure policies | `core/`, band and power policies | SHA backend, deterministic radio/clock/entropy | CTest, ASan/UBSan |
| `tools/` | Host commands, wire decoding, update decisions and device selection | Python device tools | Ports, subprocesses and device responses | Python unittest through CTest |
| `simulation/` | Zephyr component integration and application lifecycle | Application owners, threads, zbus, NVS, UART emulation and protocol peers | Physical radio, flash, GPIO and external devices | Twister on `native_sim/native/64` |
| `hardware/` | Peripheral behavior, physical timing, RF/data transfer and firmware rollback | Target firmware and peripherals | Only the boundary named by each fixture | Explicit board-specific runners |
| `contracts/` | Identical behavior required on native and hardware backends | Persistence and UI components | Backend selected by the including test application | Included by simulation/hardware CMake |
| `support/` | Software SHA backend and deterministic DATA patterns | Test implementations only | No scenario ownership | Linked by consumers |

Host link tests use two real protocol peers. Native RF tests use the real RF
owner and protocol peers behind simulated radio/clock/serial edges. Keeping
those levels separate makes a protocol failure distinguishable from an owner
or Zephyr integration failure. Passing either level does not establish RF
performance or hardware timing.

## Fast checks

From the repository root, without a Zephyr workspace or connected devices:

```sh
cmake -S tests -B build/tests
cmake --build build/tests --parallel
ctest --test-dir build/tests --parallel 4 --output-on-failure --output-junit results.xml
```

This runs architecture gates, every Python tool module and every host scenario.
Each host scenario has its own executable, static state, timeout and CTest
result. Production code and the link fixture are compiled once. Zero executed
checks is a failure. ASan and UBSan are enabled by default; CI additionally
enables LeakSanitizer with `-DZLRS_HOST_LEAKS=ON`.

List or select tests using CTest:

```sh
ctest --test-dir build/tests -N
ctest --test-dir build/tests -L link --output-on-failure
ctest --test-dir build/tests -R '^host.link.pairing_context$' --output-on-failure
ctest --test-dir build/tests --rerun-failed --output-on-failure
```

Labels include `host`, `proto`, `link`, `policy`, `fuzz`, `tools` and
`architecture`. The host-only CMake entry at `tests/host` remains available.
Python tool modules are discovered from `tools/test_*.py` and run in separate
processes; they must not depend on another module's import side effects.

`fuzz.malformed_inputs` is deterministic malformed-input stress, with a fixed
seed and `ZLRS_HOST_FUZZ_ITERS` configurable in CMake. It checks parser robustness;
successful protocol transitions and recovery need named stateful scenarios.
The number of random inputs is not a functional coverage claim.

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
contains application lifecycle, persistence, management, UI, three serial
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
The separate firmware workflow builds signed target images.

## Hardware acceptance

Hardware runners stay explicit because their inputs include board serial
identities, signed fixture images, physical connections and a log destination.
They are not part of CTest discovery or the native test root.

| Fixture | Responsibility |
| --- | --- |
| `hardware/unit` | Target component image: entropy, SHA, link, persistence, slot timer, UI and log acceptance; original firmware rollback |
| `hardware/application` | Application boot, confirmation, watchdog and recovery acceptance |
| `hardware/mgmt` | Real CDC management, settings and firmware identity; see its README |
| `hardware/radio` | Two-board RF/DATA transfer, diagnostic capture and isolated RAW checks |

Run a fixture's `run.py --help` for its required inputs. Build, wiring and
acceptance procedures are in [Development and acceptance](../doc/10-debug.md).
Save full logs and image hashes with the verdict. Hardware runners must report
restoration/rollback failures separately from the fixture's test verdict.

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

Register host scenarios in `host/CMakeLists.txt` and native configurations in
their `testcase.yaml`. Add a named regression to the owning level when fixing a
defect. Fake only the component's external boundary; an owner integration test
must keep the real components whose interaction it claims to verify.
