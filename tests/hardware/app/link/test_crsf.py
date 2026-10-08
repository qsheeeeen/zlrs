#!/usr/bin/env python3
"""Verify CRSF forwarding, telemetry and cut failsafe on two mixed-mode boards."""

import argparse
import glob
import json
import time

import serial


def crc8(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 255 if crc & 128 else (crc << 1) & 255
    return crc


def port_for(identity):
    ports = glob.glob(f"/dev/serial/by-id/*{identity}*-if00")
    if len(ports) != 1:
        raise ValueError(f"expected one CRSF CDC for {identity}, found {ports}")
    return ports[0]


def frame(payload):
    body = bytes([0x16]) + payload
    return bytes([0xC8, len(body) + 1]) + body + bytes([crc8(body)])


class Capture:
    def __init__(self, port):
        self.port = port
        self.buffer = bytearray()
        self.bad_crc = 0
        self.bad_packets = []

    def read(self):
        self.buffer.extend(self.port.read(self.port.in_waiting))
        frames = []
        while len(self.buffer) >= 2:
            if self.buffer[0] not in (0xC8, 0xEA) or not 2 <= self.buffer[1] <= 62:
                del self.buffer[0]
                continue
            size = self.buffer[1] + 2
            if len(self.buffer) < size:
                break
            packet = bytes(self.buffer[:size])
            del self.buffer[:size]
            if crc8(packet[2:-1]) != packet[-1]:
                self.bad_crc += 1
                if len(self.bad_packets) < 5:
                    self.bad_packets.append(packet.hex())
                continue
            frames.append((packet[0], packet[2], packet[3:-1]))
        return frames


def capture(tx, rx, seconds, payload=None):
    tx_frames, rx_frames = [], []
    deadline = time.monotonic() + seconds
    next_send = time.monotonic()
    while time.monotonic() < deadline:
        now = time.monotonic()
        if payload is not None and now >= next_send:
            tx.port.write(frame(payload))
            next_send = now + 0.020
        tx_frames.extend(tx.read())
        rx_frames.extend(rx.read())
        time.sleep(0.001)
    return tx_frames, rx_frames


def rc_frames(frames):
    return [payload for addr, kind, payload in frames if addr == 0xC8 and kind == 0x16]


def run(args):
    with serial.Serial(port_for(args.tx_serial), 420000, timeout=0) as tx_port, \
            serial.Serial(port_for(args.rx_serial), 420000, timeout=0) as rx_port:
        tx, rx = Capture(tx_port), Capture(rx_port)
        capture(tx, rx, 1.0)  # Drain USB queues and allow any previous input to expire.
        startup_bad_crc = tx.bad_crc + rx.bad_crc
        startup_bad_packets = tx.bad_packets + rx.bad_packets
        tx.bad_crc = rx.bad_crc = 0
        tx.bad_packets.clear()
        rx.bad_packets.clear()
        _, idle = capture(tx, rx, 1.1)
        assert not rc_frames(idle), "RX emits RC without handset input"

        first = bytes((0x31 + i * 7) & 255 for i in range(22))
        telemetry, output = capture(tx, rx, 3.0, first)
        received = rc_frames(output)
        assert len(received) >= 10, f"too few RC frames: {len(received)}"
        assert all(payload == first for payload in received), "RC payload changed in transit"
        stats = [p for addr, kind, p in telemetry if addr == 0xEA and kind == 0x14]
        details = [p for addr, kind, p in telemetry if addr == 0xEA and kind == 0x1D]
        assert stats and all(len(p) == 10 for p in stats), "missing standard 0x14 statistics"
        assert details and all(len(p) == 8 for p in details), "missing standard 0x1D statistics"
        assert all(p[2] <= 100 and p[8] <= 100 and p[4] == 0 and p[6] <= 8 for p in stats)
        assert any(p[2] > 0 for p in stats), "handset never receives positive uplink LQ"
        assert all(p[1] <= 100 and p[2] <= 100 and p[6] == 0 and p[7] == 0 for p in details)
        assert all(p[5] > 0 for p in details), "invalid standard fps field"

        # Local timeout is 100 ms; drain the final on-air/USB frame before the silence check.
        capture(tx, rx, 0.6)
        _, stopped = capture(tx, rx, 1.2)
        assert not rc_frames(stopped), "RX keeps emitting channel values after input loss"

        second = bytes((0xE2 - i * 5) & 255 for i in range(22))
        _, resumed = capture(tx, rx, 3.0, second)
        recovered = rc_frames(resumed)
        assert len(recovered) >= 10, "RC output did not recover after restoring input"
        assert all(payload == second for payload in recovered), "stale RC after recovery"
        assert tx.bad_crc == 0 and rx.bad_crc == 0, (
            f"bad output CRSF CRC: TX {tx.bad_packets}, RX {rx.bad_packets}"
        )
        assert startup_bad_crc == 0, f"truncated startup frames: {startup_bad_packets}"
        return {
            "result": "pass", "rc_frames": len(received), "resumed_rc_frames": len(recovered),
            "idle_rc_frames": len(rc_frames(idle)), "cut_rc_frames": len(rc_frames(stopped)),
            "link_stats_frames": len(stats), "link_tx_stats_frames": len(details),
            "last_0x14": stats[-1].hex(), "last_0x1d": details[-1].hex(),
            "bad_crc": tx.bad_crc + rx.bad_crc,
            "startup_bad_crc": startup_bad_crc, "startup_bad_packets": startup_bad_packets,
        }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tx-serial", required=True)
    parser.add_argument("--rx-serial", required=True)
    print(json.dumps(run(parser.parse_args())))
