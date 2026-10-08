#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real two-board CRSF service through USB CDC; not handset/FC pins."""

import argparse
import glob
import json
import struct
import time

import serial


def crc8(data, polynomial=0xD5):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ (polynomial if crc & 128 else 0)) & 255
    return crc


def frame(kind, payload, address=0xC8):
    body = bytes([kind]) + bytes(payload)
    return bytes([address, len(body) + 1]) + body + bytes([crc8(body)])


def command(destination, group, subcommand, data=b"", origin=0xEA):
    body = bytes([destination, origin, group, subcommand]) + data
    return body + bytes([crc8(bytes([0x32]) + body, 0xBA)])


def port_for(identity, interface):
    paths = glob.glob(f"/dev/serial/by-id/*{identity}*-if{interface:02d}")
    if len(paths) != 1:
        raise ValueError(f"expected one port for {identity} if{interface:02d}: {paths}")
    return paths[0]


class Endpoint:
    def __init__(self, identity):
        self.port = serial.Serial(port_for(identity, 0), 420000, timeout=0)
        self.buffer = bytearray()
        self.frames = []
        self.bad_crc = 0

    def close(self):
        self.port.close()

    def send(self, kind, payload, address=0xC8):
        self.port.write(frame(kind, payload, address))

    def read(self):
        self.buffer.extend(self.port.read(self.port.in_waiting))
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
                continue
            self.frames.append((packet[2], packet[3:-1]))


