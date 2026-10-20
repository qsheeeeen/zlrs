#!/usr/bin/env python3
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# zlrs management tool: talks to the device over the dedicated management CDC.
#
#   tools/zlrs.py status --port /dev/ttyACM1
#   tools/zlrs.py set-phrase --port /dev/ttyACM1 "correct horse battery staple"
#   python3 tests/tools/run.py --module zlrs_mgmt   # no device: check management framing
#
# Read-only queries leave RF running; --refresh requests fresh chip measurements in maintenance.
# Older firmware falls back to an explicit management session when it rejects an online query.
# The two USB CDCs are data and management (/dev/ttyACM0/1).
# Framing and commands: doc/07-protocol.md#usb-management-framing.
# Public wire definitions: include/zlrs/mgmt/mgmt.h.
#
# Standard library only; talking to a device needs pyserial (pip install pyserial).

import argparse
from collections import namedtuple
from contextlib import contextmanager
import struct
import os
import sys

from zlrs_cli import HelpFormatter, bounded_int, positive_seconds, fields, heading, notice

SOF = b"\xa5\x5a"
VERSION = 5
# Fields follow wire order, with explicit little-endian widths and signedness.
STATUS_FIELDS = {
    "version": "B", "role": "B", "state": "B", "flags": "B",
    "dev_id": "4s", "peer_id": "4s", "lq_percent": "B", "band": "B",
    "region": "B", "tx_power_dbm": "b", "data_baud": "I", "profile": "B",
    "rssi_dbm": "h", "peer_rssi_dbm": "h", "peer_lq_percent": "B",
    "active_power_dbm": "b", "power_floor_dbm": "b", "temperature_centi_c": "h",
    "tx_frames": "I", "rx_frames": "I", "rx_crc_errors": "I", "rx_bad_tag": "I",
    "tx_retries": "I", "data_drop_bytes": "I", "missed_slots": "I", "radio_fail": "I",
    "frequency_hz": "I", "hop_index": "H", "frame_period_us": "I",
    "radio_temperature_centi_c": "h", "radio_xosc_temperature_centi_c": "h",
    "radio_vbat_mv": "H", "radio_version_major": "B", "radio_version_minor": "B",
    "radio_chip_mode": "B", "radio_command_status": "B", "radio_reset_status": "B",
    "radio_valid": "B", "radio_errors": "H", "radio_irq": "I",
    "data_phy": "B",
}
STATUS_STRUCT = struct.Struct("<" + "".join(STATUS_FIELDS.values()))
STATUS_LEN = STATUS_STRUCT.size
Status = namedtuple("Status", STATUS_FIELDS)


CMD_ENTER = 0x00
CMD_STATUS = 0x01
CMD_PAIR = 0x02
CMD_ERASE_PAIR = 0x03
CMD_SET_KEY_HEX = 0x04
CMD_SET_PHRASE = 0x05
CMD_BUILD = 0x06
CMD_IMAGE_HASH = 0x07
CMD_SET_BAUD = 0x08
CMD_SET_REGION = 0x09
CMD_SET_ROLE = 0x0A
CMD_SET_PROFILE = 0x0B
CMD_SET_BAND = 0x0C
CMD_NEW_DEV_ID = 0x0D
CMD_EXIT = 0x0E
CMD_FACTORY_RESET = 0x0F
CMD_RECOVERY = 0x10
CMD_CONFIRM = 0x11
CMD_LOG_INFO = 0x12
CMD_LOG_READ = 0x13
CMD_DEBUG_REPORT = 0x14
CMD_OBSERVE = 0x15
ACK = 0x80
STATUS_RESP = 0x81
BUILD_RESP = 0x82
IMAGE_HASH_RESP = 0x83
LOG_INFO_RESP = 0x84
LOG_DATA_RESP = 0x85
DEBUG_REPORT_RESP = 0x86
OBSERVE_RESP = 0x87
OBSERVE_HEADER_LEN = 8
PAYLOAD_MAX_LEN = STATUS_LEN + OBSERVE_HEADER_LEN
ONLINE_COMMANDS = {CMD_STATUS, CMD_BUILD, CMD_IMAGE_HASH}

