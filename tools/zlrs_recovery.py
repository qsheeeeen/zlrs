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
#   python3 tools/zlrs.py recovery --port /dev/ttyACM1   # management port (second CDC in app mode)
#   python3 tests/tools/run.py --module zlrs_recovery   # no device: check recovery framing
#
# Then upload a signed image with an mcumgr client:
#   smpmgr --port <recovery port> upgrade zephyr.signed.bin
#
# The ports change: two CDCs in application mode (management port is ttyACM1), two in bootloader
# mode (recovery first, console second). Find recovery by /dev/serial/by-id/...MCUBOOT...-if00.

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
