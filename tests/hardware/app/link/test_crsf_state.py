#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate CRSF persistent settings, reboot, bind/cancel and stale-input recovery on two boards."""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import serial

from test_crsf_device import Rig, command

REPO = Path(__file__).resolve().parents[4]


def manage(action, identity):
    return subprocess.run(
        [sys.executable, str(REPO / "tools/zlrs.py"), action, "--serial", identity],
        check=True, capture_output=True, text=True, timeout=15,
    ).stdout


def command_status(rig, destination, parameter, action):
    rig.clear()
    rig.tx.send(0x2D, bytes([destination, 0xEA, parameter, action]))
    payload = rig.expect(rig.tx, 0x2B, lambda p: p[:3] == bytes([0xEA, destination, parameter]))
    definition = payload[4:]
    return definition[definition.index(0, 2) + 1]


def reopen(args):
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        rig = None
        try:
            rig = Rig(args)
            rig.pump(2.0)  # USB/IP can briefly expose an instance that is still being reset.
            return rig
        except (OSError, ValueError, serial.SerialException):
            if rig is not None:
                rig.tx.close()
                rig.rx.close()
            time.sleep(0.2)
    raise AssertionError("application USB did not return after CRSF reboot")


def model_value(rig, destination):
    definition, _ = rig.parameter(destination, 5)
    start = definition.index(0, 2) + 1
    return int.from_bytes(definition[start:start + 4], "big", signed=True)


def wait_remote(rig):
    begin = time.monotonic()
    rig.tx.send(0x28, b"\xec\xea")
    rig.expect(rig.tx, 0x29, lambda p: p[:2] == b"\xea\xec", seconds=100)
    return round(time.monotonic() - begin, 3)


def write_local(rig, endpoint, destination, number, value, wide=False):
    origin = 0xEA if endpoint is rig.tx else 0xC8
    encoded = value.to_bytes(4, "big", signed=True) if wide else bytes([value])
    endpoint.send(0x2D, bytes([destination, origin, number]) + encoded)
    rig.expect(endpoint, 0x2D, lambda p: p == bytes([origin, destination, number]) + encoded)


def reboot(rig, args, destination):
    assert command_status(rig, destination, 10, 1) == 3
    assert command_status(rig, destination, 10, 4) == 2
    disconnected = False
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            rig.pump(0.1)
        except (OSError, serial.SerialException):
            disconnected = True
            break
    rig.tx.close()
    rig.rx.close()
    assert disconnected, "CRSF confirmation did not reboot the board"
    return reopen(args)


def run(args):
    rig = Rig(args)
    results = {}
    try:
        rig.write(0xEE, 5, 17, wide=True)
        rig.write(0xEC, 5, 23, wide=True)
        rig = reboot(rig, args, 0xEE)
        assert model_value(rig, 0xEE) == 17
        results["tx_reboot_discovery_wait_s"] = wait_remote(rig)
        rig = reboot(rig, args, 0xEC)
        results["rx_reboot_discovery_wait_s"] = wait_remote(rig)
        assert model_value(rig, 0xEC) == 23
        results["persistent_model_and_reboot"] = {"tx": 17, "rx": 23}
        rig.write(0xEE, 5, 0, wide=True)
        rig.write(0xEC, 5, 0, wide=True)

        assert rig.direct(0xEE, 0x10, 1) == 1
        assert rig.direct(0xEE, 0x10, 2) == 1
        wait_remote(rig)
        results["bind_cancel_preserves_peer"] = True
        assert command_status(rig, 0xEE, 4, 1) == 2
        assert command_status(rig, 0xEE, 4, 5) == 0
        wait_remote(rig)
        results["bind_menu_start_cancel"] = True

        # An unbound/available receiver enters bind from its local CRSF port.
        rig.rx.send(0x32, command(0xEC, 0x10, 1, origin=0xC8))
        rig.expect(rig.rx, 0x32, lambda p: p[:6] == b"\xc8\xec\xff\x10\x01\x01")
        assert rig.direct(0xEE, 0x10, 1) == 1
        rig.pump(1)
        wait_remote(rig)
        results["crsf_pair_and_discovery"] = True

        rig.rc = bytes(range(22))
        rig.expect(rig.rx, 0x16, lambda p: p == rig.rc)
        manage("enter", args.tx_serial)
        try:
            rig.pump(0.15)  # new samples during the frozen radio session
            rig.rc = None
            rig.pump(0.35)
        finally:
            manage("exit", args.tx_serial)
        rig.pump(0.3)
        rig.clear()
        rig.pump(1.1)
        assert not any(kind == 0x16 for kind, _ in rig.rx.frames)
        rig.rc = b"\x55" * 22
        rig.expect(rig.rx, 0x16, lambda p: p == rig.rc)
        results["management_stale_rc_cut_resume"] = True

        manage("enter", args.rx_serial)
        payload = b"\xc8\xea\x30\x03\x42\x19\x87"
        try:
            rig.tx.send(0x7A, payload)
            rig.pump(1.0)
            rig.rx.frames.clear()
        finally:
            manage("exit", args.rx_serial)
        rig.expect(rig.rx, 0x7A, lambda p: p == payload, seconds=15)
        rig.pump(0.8)
        assert not any(kind == 0x7A and p == payload for kind, p in rig.rx.frames)
        results["queued_msp_survives_radio_gap_once"] = True
        results["crc_errors"] = rig.tx.bad_crc + rig.rx.bad_crc
        assert results["crc_errors"] == 0
        return results
    except Exception:
        print(json.dumps({"partial": results}, sort_keys=True), flush=True)
        raise
    finally:
        # Restore the model before leaving even if a later case fails.
        try:
            try:
                rig.pump(0.01)
            except (OSError, serial.SerialException):
                rig.tx.close()
                rig.rx.close()
                rig = reopen(args)
            write_local(rig, rig.tx, 0xEE, 5, 0, wide=True)
            write_local(rig, rig.rx, 0xEC, 6, 0)
            write_local(rig, rig.rx, 0xEC, 5, 0, wide=True)
        finally:
            rig.tx.close()
            rig.rx.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tx-serial", required=True)
    parser.add_argument("--rx-serial", required=True)
    args = parser.parse_args()
    print(json.dumps(run(args), sort_keys=True))


if __name__ == "__main__":
    main()
