#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Wrap the **signed image** from an MCUboot build into a UF2 that can be dropped on BOOTSEL.
#
# Why this exists: Zephyr's own BUILD_OUTPUT_UF2 wraps the **unsigned** zephyr.bin at the
# flash base, and both are wrong for an MCUboot application image (MCUboot rejects unsigned
# images, and the image belongs in slot0). So this wraps zephyr.signed.bin into its own
# zephyr.signed.uf2 based at slot0 (flash base plus the code partition offset).
#
# Usage:
#   west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/hf/mcuboot app
#   python3 tools/make-signed-uf2.py            # reads $ZLRS_WS/build by default
#   python3 tools/make-signed-uf2.py <build-dir>
#
# First flash (no debugger):
#   1. hold BOOTSEL while plugging USB in, drop <build>/mcuboot/zephyr/zephyr.uf2 on RPI-RP2
#   2. then drop the <build>/<app>/zephyr/zephyr.signed.uf2 this script produces on RPI-RP2
# Later upgrades go through MCUboot serial recovery, see the README and doc/08-firmware.md.

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def find_zephyr_base() -> Path:
    """Where the Zephyr tree is: $ZEPHYR_BASE first, otherwise inferred from the west workspace."""
    env = os.environ.get("ZEPHYR_BASE")
    if env:
        return Path(env)

    here = Path(__file__).resolve().parent
    for parent in (here, *here.parents):
        candidate = parent / "zephyr" / "scripts" / "build" / "uf2conv.py"
        if candidate.is_file():
            return parent / "zephyr"

    raise SystemExit("no Zephyr tree: source tools/zephyr-env.sh or set ZEPHYR_BASE")


def read_kconfig(config_path: Path) -> dict:
    values = {}
    for line in config_path.read_text().splitlines():
        match = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line.strip())
        if match:
            values[match.group(1)] = match.group(2).strip('"')
    return values


def find_signed_image(build_dir: Path) -> Path:
    """The application's zephyr.signed.bin, excluding the bootloader's own copy."""
    candidates = [
        path
        for path in build_dir.glob("*/zephyr/zephyr.signed.bin")
        if path.parent.parent.name != "mcuboot"
    ]
    if not candidates:
        raise SystemExit(
            f"no application zephyr.signed.bin under {build_dir}: "
            "build one first with `west build --sysbuild -b <board>/mcuboot app`"
        )
    if len(candidates) > 1:
        raise SystemExit(
            "several candidates found: " + ", ".join(str(path) for path in candidates) +
            " (pass a specific directory, or remove the stale build directories)"
        )
    return candidates[0]


def main() -> int:
    default_build = Path(os.environ.get("ZLRS_WS", ".")) / "build"
    parser = argparse.ArgumentParser(description="wrap a signed image into a BOOTSEL-droppable UF2")
    parser.add_argument("build_dir", nargs="?", default=str(default_build))
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    if not build_dir.is_dir():
        raise SystemExit(f"build directory does not exist: {build_dir}")

    signed_bin = find_signed_image(build_dir)
    app_config = read_kconfig(signed_bin.parent / ".config")

    try:
        flash_base = int(app_config["CONFIG_FLASH_BASE_ADDRESS"], 0)
        load_offset = int(app_config.get("CONFIG_FLASH_LOAD_OFFSET", "0"), 0)
        family = app_config["CONFIG_BUILD_OUTPUT_UF2_FAMILY_ID"]
    except KeyError as missing:
        raise SystemExit(f"the build config lacks {missing}: this does not look like an MCUboot build")

    base_addr = flash_base + load_offset
    out_path = signed_bin.with_suffix(".uf2")
    uf2conv = find_zephyr_base() / "scripts" / "build" / "uf2conv.py"

    cmd = [
        sys.executable,
        str(uf2conv),
        "-c",
        "-f",
        family,
        "-b",
        hex(base_addr),
        "-o",
        str(out_path),
        str(signed_bin),
    ]
    subprocess.run(cmd, check=True)

    print(f"app image : {signed_bin}")
    print(f"base      : {base_addr:#x} (flash base {flash_base:#x} + partition offset {load_offset:#x})")
    print(f"UF2      : {out_path}")
    print("note      : the zephyr.uf2 in the same directory is unsigned; MCUboot rejects it")

    return 0


if __name__ == "__main__":
    sys.exit(main())
