#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Put the device into MCUboot serial recovery: no replugging, no reset button.
#
# How: the tool sends the recovery command (management protocol 0x41) on the management port; the
# device sets the boot-mode flag and reboots into MCUboot recovery (doc/10-security.md 9).
#
# Usage:
#   python3 tools/zlrs-recovery.py --port /dev/ttyACM2   # management port (third CDC in app mode)
#   python3 tools/zlrs-recovery.py --self-test           # no device: check the framing
#
# Then upload a signed image with an mcumgr client:
#   smpmgr --port <recovery port> upgrade zephyr.signed.bin
#
# The ports change: three CDCs in application mode (management port is ttyACM2), two in bootloader
# mode (recovery first, console second). Find recovery by /dev/serial/by-id/...MCUBOOT...-if00.

import argparse
import sys

SOF = b"\xa5\x5a"
VERSION = 2
CMD_RECOVERY = 0x41
DEFAULT_BAUD = 115200


def crc8(data: bytes) -> int:
    """DVB-S2 polynomial 0xD5, the same algorithm as zlrs_crc8() on the device."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def build_request() -> bytes:
    """The management protocol's recovery frame: SOF + ver + cmd + len + crc."""
    body = bytes([VERSION, CMD_RECOVERY, 0])

    return SOF + body + bytes([crc8(body)])


def self_test() -> int:
    request = build_request()
    want = bytes.fromhex("a55a02410067")

    if request != want:
        print(f"self-test: FAIL wrong framing: {request.hex()} != {want.hex()}", file=sys.stderr)
        return 1
    if request[3] != CMD_RECOVERY:
        print("self-test: FAIL wrong command id", file=sys.stderr)
        return 1

    print("self-test: ok (recovery frame matches the C side)")
    return 0


def send_recovery(port: str, baud: int, dry_run: bool) -> int:
    request = build_request()

    if dry_run:
        print(f"[dry-run] would write {request.hex()} to {port}@{baud}")
        return 0

    try:
        import serial  # pyserial
    except ImportError:
        print("pyserial required: pip install pyserial", file=sys.stderr)
        return 2

    try:
        with serial.Serial(port, baudrate=baud, timeout=1) as link:
            link.write(request)
            link.flush()
    except Exception as exc:  # noqa: BLE001
        print(f"writing to {port} failed: {exc}", file=sys.stderr)
        return 1

    print(f"recovery command sent to {port}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="put the device into MCUboot serial recovery")
    parser.add_argument("--port", help="management port (third CDC in application mode, e.g. /dev/ttyACM2)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--dry-run", action="store_true", help="print the frame instead of sending it")
    parser.add_argument("--self-test", action="store_true", help="check the framing without a device")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.port:
        parser.error("talking to a device needs --port")

    return send_recovery(args.port, args.baud, args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