# No-payload commands share registration and encoding metadata.
SIMPLE_COMMANDS = {
    "enter": (CMD_ENTER, "enter management mode"),
    "status": (CMD_STATUS, "read link, modulation, signal/power and configuration"),
    "pair": (CMD_PAIR, "start pairing on this device (start the peer too)"),
    "erase-pair": (CMD_ERASE_PAIR, "erase the peer pairing record"),
    "new-dev-id": (CMD_NEW_DEV_ID, "make a fresh device id (drops the pairing record)"),
    "factory-reset": (CMD_FACTORY_RESET, "erase configuration and pairing"),
    "recovery": (CMD_RECOVERY, "reboot into MCUboot serial recovery"),
    "build": (CMD_BUILD, "read the build string"),
    "image-hash": (CMD_IMAGE_HASH, "read the MCUboot hash of the running image (hex)"),
    "confirm": (CMD_CONFIRM, "confirm this upgrade (no confirm means a timeout rollback)"),
    "exit": (CMD_EXIT, "leave management mode and restore the radio"),
}

DEBUG_REPORTS = {"link": 0, "rate": 1, "radio": 2, "power": 3}
DEBUG_REPORT_CHUNK_MAX = 85
DEBUG_REPORT_MAX_LEN = 4096

COMMAND_MESSAGES = {
    CMD_ENTER: "Management mode entered; radio paused",
    CMD_PAIR: "Pairing started",
    CMD_ERASE_PAIR: "Pairing record erased",
    CMD_SET_KEY_HEX: "Root key saved",
    CMD_SET_PHRASE: "Binding phrase saved",
    CMD_SET_BAUD: "Data UART baud rate saved",
    CMD_SET_REGION: "Region saved",
    CMD_SET_ROLE: "Role saved",
    CMD_SET_PROFILE: "Security profile saved",
    CMD_SET_BAND: "Data band saved; reboot to apply",
    CMD_NEW_DEV_ID: "New device ID saved; pairing record cleared",
    CMD_FACTORY_RESET: "Factory settings restored",
    CMD_CONFIRM: "Running firmware confirmed",
    CMD_EXIT: "Management mode exited; radio resumed",
}

# One read carries at most this many bytes (a management frame's payload minus its own header)
LOG_CHUNK_MAX = 80
LOG_FILES_MAX = 8

STATUS_NAMES = {
    0: "ok",
    1: "bad parameter",
    2: "wrong state",
    3: "storage error",
    4: "unknown command",
    5: "temporarily unavailable; retry the command",
}

STATE_NAMES = dict(enumerate(("unconfigured", "unpaired", "pairing", "searching", "connected")))
REGION_NAMES = dict(enumerate(("FCC915", "EU868", "CN470", "CN433")))

