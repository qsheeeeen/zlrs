#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Install an unconfirmed unit test image, capture Ztest, and verify rollback."""

import argparse
from pathlib import Path
import subprocess
import sys
import time

import serial


REPO = Path(__file__).resolve().parents[3]
CLI = REPO / "tools/zlrs.py"


def image_hash(identity):
    result = subprocess.run(
        [sys.executable, str(CLI), "image-hash", "--serial", identity],
        check=True, text=True, capture_output=True, timeout=15,
    )
    for token in result.stdout.replace("=", " ").split():
        if len(token) == 64 and all(char in "0123456789abcdef" for char in token.lower()):
            return token.lower()
    raise RuntimeError("Cannot read the running firmware hash")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="exact board USB serial")
    parser.add_argument("--image", type=Path,
                        default=REPO / "build/unit-data-only-sysbuild/unit/zephyr/zephyr.signed.bin")
    parser.add_argument("--log", type=Path, required=True, help="save the complete Ztest output")
    parser.add_argument("--timeout", type=int, default=300,
                        help="test console discovery and execution timeout in seconds (default: 300)")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    args.log.write_bytes(b"")
    previous = image_hash(args.serial)
    management = Path("/dev/serial/by-id") / f"usb-zlrs_ZLRS_{args.serial}-if02"
    subprocess.run(
        [sys.executable, str(CLI), "update", "--serial", args.serial,
         "--image", str(args.image), "--no-wait", "--timeout", "45"],
        check=True,
    )
    console = Path("/dev/serial/by-id") / f"usb-zlrs_ZLRS_UNIT_TEST_{args.serial}-if04"
    deadline = time.monotonic() + args.timeout
    while not console.exists():
        if management.exists():
            time.sleep(0.5)
            if image_hash(args.serial) == previous:
                raise RuntimeError("Test firmware rolled back before its console started")
        if time.monotonic() >= deadline:
            raise TimeoutError("Test console did not enumerate")
        time.sleep(0.1)

    output = bytearray()
    with serial.Serial(str(console), 115200, timeout=0.1) as port:
        while time.monotonic() < deadline:
            try:
                chunk = port.read(4096)
            except (serial.SerialException, OSError):
                break  # Test completion reboots the unconfirmed image.
            output.extend(chunk)
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                sys.stdout.flush()
            if not console.exists():
                break
    args.log.write_bytes(output)

    deadline = time.monotonic() + 45
    while not management.exists():
        if time.monotonic() >= deadline:
            raise TimeoutError("Previous firmware did not return after the test")
        time.sleep(0.1)
    time.sleep(0.5)
    if image_hash(args.serial) != previous:
        raise RuntimeError("MCUboot did not restore the previous firmware")
    if b"PROJECT EXECUTION SUCCESSFUL" not in output:
        raise RuntimeError(f"Hardware tests failed or output was incomplete; see {args.log}")
    print(f"Tests passed; original firmware {previous} restored. Log: {args.log}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        print(f"Hardware test error: {error}", file=sys.stderr)
        sys.exit(1)
