#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Put the device into MCUboot serial recovery: no replugging, no reset button.
#
# How: the tool sends the recovery command (management protocol 0x41) on the management port; the
# device sets the boot-mode flag and reboots into MCUboot recovery (doc/09-security.md 9).
#
# Usage:
#   python3 tools/zlrs.py recovery --port /dev/ttyACM2   # management port (third CDC in app mode)
#   python3 tools/zlrs.py --self-test           # no device: check the framing
#
# Then upload a signed image with an mcumgr client:
#   smpmgr --port <recovery port> upgrade zephyr.signed.bin
#
# The ports change: three CDCs in application mode (management port is ttyACM2), two in bootloader
# mode (recovery first, console second). Find recovery by /dev/serial/by-id/...MCUBOOT...-if00.

from pathlib import Path
import re
import sys

SOF = b"\xa5\x5a"
VERSION = 3
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
    want = bytes.fromhex("a55a034100e4")

    header = Path(__file__).resolve().parent.parent / "include/zlrs/mgmt/mgmt.h"
    match = re.search(r"#define\s+ZLRS_MGMT_VERSION\s+(\d+)U", header.read_text())
    if match is None or int(match[1]) != VERSION:
        print("self-test: FAIL recovery protocol version differs from firmware", file=sys.stderr)
        return 1

    if request != want:
        print(f"self-test: FAIL wrong framing: {request.hex()} != {want.hex()}", file=sys.stderr)
        return 1
    if request[3] != CMD_RECOVERY:
        print("self-test: FAIL wrong command id", file=sys.stderr)
        return 1

    print("self-test: ok (recovery frame matches the C side)")
    return 0