def crc8(data: bytes) -> int:
    """DVB-S2 polynomial 0xD5, the same algorithm as zlrs_crc8() on the device."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def encode(cmd: int, payload: bytes = b"") -> bytes:
    if len(payload) > PAYLOAD_MAX_LEN:
        raise ValueError("payload too long")
    body = bytes([VERSION, cmd, len(payload)]) + payload
    return SOF + body + bytes([crc8(body)])


def decode(frame: bytes):
    """Return (cmd, payload); raises ValueError for a short frame or a bad CRC."""
    if len(frame) < 6 or frame[:2] != SOF:
        raise ValueError("bad frame header")
    if frame[2] != VERSION:
        raise ValueError(f"unsupported version {frame[2]}")
    length = frame[4]
    if len(frame) != length + 6:
        raise ValueError("bad frame length")
    if crc8(frame[2:-1]) != frame[-1]:
        raise ValueError("bad crc")
    return frame[3], frame[5:-1]



def decode_status(payload: bytes) -> Status:
    # Allow status/rollback verification against an older confirmed image.
    if len(payload) == 90:
        payload += b"\xff"
    if len(payload) != STATUS_LEN:
        raise ValueError(f"status payload must contain {STATUS_LEN} bytes")
    return Status(*STATUS_STRUCT.unpack(payload))


def read_status(client) -> Status:
    return decode_status(client.request(CMD_STATUS, response=STATUS_RESP))


def status_text(payload: bytes) -> str:
    status = decode_status(payload)
    flags = status.flags
    modulation = {0: "FLRC 2.6 Mbps", 1: "FLRC 650 kbps", 2: "FLRC 325 kbps",
                  3: "LoRa SF6 / BW 1 MHz", 4: "LoRa SF7 / BW 1 MHz",
                  5: "FLRC 1.3 Mbps"}.get(status.data_phy)
    if modulation is None:
        modulation = ("not reported (upgrade device firmware)" if status.data_phy == 255
                      else f"unknown profile ({status.data_phy})")
    sections = [
        ("Link", [("Role", "Leader" if status.role == 0 else "Follower"),
                  ("State", STATE_NAMES.get(status.state, str(status.state))),
                  ("Paired", "yes" if flags & 0x02 else "no"),
                  ("Session", "active" if flags & 0x04 else "inactive")]),
        ("Radio", [
            ("Band mode", "auto" if flags & 0x10 else "fixed"),
            ("Active band", "sub-GHz" if flags & 0x08 else "2.4 GHz"),
            ("Modulation", modulation),
            ("Frequency", f"{status.frequency_hz / 1000000:.3f} MHz"),
            ("Hop index", str(status.hop_index)),
            ("Frame period", f"{status.frame_period_us / 1000:.3f} ms"),
            ("Capture", "searching" if flags & 0x20 else "ready"),
            ("Transition", "scheduled" if flags & 0x40 else "none"),
        ]),
        ("Signal / Power", [
            ("Local RSSI", f"{status.rssi_dbm} dBm" if status.rssi_dbm else "unavailable"),
            ("Local LQ", f"{status.lq_percent}%"),
            ("Peer RSSI", f"{status.peer_rssi_dbm} dBm" if status.peer_rssi_dbm else "unavailable"),
            ("Peer LQ", f"{status.peer_lq_percent}%" if status.peer_rssi_dbm else "unavailable"),
            ("TX power", f"{status.active_power_dbm} dBm"),
            ("TX range", f"{status.power_floor_dbm}..{status.tx_power_dbm} dBm"),
            ("Power control", "debug fixed" if flags & 0x80 else "automatic"),
        ]),
        ("Traffic / Errors", [
            ("RF frames", f"TX {status.tx_frames}, RX {status.rx_frames}"),
            ("ARQ retries", str(status.tx_retries)),
            ("CRC errors", str(status.rx_crc_errors)),
            ("Auth errors", str(status.rx_bad_tag)),
            ("Radio failures", str(status.radio_fail)),
            ("Missed slots", str(status.missed_slots)),
            ("Dropped data", f"{status.data_drop_bytes} bytes"),
        ]),
        ("Device / Configuration", [
            ("Hardware", "HF (868/915 MHz)" if status.band else "LF (470/433 MHz)"),
            ("Region", REGION_NAMES.get(status.region, str(status.region))),
            ("Security profile", f"P{status.profile}"),
            ("Device ID", status.dev_id.hex()),
            ("Peer ID", status.peer_id.hex()),
            ("Data UART", f"{status.data_baud or 115200} baud"),
            ("Root key", "configured" if flags & 0x01 else "missing"),
        ]),
    ]
    valid = status.radio_valid
    radio_temp = status.radio_temperature_centi_c
    xosc_temp = status.radio_xosc_temperature_centi_c
    vbat = status.radio_vbat_mv
    mode, command, reset = (status.radio_chip_mode, status.radio_command_status, status.radio_reset_status)
    mode_names = {0: "sleep", 1: "standby RC", 2: "standby XOSC",
                  3: "frequency synthesis", 4: "RX", 5: "TX"}
    command_names = {0: "failed", 1: "invalid command", 2: "OK", 3: "read data"}
    reset_names = {0: "cleared", 1: "power-on/brown-out", 2: "reset pin",
                   5: "NSS wakeup", 6: "RTC wakeup"}
    errors = status.radio_errors
    error_names = ("HF XOSC start", "LF XOSC start", "PLL lock", "LF RC calibration",
                   "HF RC calibration", "PLL calibration", "AAF calibration",
                   "image calibration", "chip busy", "missing front-end calibration",
                   "measurement ADC calibration", "PA offset calibration",
                   "PPF calibration", "SRC calibration", "SRC saturation",
                   "SRC tolerance")
    error_text = ", ".join(name for bit, name in enumerate(error_names)
                           if errors & (1 << bit)) or "none"
    irq = status.radio_irq
    sections.append(("Sensors", [
        ("MCU chip temp", "unavailable" if status.temperature_centi_c == -32768
         else f"{status.temperature_centi_c / 100:.2f} C"),
        ("LR2021 chip temp", f"{radio_temp / 100:.2f} C" if valid & 0x01 else "unavailable"),
        ("LR2021 XOSC temp", f"{xosc_temp / 100:.2f} C" if valid & 0x02 else "unavailable"),
        ("LR2021 supply", f"{vbat / 1000:.3f} V ({vbat} mV)" if valid & 0x04 else "unavailable"),
    ]))
    sections.append(("LR2021 snapshot", [
        ("Chip version", f"major 0x{status.radio_version_major:02x}, minor 0x{status.radio_version_minor:02x}"
         if valid & 0x08 else "unavailable"),
        ("Chip mode", mode_names.get(mode, f"unknown ({mode})") if valid & 0x10 else "unavailable"),
        ("Last command", command_names.get(command, f"unknown ({command})")
         if valid & 0x10 else "unavailable"),
        ("Reset source", reset_names.get(reset, f"unknown ({reset})")
         if valid & 0x10 else "unavailable"),
        ("IRQ status", f"0x{irq:08x}" if valid & 0x10 else "unavailable"),
        ("Chip errors", f"0x{errors:04x} ({error_text})" if valid & 0x20 else "unavailable"),
    ]))
    return "\n\n".join(title + "\n" + fields(rows) for title, rows in sections)



def read_response(link):
    """Read one frame: find SOF, then collect bytes until the length field is satisfied."""
    buf = bytearray()
    while True:
        byte = link.read(1)
        if not byte:
            raise TimeoutError("timed out waiting for a response")
        buf += byte
        if len(buf) == 1 and buf[0] != 0xA5:
            buf.clear()
        elif len(buf) == 2 and buf[1] != 0x5A:
            buf[:] = b"\xa5" if buf[-1] == 0xA5 else b""
        elif len(buf) == 5 and buf[4] > PAYLOAD_MAX_LEN:
            raise ValueError(f"response payload exceeds {PAYLOAD_MAX_LEN} bytes")
        elif len(buf) >= 5 and len(buf) == buf[4] + 6:
            return decode(bytes(buf))


class ManagementError(ValueError):
    def __init__(self, command, status):
        super().__init__(STATUS_NAMES.get(status, hex(status)))
        self.command = command
        self.status = status


class ManagementClient:
    """One serial connection with command/ACK matching shared by all operations."""

    def __init__(self, link):
        self.link = link
        self.entered = False

    def request(self, command, payload=b"", response=ACK):
        self.link.write(encode(command, payload))
        if response is None:
            self.link.flush()
            return b""
        got, body = read_response(self.link)
        if got == ACK:
            if len(body) != 2 or body[0] != command:
                raise ValueError("malformed or mismatched ACK")
            if body[1] != 0:
                raise ManagementError(command, body[1])
            if response != ACK:
                raise ValueError("expected response data, received only an OK ACK")
        elif got != response:
            raise ValueError(f"unexpected response 0x{got:02x} for command 0x{command:02x}")
        if command == CMD_ENTER:
            self.entered = True
        elif command == CMD_EXIT:
            self.entered = False
        return body


@contextmanager
def management_session(port, baud, timeout, enter=True, keep=False, observe=False):
    import serial  # pyserial

    with serial.Serial(port, baudrate=baud, timeout=timeout) as link:
        client = ManagementClient(link)
        failure = None
        try:
            if enter and not observe:
                client.request(CMD_ENTER)
            yield client
        except BaseException as exc:
            failure = exc
            raise
        finally:
            if not keep and (not observe or client.entered):
                try:
                    client.request(CMD_EXIT)
                except (OSError, ValueError, TimeoutError) as exc:
                    if failure is not None:
                        notice("error", str(failure), file=sys.stderr)
                    raise ValueError("could not leave management mode; run exit to restore "
                                     f"the radio: {exc}") from exc


def read_debug_report(client, report_id: int) -> bytes:
    """Read a debug-only text report in bounded management frames."""
    report = bytearray()
    offset = 0
    total = None

    while total is None or offset < total:
        request = bytes([report_id]) + offset.to_bytes(2, "little")
        payload = client.request(CMD_DEBUG_REPORT, request, DEBUG_REPORT_RESP)
        if len(payload) < 5:
            raise ValueError("short debug report response")

        got_id = payload[0]
        got_offset = int.from_bytes(payload[1:3], "little")
        got_total = int.from_bytes(payload[3:5], "little")
        chunk = payload[5:]
        if got_id != report_id or got_offset != offset:
            raise ValueError("debug report id or offset does not match the request")
        if got_total > DEBUG_REPORT_MAX_LEN:
            raise ValueError(f"debug report is too large ({got_total} bytes)")
        if total is not None and got_total != total:
            raise ValueError("debug report length changed while reading")
        if len(chunk) > DEBUG_REPORT_CHUNK_MAX or offset + len(chunk) > got_total:
            raise ValueError("invalid debug report chunk length")
        if got_total > offset and not chunk:
            raise ValueError("debug report ended before all bytes arrived")

        total = got_total
        report.extend(chunk)
        offset += len(chunk)

    if len(report) != total:
        raise ValueError("debug report is incomplete")
    return bytes(report)


def device_heading(port: str) -> None:
    """Separate each target block in multi-device output."""
    print("-" * 72)
    heading(f"Device: {port}")


def talk(port: str, baud: int, frame: bytes, expect: bool, timeout: float,
         prelude: bytes | None, quiet: bool = False, keep: bool = False,
         refresh: bool = False) -> int:
    command, request = decode(frame)
    response = {CMD_STATUS: STATUS_RESP, CMD_BUILD: BUILD_RESP,
                CMD_IMAGE_HASH: IMAGE_HASH_RESP}.get(command, ACK) if expect else None
    online = command in ONLINE_COMMANDS and prelude is not None and not keep and not refresh
    try:
        with management_session(port, baud, timeout, enter=prelude is not None,
                                keep=keep or command in (CMD_ENTER, CMD_EXIT, CMD_RECOVERY),
                                observe=online) as client:
            if command == CMD_DEBUG_REPORT:
                if len(request) != 3:
                    raise ValueError("bad debug report request")
                report_id = request[0]
                report_name = next((name for name, ident in DEBUG_REPORTS.items()
                                    if ident == report_id), None)
                if report_name is None:
                    raise ValueError(f"unknown debug report id {report_id}")
                heading(f"ZLRS {report_name.title()} Diagnostics")
                print(read_debug_report(client, report_id).decode(errors="replace"), end="")
                return 0

            query = CMD_OBSERVE if online and command == CMD_STATUS else command
            answer = OBSERVE_RESP if query == CMD_OBSERVE else response
            try:
                payload = client.request(query, request, answer)
            except ManagementError as error:
                if not online or error.status not in (2, 4):
                    raise
                client.request(CMD_ENTER)
                payload = client.request(command, request, response)
                query = command
            if response is None:
                notice("info", "Recovery requested; waiting for USB re-enumeration")
            elif response == STATUS_RESP:
                heading("ZLRS Status")
                if query == CMD_OBSERVE:
                    if len(payload) != OBSERVE_HEADER_LEN + STATUS_LEN:
                        raise ValueError("invalid observation snapshot length")
                    sampled_ms, hardware_ms = struct.unpack_from("<II", payload)
                    age = (sampled_ms - hardware_ms) & 0xffffffff
                    print("[Snapshot]")
                    print(fields([("Device uptime", f"{sampled_ms} ms"),
                                  ("Chip observations age", f"{age} ms" if hardware_ms else "unavailable")]))
                    payload = payload[OBSERVE_HEADER_LEN:]
                print(status_text(payload))
            elif response == BUILD_RESP:
                print(payload.decode(errors="replace"))
            elif response == IMAGE_HASH_RESP:
                if len(payload) != 32:
                    raise ValueError("image hash must contain 32 bytes")
                print(payload.hex())
            elif not quiet:
                notice("ok", COMMAND_MESSAGES.get(command, "Command accepted"))
        return 0
    except ImportError:
        notice("error", "pyserial required: pip install pyserial", file=sys.stderr)
        return 2
    except (OSError, ValueError, TimeoutError) as exc:
        notice("error", str(exc), file=sys.stderr)
        return 1


def dump_logs(port: str, baud: int, timeout: float, out_dir: str,
              keep: bool = False, no_enter: bool = False) -> int:
    """Export the log store within one management session."""
    os.makedirs(out_dir, exist_ok=True)
    try:
        with management_session(port, baud, timeout, enter=not no_enter, keep=keep) as client:
            payload = client.request(CMD_LOG_INFO, response=LOG_INFO_RESP)
            if len(payload) < 2:
                raise ValueError("log info is malformed")
            count, newest = payload[:2]
            if count > LOG_FILES_MAX or len(payload) != 2 + 6 * count:
                raise ValueError("log info is malformed")
            files = []
            for i in range(count):
                at = 2 + 6 * i
                num = int.from_bytes(payload[at:at + 2], "little")
                size = int.from_bytes(payload[at + 2:at + 6], "little")
                files.append((i, num, size))

            heading("ZLRS Log Export")
            print(fields([("Files", count), ("Newest index", newest), ("Directory", out_dir)]))
            print()
            total = 0
            for index, num, size in files:
                path = os.path.join(out_dir, f"log.{num:04d}.bin")
                written = 0
                with open(path, "wb") as out:
                    while written < size:
                        want = min(LOG_CHUNK_MAX, size - written)
                        body = bytes([index]) + written.to_bytes(4, "little") + bytes([want])
                        try:
                            payload = client.request(CMD_LOG_READ, body, LOG_DATA_RESP)
                            if len(payload) < 5 or payload[:5] != body[:5]:
                                raise ValueError("log chunk index or offset does not match")
                            got = payload[5:]
                            if not got or len(got) > want:
                                raise ValueError("incomplete or oversized log chunk")
                        except (OSError, ValueError, TimeoutError) as exc:
                            raise ValueError(f"{path}: read failed at offset {written}: {exc}") from exc
                        out.write(got)
                        written += len(got)
                print(f"  {written:>10,} bytes  {path}")
                total += written
        print()
        notice("ok", f"Exported {total:,} bytes from {count} log file(s)")
        return 0
    except ImportError:
        notice("error", "pyserial required: pip install pyserial", file=sys.stderr)
        return 2
    except (OSError, ValueError, TimeoutError) as exc:
        notice("error", str(exc), file=sys.stderr)
        return 1


def app_ports() -> list[str]:
    """Application-mode modules currently plugged in (by-id, bootloader mode excluded)."""
    import glob
    from pathlib import Path

    out = []

    for path in sorted(glob.glob("/dev/serial/by-id/*")):
        name = Path(path).name.upper()

        if "ZLRS" not in name or "MCUBOOT" in name:
            continue
        # Each module has several CDCs; the tool only talks to the management port (if02)
        if not name.endswith("-IF02"):
            continue
        out.append(path)

    return out


def pick_targets(args, parser) -> list[str]:
    """Pick the target modules: explicit --port, then --serial, --all, or the only one present."""
    if args.port:
        return [args.port]

    ports = app_ports()

    if args.serial:
        hit = [p for p in ports if args.serial.upper() in p.upper()]

        if not hit:
            parser.error(f"no module with serial {args.serial!r}; present: {ports}")
        if len(hit) > 1:
            parser.error("serial matches several modules; use a longer serial or --port")
        return hit

    if args.all:
        if not ports:
            parser.error("no ZLRS module found")
        return ports

    if not ports:
        parser.error("no ZLRS module found (nothing matching ZLRS under /dev/serial/by-id)")
    if len(ports) > 1:
        parser.error(
            "several modules are plugged in: pick one with --serial / --port, or use --all:\n  "
            + "\n  ".join(ports))
    return [ports[0]]


def add_options(parser, include_list=True):
    target = parser.add_mutually_exclusive_group()

    target.add_argument("--port", help="management port device (second CDC in application mode, e.g. /dev/ttyACM1)")
    target.add_argument("--serial", help="pick a module by USB serial substring (with several plugged in)")
    target.add_argument("--all", action="store_true",
                        help="run this command on every ZLRS module (with several plugged in)")
    if include_list:
        parser.add_argument("--list", action="store_true", help="list the modules present, then exit")
    else:
        parser.set_defaults(list=False)
    parser.add_argument("--baud", type=bounded_int(1, 4000000), default=115200, help="management connection baud rate")
    parser.add_argument("--timeout", type=positive_seconds, default=10.0,
                        help="response timeout per frame in seconds (management entry may take several seconds)")
    parser.add_argument("--dry-run", action="store_true", help="print the frame instead of talking to the device")
    parser.add_argument("--no-enter", action="store_true",
                        help="skip the enter-management-mode step (when already in it)")
    parser.add_argument("--keep", action="store_true",
                        help="stay in management mode after the command (the default leaves it, which restores the radio)")
    parser.add_argument("--refresh", action="store_true",
                        help="pause RF to refresh chip measurements for a read-only query")


def add_commands(sub, parents=()):
    commands = {}

    def command(name, **kwargs):
        child = sub.add_parser(name, parents=list(parents), formatter_class=HelpFormatter, **kwargs)
        commands[name] = child
        return child

    for name, (_, description) in SIMPLE_COMMANDS.items():
        command(name, help=description, description=description)
    for name in DEBUG_REPORTS:
        description = ("compare per-link TX power limits and setpoints" if name == "power"
                       else f"read detailed {name} diagnostics")
        command(name, help=f"{description} (debug firmware only)")
    phrase = command("set-phrase", help="derive the root key from a binding phrase", description="derive the root key from a binding phrase")
    phrase.add_argument("phrase", help="1..32 UTF-8 bytes; use the same phrase on both peers")
    key = command("set-key-hex", help="set a 32-byte root key", description="set a 32-byte root key")
    key.add_argument("hex_key", help="64 hex characters")
    baud = command("set-baud", help="set the data UART baud rate", description="set the data UART baud rate")
    baud.add_argument("data_baud", type=bounded_int(9600, 4000000), help="data UART baud rate")
    region = command("set-region", help="select a region compatible with the HF/LF board", description="select a region compatible with the HF/LF board")
    region.add_argument("region", type=int, choices=[0, 1, 2, 3],
                        help="0=FCC915 1=EU868 2=CN470 3=CN433")
    role = command("set-role", help="select the Leader or Follower role",
                   description="Select the timing role; both endpoints send and receive DATA.")
    role.add_argument("role", choices=["leader", "follower"],
                      help="leader provides the clock; follower follows it")
    profile = command("set-profile", help="select the authentication profile", description="select the authentication profile")
    profile.add_argument("profile", choices=["p0", "p1", "p2"],
                         help="P0 has no tag (debug images only), P1 is the production profile, "
                              "P2 is not implemented yet")
    band = command("set-band", help="select automatic switching or a fixed band (reboot required)",
                   description="Save the data band; reboot both peers to apply.")
    band.add_argument("band", choices=["auto", "2.4", "subg"],
                      help="select the data frequency band")
    logs = command("logs", help="dump the flash log store to a directory")
    logs.add_argument("out_dir", nargs="?", default="zlrs-logs",
                      help="where the log files are written (default: ./zlrs-logs)")
    return commands


def main(args=None, parser=None) -> int:
    if args is None:
        parser = argparse.ArgumentParser(
            description="Configure and inspect ZLRS through its management USB port.",
            formatter_class=HelpFormatter)
        add_options(parser)
        sub = parser.add_subparsers(dest="command", title="commands", metavar="COMMAND")
        add_commands(sub)
        args = parser.parse_args()

    if args.list:
        ports = app_ports()

        if not ports:
            notice("warn", "No ZLRS module plugged in")
            return 1
        heading("ZLRS Devices")
        print(f"  {'#':<4}Management port")
        for i, path in enumerate(ports, 1):
            print(f"  {i:<4}{path}")
        print(f"\n  {len(ports)} device(s) connected")
        return 0

    if args.command is None:
        parser.print_help()
        return 2

    # The log dump is a sequence of requests, not one frame; it runs its own session.
    if args.command == "logs":
        if args.dry_run:
            print(f"[dry-run] export logs to {args.out_dir}; no files or ports opened")
            if not args.no_enter:
                print(f"enter: {encode(CMD_ENTER).hex()}")
            print(f"info : {encode(CMD_LOG_INFO).hex()}")
            print("read : request chunks using offsets and lengths returned by log-info")
            if not args.keep:
                print(f"exit : {encode(CMD_EXIT).hex()}")
            return 0
        targets = pick_targets(args, parser)
        rc = 0
        for port in targets:
            if len(targets) > 1:
                device_heading(port)
            this = dump_logs(port, args.baud, args.timeout,
                             args.out_dir if len(targets) == 1 else os.path.join(args.out_dir,
                                                                                os.path.basename(port)),
                             keep=args.keep, no_enter=args.no_enter)
            if this != 0:
                rc = this
        return rc

    if args.command in SIMPLE_COMMANDS:
        frame = encode(SIMPLE_COMMANDS[args.command][0])
        expect = args.command != "recovery"
    elif args.command in DEBUG_REPORTS:
        frame = encode(CMD_DEBUG_REPORT, bytes([DEBUG_REPORTS[args.command], 0, 0]))
        expect = True
    elif args.command == "set-phrase":
        payload = args.phrase.encode()
        if not 1 <= len(payload) <= 32:
            parser.error("binding phrase must contain 1..32 UTF-8 bytes")
        frame, expect = encode(CMD_SET_PHRASE, payload), True
    elif args.command == "set-key-hex":
        if len(args.hex_key) != 64 or any(c not in "0123456789abcdefABCDEF" for c in args.hex_key):
            parser.error("the root key must be exactly 64 hex characters")
        frame, expect = encode(CMD_SET_KEY_HEX, args.hex_key.encode()), True
    elif args.command == "set-baud":
        frame = encode(CMD_SET_BAUD, args.data_baud.to_bytes(4, "little"))
        expect = True
    elif args.command == "set-region":
        frame = encode(CMD_SET_REGION, bytes([args.region]))
        expect = True
    elif args.command == "set-role":
        frame = encode(CMD_SET_ROLE, bytes([0 if args.role == "leader" else 1]))
        expect = True
    elif args.command == "set-profile":
        frame = encode(CMD_SET_PROFILE, bytes([int(args.profile[1])]))
        expect = True
    elif args.command == "set-band":
        frame = encode(CMD_SET_BAND, bytes([{"2.4": 0, "subg": 1, "auto": 2}[args.band]]))
        expect = True
    else:
        parser.error(f"unknown command: {args.command}")

    query_command = decode(frame)[0]
    online = (query_command in ONLINE_COMMANDS and not args.no_enter and
              not args.keep and not args.refresh)
    if args.dry_run:
        if args.command not in ("enter", "recovery", "exit") and not args.no_enter and not online:
            print(f"enter: {encode(CMD_ENTER).hex()}")
        command_frame = encode(CMD_OBSERVE) if online and query_command == CMD_STATUS else frame
        print(f"cmd  : {command_frame.hex()}")
        if args.command not in ("enter", "exit", "recovery") and not args.keep and not online:
            print(f"exit : {encode(CMD_EXIT).hex()}")
        return 0

    prelude = None
    if args.command not in ("enter", "exit", "recovery") and not args.no_enter:
        prelude = encode(CMD_ENTER)

    # With several modules plugged in, decide the targets first (--serial / --port / --all).
    targets = pick_targets(args, parser)

    rc = 0
    for port in targets:
        if len(targets) > 1:
            device_heading(port)
        try:
            this = talk(port, args.baud, frame, expect, args.timeout, prelude, keep=args.keep,
                        refresh=args.refresh)
        except (OSError, ValueError, TimeoutError) as exc:
            notice("error", f"{port}: {exc}", file=sys.stderr)
            this = 1
        if this != 0:
            rc = this

    return rc
