# Contributing

Thanks for looking. zlrs is a point-to-point link: one LR2021 radio, two logical
links (control and data), Zephyr only. The design is written down first —
`docs/design/` is the specification, code follows it. Read
[docs/design/organization.md](docs/design/organization.md) before changing a
directory layout or a hardware abstraction.

## Build

The workspace is a west T2 workspace: this repository is the manifest
repository.

```shell
scripts/setup-zephyr.sh          # once; pulls ~5 GB of Zephyr and its modules
source scripts/zephyr-env.sh     # once per shell
zlrs-build                       # plain build, HF SKU by default
ZLRS_BOARD=rp2350_zero_core2021/rp2350a/m33/lf zlrs-build -d build-lf
zlrs-build-mcuboot               # application plus MCUboot (sysbuild)
```

Zephyr SDK 1.0.0 is required; a distribution `arm-none-eabi-gcc` trips a static
assertion in the vendor radio stack.

## Test

Everything below runs without hardware except the last item.

```shell
make -C tests/host run           # pure C core: unit tests and a two-node simulation
make -C tests/host run LEAKS=1   # adds LeakSanitizer (needs ptrace)
```

`.github/workflows/firmware.yml` is the build matrix: both SKUs, the bench
switch combinations, the single-link cuts and the MCUboot images. Run it locally
before pushing if you touched Kconfig, devicetree or CMake:

```shell
for sku in hf lf; do
  B=rp2350_zero_core2021/rp2350a/m33/$sku
  west build -p always -b "$B" app -d "build-$sku"
  west build -p always -b "$B" app -d "build-$sku-noradio" -- -DCONFIG_ZLRS_APP_RADIO=n
  west build -p always --sysbuild -b "$B/mcuboot" app -d "build-$sku-mcuboot"
done
```

With a board attached:

```shell
python3 scripts/zlrs-bringup.py            # USB, config mode, config protocol, NVS
python3 scripts/zlrs-update.py --image <signed.bin>   # A/B update with rollback
```

## Style

- C: `.clang-format` (Zephyr's style). `git ls-files '*.c' '*.h' | xargs clang-format -i`
  before committing; CI runs the same command with `--dry-run --Werror`.
- Comments explain *why*, never what the code already says.
- English everywhere: code, comments, Kconfig help, commit messages, docs.
- `core/` must not include Zephyr headers. That boundary is what keeps the
  protocol testable on a host, and CI enforces it.
- Vendor code (`usp_zephyr`, `modules/lib/usp`) is a west module: never patch it
  in this repository, report or work around upstream.

## Commits and pull requests

- One topic per commit, staged so each commit builds and passes the host tests.
- The commit message says what changed and why; the diff says how.
- A pull request that changes behaviour should say which test proves it. If it
  cannot be tested here, say what was checked by hand and on which hardware.

## Adding hardware

A new hardware variant is *MCU board plus radio chip plus radio design plus
ports*, described once in devicetree under `boards/<vendor>/<board>/`. The
software discovers what exists through aliases — see
[docs/design/organization.md](docs/design/organization.md) section 2 — so a new
variant normally needs no C changes at all. Copy an existing board directory,
adjust pins and aliases, and add the variant to the build matrix.

## Reporting problems

Ordinary bugs: open an issue with the board, the exact commands and what came
out (the console log is enough — the status line prints the counters that
matter). Security problems: see [SECURITY.md](SECURITY.md).
