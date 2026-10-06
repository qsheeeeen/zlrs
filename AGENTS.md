# Repository Guidelines

## Project Structure & Module Organization

zlrs is a Zephyr firmware project carrying CRSF control and byte-stream data over an LR2021 radio.

- `core/` and `include/zlrs/`: portable C protocol, link, CRSF, management, region, and LED modules. Keep these independent of Zephyr.
- `app/src/`: application entry point and platform, link, management, state, UI, and storage layers. Application code binds directly to Zephyr/RP2350; do not introduce a ports abstraction.
- `boards/`, `dts/`, `drivers/`, `subsys/`: board definitions, bindings, entropy driver, USB, and boot support.
- `tests/host/`: hardware-independent tests and two-node simulation.
- `tools/`: workspace setup, management, upgrade, and validation scripts.
- `doc/`: numbered design documents; track unimplemented behavior and hardware acceptance in `doc/12-roadmap.md`. Keep upstream module source unchanged; implement integration changes inside zlrs using public APIs and existing Kconfig options.

## Build, Test, and Development Commands

Run from the repository root:

```sh
tools/setup-zephyr.sh          # Install the west workspace and dependencies
source tools/zephyr-env.sh     # Select SDK, workspace, and board
zlrs-build                    # Build application plus MCUboot with sysbuild
zlrs-rebuild-app               # Rebuild app in an existing sysbuild tree
cmake -S tests/host -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure # Run host tests with ASan and UBSan
tests/gates.sh                # Check architecture and configuration rules
```

Set `ZLRS_BOARD=rp2350_zero_core2021/rp2350a/m33/lf` for the LF target; HF is default. Run `python3 tests/tools/run.py` for management, recovery, and update checks.

## Coding Style & Naming Conventions

Follow `.editorconfig` and `.clang-format`: C uses tabs, eight-column indentation, a 100-column limit, and Linux-style braces. Python/shell use four spaces; YAML uses two. Use `snake_case`, `zlrs_` prefixes for public APIs, and `ZLRS_` constants.

Check C formatting with:

```sh
git ls-files '*.c' '*.h' | xargs clang-format --dry-run --Werror
```

## Testing Guidelines

Use the custom C harness in `tests/host/test.h`; organize cases in `test_<module>.c`. Add regression cases for changed behavior, especially loss, replay, timing, malformed input, and backpressure. Host crypto links the production `core/proto/crypto.c` with `sha256_soft.c`; it does not exercise the hardware SHA driver. Report firmware builds and hardware validation separately from simulation results.

## Commit & Pull Request Guidelines

Follow existing subjects: `feat(app): ...`, `refactor(config): ...`, `build(usp_zephyr): ...`, or `docs: ...`. Keep commits focused. PRs should explain the problem, resulting behavior, affected boards/configurations, and validation performed; link relevant issues and identify hardware checks still pending.

## Configuration & Firmware Updates

Describe hardware through devicetree and deliberate feature cuts through Kconfig. Keep debug switches out of shipped configurations. Install applications through MCUboot; BOOTSEL receives only the bootloader UF2.
