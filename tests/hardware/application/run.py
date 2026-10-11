#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify management on an RF initialization fault, then restore the previous firmware."""

import argparse
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import zlrs_update as update


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--image", type=Path,
                        default=ROOT / "build/application-fault-sysbuild/application/zephyr/zephyr.signed.bin")
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    args.log.write_text("")

    def run(command, check=True):
        result = subprocess.run(command, text=True, capture_output=True, timeout=180)
        output = result.stdout + result.stderr
        print(output, end="", flush=True)
        with args.log.open("a") as log:
            log.write(output)
        if check:
            result.check_returncode()
        return result

    cli = [sys.executable, str(ROOT / "tools/zlrs.py")]
    old_hash = update.parse_hex_hash(run(cli + ["image-hash", "--serial", args.serial]).stdout)
    if old_hash is None:
        raise RuntimeError("could not read original image hash")

    try:
        run(cli + ["update", "--serial", args.serial, "--image", str(args.image),
                   "--no-confirm-check", "--timeout", "45"])
        # The image uses the real application and USB management code, with only radio
        # initialization and the confirmed-image verdict replaced by test doubles.
        run(cli + ["status", "--serial", args.serial])
        run(cli + ["build", "--serial", args.serial])
    finally:
        management = update.wait_for_by_id("ZLRS", 30, exclude="MCUBOOT",
                                          serial=args.serial, exact_serial=True)
        if management is None:
            raise RuntimeError("test management port unavailable for restoration")
        run(cli + ["recovery", "--port", management])
        recovery = update.wait_for_by_id("MCUBOOT", 30, serial=args.serial, exact_serial=True)
        if recovery is None:
            raise RuntimeError("test firmware did not enter recovery")
        # USB/IP may expose the CDC node before the first SMP exchange can complete.
        for attempt in range(3):
            result = run([update.find_client(), "--port", recovery, "--baudrate", "115200",
                          "os", "reset"], check=False)
            if result.returncode == 0 or not Path(recovery).exists():
                break
            time.sleep(1)
        else:
            result.check_returncode()
        management = update.wait_for_by_id("ZLRS", 45, exclude="MCUBOOT",
                                          serial=args.serial, exact_serial=True)
        if management is None:
            raise RuntimeError("original firmware did not return")
        rc, output = update.run_mgmt_subcmd_retry(management, "image-hash")
        if rc != 0 or update.parse_hex_hash(output) != old_hash:
            raise RuntimeError("original firmware hash was not restored")

    message = f"RF fault management verified; original firmware {old_hash} restored.\n"
    print(message, end="")
    with args.log.open("a") as log:
        log.write(message)


if __name__ == "__main__":
    main()