class Rig:
    def __init__(self, args):
        self.tx = Endpoint(args.tx_serial)
        try:
            self.rx = Endpoint(args.rx_serial)
        except Exception:
            self.tx.close()
            raise
        self.rc = None
        self.next_rc = 0
        self.next_ping = 0
        self.results = {}

    def pump(self, seconds=0.005):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            now = time.monotonic()
            if self.rc is not None and now >= self.next_rc:
                self.tx.send(0x16, self.rc, 0xEE)  # EdgeTX module sync/address
                self.next_rc = now + 0.020
            if now >= self.next_ping:
                # Keep receive-only USB selected even with USB/IP hosts lacking DTR.
                self.rx.send(0x28, b"\xec\xc8")
                self.next_ping = now + 0.300
            self.tx.read()
            self.rx.read()
            time.sleep(0.001)

    def clear(self):
        self.pump(0.020)
        self.tx.frames.clear()
        self.rx.frames.clear()

    def expect(self, endpoint, kind, predicate, seconds=5):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump()
            for index, (typ, payload) in enumerate(endpoint.frames):
                if typ == kind and predicate(payload):
                    del endpoint.frames[index]
                    return payload
        raise AssertionError(f"missing CRSF {kind:#x}; recent {endpoint.frames[-12:]}")

    def parameter(self, device, number):
        data = bytearray()
        for chunk in range(8):
            self.clear()
            self.tx.send(0x2C, bytes([device, 0xEA, number, chunk]))
            payload = self.expect(self.tx, 0x2B, lambda p: p[:3] == bytes([0xEA, device, number]))
            data.extend(payload[4:])
            if payload[3] == 0:
                return bytes(data), chunk + 1
        raise AssertionError("parameter has too many chunks")

    def write(self, device, number, value, wide=False):
        self.clear()
        encoded = struct.pack(">i", value) if wide else bytes([value])
        self.tx.send(0x2D, bytes([device, 0xEA, number]) + encoded)
        response = self.expect(self.tx, 0x2D, lambda p: p == bytes([0xEA, device, number]) + encoded)
        return response

    def direct(self, device, group, subcommand, data=b""):
        self.clear()
        self.tx.send(0x32, command(device, group, subcommand, data))
        response = self.expect(self.tx, 0x32, lambda p: p[:5] == bytes([0xEA, device, 0xFF, group, subcommand]))
        assert crc8(bytes([0x32]) + response[:-1], 0xBA) == response[-1]
        return response[5]

    def run(self, args):
        self.pump(1.2)
        self.clear()
        self.tx.send(0x28, b"\xee\xea")
        local = self.expect(self.tx, 0x29, lambda p: p[:2] == b"\xea\xee")
        assert local[2:].startswith(b"ZLRS TX\x00")
        self.tx.send(0x28, b"\xec\xea")
        remote = self.expect(self.tx, 0x29, lambda p: p[:2] == b"\xea\xec", seconds=100)
        assert remote[2:].startswith(b"ZLRS RX\x00")
        self.clear()
        self.tx.send(0x28, b"\x00\xea")
        self.expect(self.tx, 0x29, lambda p: p[:2] == b"\xea\xee")
        self.expect(self.tx, 0x29, lambda p: p[:2] == b"\xea\xec")
        self.results["discovery"] = {"local": local.hex(), "remote": remote.hex(), "broadcast": True}

        chunks = {}
        for number in range(local[-2] + 1):
            data, count = self.parameter(0xEE, number)
            assert len(data) >= 2
            chunks[number] = count
        assert chunks[9] >= 2, "hardware parameter chunking was not exercised"
        self.results["parameter_chunks"] = chunks
        # Rejected power and SKU/region writes return unchanged definitions.
        power, _ = self.parameter(0xEE, 2)
        self.clear()
        self.tx.send(0x2D, b"\xee\xea\x02" + struct.pack(">i", 99))
        rejected = self.expect(self.tx, 0x2B, lambda p: p[:3] == b"\xea\xee\x02")
        assert rejected[4:] == power
        region, _ = self.parameter(0xEE, 3)
        self.clear()
        self.tx.send(0x2D, b"\xee\xea\x03\x00")  # FCC915 cannot use these LF boards
        rejected = self.expect(self.tx, 0x2B, lambda p: p[:3] == b"\xea\xee\x03")
        assert rejected[4:] == region
        self.results["invalid_parameter_rejection"] = True

        assert self.direct(0xEE, 0x10, 0x05, b"\x00") == 1
        self.tx.send(0x32, command(0xEE, 0x10, 0x06))
        selected = self.expect(self.tx, 0x32, lambda p: p[:5] == b"\xea\xee\x10\x07\x00")
        assert crc8(bytes([0x32]) + selected[:-1], 0xBA) == selected[-1]
        self.clear()
        bad = bytearray(command(0xEE, 0x10, 0x05, b"\x07"))
        bad[-1] ^= 1
        self.tx.send(0x32, bad)
        self.pump(0.4)
        assert not any(typ == 0x32 for typ, _ in self.tx.frames)
        self.results["command_crc_model_query"] = True
        self.clear()
        self.tx.send(0x32, command(0xEE, 0x0A, 0x70, b"\x00" + struct.pack(">I", 1000000)))
        speed = self.expect(self.tx, 0x32, lambda p: p[:4] == b"\xea\xee\x0a\x71")
        assert speed[4:6] == b"\x00\x00", "USB cannot negotiate a physical UART baud"
        assert crc8(bytes([0x32]) + speed[:-1], 0xBA) == speed[-1]
        self.results["usb_speed_rejected"] = True

        self.rc = bytes((0x31 + i * 7) & 255 for i in range(22)) + b"\x02"
        self.clear()
        self.pump(1.2)
        channels = [p for typ, p in self.rx.frames if typ == 0x16]
        assert len(channels) >= 10 and all(p == self.rc for p in channels)
        for kind, length in ((0x14, 10), (0x1C, 7), (0x1D, 8)):
            report = self.expect(self.tx, kind, lambda p: len(p) == length)
            self.results[f"stats_{kind:02x}"] = report.hex()
        assert bytes.fromhex(self.results["stats_14"])[5] in (0, 1, 2)
        assert 0 <= bytes.fromhex(self.results["stats_1c"])[4] <= 10
        assert bytes.fromhex(self.results["stats_1d"])[4] == 10
        fc_stats = self.expect(self.rx, 0x14, lambda p: len(p) == 10 and p[2] > 0)
        timing = self.expect(
            self.tx, 0x3A,
            lambda p: len(p) == 11 and p[:3] == b"\xea\xee\x10" and p[7:11] != b"\0" * 4,
        )
        assert int.from_bytes(timing[3:7], "big") > 0
        self.results["rc_extended_frames"] = len(channels)
        self.results["fc_stats"] = fc_stats.hex()
        self.results["crsf_shot"] = timing.hex()

        opaque = [
            (0x08, bytes.fromhex("00a000640001f45a")),
            (0x02, bytes(range(15))),
            (0x1E, bytes.fromhex("000100020003")),
            (0x21, b"ACRO\x00"),
            (0x88, b"\xea\xc8" + bytes(range(58))),
        ]
        for kind, payload in opaque:
            self.clear()
            self.rx.send(kind, payload)
            self.expect(self.tx, kind, lambda p, expected=payload: p == expected)
        self.results["telemetry_forwarding"] = [f"{kind:02x}" for kind, _ in opaque]
        for kind, endpoint, destination, origin in (
            (0x7A, self.tx, 0xC8, 0xEA), (0x7B, self.rx, 0xEA, 0xC8),
            (0x7C, self.tx, 0xC8, 0xEA),
        ):
            payload = bytes([destination, origin, 0x30, 0x02, 0x01, 0x00, 0xA5])
            self.clear()
            endpoint.send(kind, payload)
            self.expect(self.rx if endpoint == self.tx else self.tx, kind, lambda p: p == payload)
        self.results["msp_forwarding"] = ["7a", "7b", "7c"]

        # Enable receiver model matching, prove wrong-model cut and correct-model delivery.
        self.write(0xEC, 5, 77, wide=True)
        self.write(0xEC, 6, 1)
        try:
            self.pump(0.4)
            self.clear()
            self.pump(0.4)
            assert not any(typ == 0x16 for typ, _ in self.rx.frames)
            assert self.direct(0xEE, 0x10, 0x05, b"\x4d") == 1
            self.expect(self.rx, 0x16, lambda p: p == self.rc)
            self.results["model_match"] = True
        finally:
            self.write(0xEC, 6, 0)
            self.write(0xEC, 5, 0, wide=True)
            self.direct(0xEE, 0x10, 0x05, b"\x00")

        # Disable external telemetry at TX without suppressing control or transport ACKs.
        assert self.direct(0xEE, 0x10, 0x0B) == 1
        self.clear()
        self.rx.send(0x08, opaque[0][1])
        self.pump(0.5)
        assert not any(typ == 0x08 for typ, _ in self.tx.frames)
        assert self.direct(0xEE, 0x10, 0x0A) == 1
        self.rx.send(0x08, opaque[0][1])
        self.expect(self.tx, 0x08, lambda p: p == opaque[0][1])
        self.results["telemetry_enable_disable"] = True

        self.rc = None
        self.pump(0.35)
        self.clear()
        self.pump(1.1)
        assert not any(typ == 0x16 for typ, _ in self.rx.frames)
        self.rc = bytes(range(22))
        self.expect(self.rx, 0x16, lambda p: p == self.rc)
        self.results["cut_resume"] = True

        self.duplex(args)
        self.results["crc_errors"] = self.tx.bad_crc + self.rx.bad_crc
        assert self.results["crc_errors"] == 0
        return self.results

    def duplex(self, args):
        expected_tx = bytes((i * 17 + 3) & 255 for i in range(args.data_bytes))
        expected_rx = bytes((i * 29 + 7) & 255 for i in range(args.data_bytes))
        with serial.Serial(port_for(args.tx_serial, 2), 115200, timeout=0) as tx_data, \
                serial.Serial(port_for(args.rx_serial, 2), 115200, timeout=0) as rx_data:
            tx_data.reset_input_buffer()
            rx_data.reset_input_buffer()
            got_tx, got_rx = bytearray(), bytearray()
            sent = 0
            next_send = 0
            start = time.monotonic()
            self.clear()
            self.rx.send(0x21, b"DATA TEST\x00")
            self.tx.send(0x7A, b"\xc8\xea\x30\x02\x01\x00")
            while time.monotonic() - start < 45:
                now = time.monotonic()
                if sent < args.data_bytes and now >= next_send:
                    tx_data.write(expected_tx[sent:sent + 32])
                    rx_data.write(expected_rx[sent:sent + 32])
                    sent += 32
                    next_send = now + 0.050
                self.pump(0.001)
                got_tx.extend(tx_data.read(tx_data.in_waiting))
                got_rx.extend(rx_data.read(rx_data.in_waiting))
                if len(got_tx) >= args.data_bytes and len(got_rx) >= args.data_bytes:
                    break
            assert bytes(got_rx) == expected_tx, f"TX->RX DATA mismatch {len(got_rx)}"
            assert bytes(got_tx) == expected_rx, f"RX->TX DATA mismatch {len(got_tx)}"
            self.expect(self.tx, 0x21, lambda p: p == b"DATA TEST\x00")
            self.expect(self.rx, 0x7A, lambda p: p == b"\xc8\xea\x30\x02\x01\x00")
            if not args.data_only:
                assert any(typ == 0x16 for typ, _ in self.rx.frames)
            self.pump(0.3)
            assert tx_data.in_waiting == 0 and rx_data.in_waiting == 0
            self.results["concurrent_duplex"] = {
                "bytes_each_direction": args.data_bytes,
                "seconds": round(time.monotonic() - start, 3),
                "crsf": True,
                "rc": not args.data_only,
            }

    def data_only(self, args):
        self.pump(1.2)
        self.tx.send(0x28, b"\xec\xea")
        info = self.expect(self.tx, 0x29, lambda p: p[:2] == b"\xea\xec", seconds=100)
        self.results["remote_discovery"] = info.hex()
        model, _ = self.parameter(0xEC, 5)
        assert model[1] == 8
        self.results["remote_parameter"] = True
        self.duplex(args)
        assert not any(typ == 0x16 for typ, _ in self.rx.frames)
        self.results["crc_errors"] = self.tx.bad_crc + self.rx.bad_crc
        assert self.results["crc_errors"] == 0
        return self.results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tx-serial", required=True)
    parser.add_argument("--rx-serial", required=True)
    parser.add_argument("--data-bytes", type=int, default=4096)
    parser.add_argument("--data-only", action="store_true", help="verify CRSF service in data24")
    args = parser.parse_args()
    rig = Rig(args)
    try:
        results = rig.data_only(args) if args.data_only else rig.run(args)
        print(json.dumps(results, ensure_ascii=False, sort_keys=True))
    except Exception:
        print(json.dumps({"partial": rig.results}, ensure_ascii=False, sort_keys=True), flush=True)
        raise
    finally:
        rig.tx.close()
        rig.rx.close()


if __name__ == "__main__":
    main()
