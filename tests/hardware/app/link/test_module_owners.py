#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Two-board acceptance: shared configuration frontends and stalled DATA output."""

import argparse
import json
import subprocess
import sys
import threading
import time
from pathlib import Path

import serial

from test_crsf_device import Rig, port_for
from test_crsf_state import reboot, wait_remote

REPO = Path(__file__).resolve().parents[4]


def manage(identity, action, *values, rejected=False):
    result = subprocess.run(
        [sys.executable, str(REPO / "tools/zlrs.py"), action, *map(str, values),
         "--serial", identity], capture_output=True, text=True, timeout=15,
    )
    assert (result.returncode != 0) == rejected, result.stdout + result.stderr
    if rejected:
        assert "bad parameter" in (result.stdout + result.stderr).lower()
    return result.stdout


def value(rig, number, wide=False):
    definition, _ = rig.parameter(0xEE, number)
    start = definition.index(0, 2) + 1
    if not wide:
        start = definition.index(0, start) + 1  # Selection options precede the current value.
    return int.from_bytes(definition[start:start + (4 if wide else 1)], "big", signed=wide)


def rejected_write(rig, number, candidate, wide=False):
    before, _ = rig.parameter(0xEE, number)
    encoded = candidate.to_bytes(4, "big", signed=True) if wide else bytes([candidate])
    rig.clear()
    rig.tx.send(0x2D, bytes([0xEE, 0xEA, number]) + encoded)
    response = rig.expect(rig.tx, 0x2B, lambda p: p[:3] == b"\xea\xee" + bytes([number]))
    assert response[4:] == before


def shared_configuration(args):
    rig = Rig(args)
    try:
        manage(args.tx_serial, "set-power", -10)
        assert value(rig, 2, True) == -10
        for power in (-11, 99):
            manage(args.tx_serial, "set-power", power, rejected=True)
            rejected_write(rig, 2, power, True)
        manage(args.tx_serial, "set-region", 0, rejected=True)
        rejected_write(rig, 3, 0)

        # Validate against the saved combination even before it becomes live.
        manage(args.tx_serial, "set-region", 2)
        assert value(rig, 3) == 2
        manage(args.tx_serial, "set-power", 17)
        assert value(rig, 2, True) == 17
        manage(args.tx_serial, "set-region", 3, rejected=True)
        rejected_write(rig, 3, 3)
        rig.write(0xEE, 2, -10, wide=True)
        assert "-10 dBm" in manage(args.tx_serial, "status")
        rig.write(0xEE, 3, 3)
        assert "CN433" in manage(args.tx_serial, "status")

        rig.write(0xEE, 8, 1)
        assert value(rig, 8) == 1
        assert "mixed" in manage(args.tx_serial, "status")
        manage(args.tx_serial, "set-mode", "mixed")
        assert value(rig, 8) == 0
        assert rig.tx.bad_crc + rig.rx.bad_crc == 0
        return {"shared_saved_values": True, "same_invalid_value_rejection": True,
                "pending_region_power_validation": True, "mode_waits_for_reboot": True}
    finally:
        # No reboot while the intentionally different RF combination is saved.
        try:
            manage(args.tx_serial, "set-power", -10)
            manage(args.tx_serial, "set-region", 3)
            manage(args.tx_serial, "set-mode", "mixed")
        finally:
            rig.tx.close()
            rig.rx.close()


def stalled_output(args):
    results = []
    for source, sink in ((args.tx_serial, args.rx_serial), (args.rx_serial, args.tx_serial)):
        print(json.dumps({"stage": "stalled_output", "source": source, "sink": sink,
                          "bytes": args.bytes}), flush=True)
        expected = bytes((i * 37 + (i >> 8) + 11) & 255 for i in range(args.bytes))
        errors = []
        with serial.Serial(port_for(source, 2), 115200, timeout=0, write_timeout=120) as tx, \
                serial.Serial(port_for(sink, 2), 115200, timeout=0, write_timeout=120) as rx:
            tx.reset_input_buffer()
            rx.reset_input_buffer()

            def write():
                try:
                    assert tx.write(expected) == len(expected)
                except Exception as exc:
                    errors.append(str(exc))

            start = time.monotonic()
            writer = threading.Thread(target=write)
            writer.start()
            time.sleep(3)  # Exceed the endpoint ring without draining the receive port.
            received = bytearray()
            deadline = start + 120
            while len(received) < len(expected) and time.monotonic() < deadline and not errors:
                received.extend(rx.read(rx.in_waiting))
                time.sleep(0.002)
            writer.join(timeout=max(0, deadline - time.monotonic()))
            assert not writer.is_alive() and not errors, errors
            assert bytes(received) == expected, f"{source}->{sink}: {len(received)}/{len(expected)}"
            time.sleep(0.5)
            assert rx.in_waiting == 0, "duplicate bytes after output resumed"
            results.append({"source": source, "sink": sink, "bytes": len(received),
                            "seconds": round(time.monotonic() - start, 3), "exact": True})
    return results


def peer_restart(args):
    rig = Rig(args)
    results = []
    probe = argparse.Namespace(**vars(args), data_bytes=512)
    try:
        for destination in (0xEE, 0xEC):
            rig = reboot(rig, args, destination)
            wait = wait_remote(rig)
            if not args.data_only:
                rig.rc = bytes(range(22))
            print(json.dumps({"stage": "peer_restart", "rebooted":
                              "tx" if destination == 0xEE else "rx",
                              "discovery_wait_s": wait}), flush=True)
            rig.duplex(probe)
            results.append({"rebooted": "tx" if destination == 0xEE else "rx",
                            "discovery_wait_s": wait,
                            **rig.results["concurrent_duplex"]})
        return results
    finally:
        rig.tx.close()
        rig.rx.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tx-serial", required=True)
    parser.add_argument("--rx-serial", required=True)
    parser.add_argument("--bytes", type=int, default=65536)
    parser.add_argument("--data-only", action="store_true")
    args = parser.parse_args()
    result = {}
    if not args.data_only:
        result["configuration"] = shared_configuration(args)
        print(json.dumps({"stage": "configuration", **result["configuration"]}), flush=True)
    result["peer_restart"] = peer_restart(args)
    print(json.dumps({"stage": "peer_restart_complete", **result}), flush=True)
    result["stalled_output"] = stalled_output(args)
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
