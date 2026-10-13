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
# Except for `enter`, every command first sends enter-management-mode and waits for the ACK.
# The two USB CDCs are data and management (/dev/ttyACM0/1).
# Framing and the command table: doc/09-security.md 9 and include/zlrs/mgmt/mgmt.h.
#
# Standard library only; talking to a device needs pyserial (pip install pyserial).

import argparse
import os
import sys

from zlrs_cli import HelpFormatter, bounded_int, positive_seconds, fields, heading, notice

SOF = b"\xa5\x5a"
VERSION = 4
STATUS_LEN = 90

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
CMD_SET_POWER = 0x0A
CMD_SET_ROLE = 0x0B
CMD_SET_PROFILE = 0x0C
CMD_SET_BAND = 0x0D
CMD_NEW_DEV_ID = 0x0E
CMD_EXIT = 0x0F
CMD_FACTORY_RESET = 0x10
CMD_RECOVERY = 0x11
CMD_CONFIRM = 0x12
CMD_LOG_INFO = 0x13
CMD_LOG_READ = 0x14
CMD_DEBUG_REPORT = 0x15
ACK = 0x80
STATUS_RESP = 0x81
BUILD_RESP = 0x82
IMAGE_HASH_RESP = 0x83
LOG_INFO_RESP = 0x84
LOG_DATA_RESP = 0x85
DEBUG_REPORT_RESP = 0x86

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
    CMD_SET_POWER: "TX power ceiling saved",
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
    if len(payload) > 90:
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



def status_text(payload: bytes) -> str:
    if len(payload) != STATUS_LEN:
        raise ValueError(f"status payload must contain {STATUS_LEN} bytes")
    flags = payload[3]
    dev = payload[4:8].hex()
    peer = payload[8:12].hex()
    baud = int.from_bytes(payload[16:20], "little")
    power = payload[15] - 256 if payload[15] > 127 else payload[15]
    sections = [
        ("Device", [("Role", "TX" if payload[1] == 0 else "RX"),
                    ("Hardware", "HF (868/915 MHz)" if payload[13] else "LF (470/433 MHz)"),
                    ("Device ID", dev), ("Peer ID", peer)]),
        ("Link", [("Data band", "sub-GHz" if flags & 0x08 else "2.4 GHz"),
                  ("State", STATE_NAMES.get(payload[2], str(payload[2]))),
                  ("Quality", f"{payload[12]}%"),
                  ("Paired", "yes" if flags & 0x02 else "no"),
                  ("Session", "active" if flags & 0x04 else "inactive")]),
        ("Configuration", [("Region", REGION_NAMES.get(payload[14], str(payload[14]))),
                           ("TX ceiling", f"{power} dBm"),
                           ("Data UART", f"{baud or 115200} baud"),
                           ("Profile", f"P{payload[20]}"),
                           ("Root key", "configured" if flags & 0x01 else "missing")]),
    ]
    rssi = int.from_bytes(payload[21:23], "little", signed=True)
    peer_rssi = int.from_bytes(payload[23:25], "little", signed=True)
    active_power = int.from_bytes(payload[26:27], "little", signed=True)
    floor = int.from_bytes(payload[27:28], "little", signed=True)
    temperature = int.from_bytes(payload[28:30], "little", signed=True)
    counters = [int.from_bytes(payload[offset:offset + 4], "little")
                for offset in range(30, 62, 4)]
    tx, rx, crc, tag, retry, dropped, missed, failed = counters
    frequency = int.from_bytes(payload[62:66], "little")
    hop = int.from_bytes(payload[66:68], "little")
    period = int.from_bytes(payload[68:72], "little")
    sections.extend([
        ("Measurements", [
            ("RSSI", f"{rssi} dBm" if rssi else "unavailable"),
            ("Peer RSSI", f"{peer_rssi} dBm" if peer_rssi else "unavailable"),
            ("Peer LQ", f"{payload[25]}%" if peer_rssi else "unavailable"),
            ("TX setpoint", f"{active_power} dBm"),
            ("Power floor", f"{floor} dBm"),
            ("MCU temperature", "unavailable" if temperature == -32768
             else f"{temperature / 100:.2f} C"),
            ("Hop frequency", f"{frequency / 1000000:.3f} MHz"),
            ("Hop index", str(hop)),
            ("Frame period", f"{period / 1000:.3f} ms"),
        ]),
        ("Counters", [
            ("RF frames", f"TX {tx}, RX {rx}"),
            ("ARQ retries", str(retry)),
            ("CRC errors", str(crc)),
            ("Auth errors", str(tag)),
            ("Radio failures", str(failed)),
            ("Missed slots", str(missed)),
            ("Dropped data", f"{dropped} bytes"),
        ]),
    ])
    valid = payload[83]
    radio_temp = int.from_bytes(payload[72:74], "little", signed=True)
    xosc_temp = int.from_bytes(payload[74:76], "little", signed=True)
    vbat = int.from_bytes(payload[76:78], "little")
    mode, command, reset = payload[80:83]
    mode_names = {0: "sleep", 1: "standby RC", 2: "standby XOSC",
                  3: "frequency synthesis", 4: "RX", 5: "TX"}
    command_names = {0: "failed", 1: "invalid command", 2: "OK", 3: "read data"}
    reset_names = {0: "cleared", 1: "power-on/brown-out", 2: "reset pin",
                   5: "NSS wakeup", 6: "RTC wakeup"}
    errors = int.from_bytes(payload[84:86], "little")
    error_names = ("HF XOSC start", "LF XOSC start", "PLL lock", "LF RC calibration",
                   "HF RC calibration", "PLL calibration", "AAF calibration",
                   "image calibration", "chip busy", "missing front-end calibration",
                   "measurement ADC calibration", "PA offset calibration",
                   "PPF calibration", "SRC calibration", "SRC saturation",
                   "SRC tolerance")
    error_text = ", ".join(name for bit, name in enumerate(error_names)
                           if errors & (1 << bit)) or "none"
    irq = int.from_bytes(payload[86:90], "little")
    sections.append(("LR2021", [
        ("Junction temperature", f"{radio_temp / 100:.2f} C" if valid & 0x01 else "unavailable"),
        ("XOSC temperature", f"{xosc_temp / 100:.2f} C" if valid & 0x02 else "unavailable"),
        ("Supply voltage", f"{vbat / 1000:.3f} V ({vbat} mV)" if valid & 0x04 else "unavailable"),
        ("Chip version", f"major 0x{payload[78]:02x}, minor 0x{payload[79]:02x}"
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
        elif len(buf) == 5 and buf[4] > 90:
            raise ValueError("response payload exceeds 90 bytes")
        elif len(buf) >= 5 and len(buf) == buf[4] + 6:
            return decode(bytes(buf))


def read_debug_report(link, report_id: int) -> bytes:
    """Read a debug-only text report in bounded management frames."""
    report = bytearray()
    offset = 0
    total = None

    while total is None or offset < total:
        request = bytes([report_id]) + offset.to_bytes(2, "little")
        link.write(encode(CMD_DEBUG_REPORT, request))
        cmd, payload = read_response(link)

        if cmd == ACK:
            if len(payload) != 2 or payload[0] != CMD_DEBUG_REPORT:
                raise ValueError("ACK does not match the debug report command")
            if payload[1] == 4:
                raise ValueError("detailed reports require a debug firmware image")
            raise ValueError(STATUS_NAMES.get(payload[1], hex(payload[1])))
        if cmd != DEBUG_REPORT_RESP or len(payload) < 5:
            raise ValueError(f"unexpected debug report response 0x{cmd:02x}")

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
         prelude: bytes | None, quiet: bool = False) -> int:
    try:
        import serial  # pyserial
    except ImportError:
        notice("error", "pyserial required: pip install pyserial", file=sys.stderr)
        return 2

    with serial.Serial(port, baudrate=baud, timeout=timeout) as link:
        if prelude is not None:
            link.write(prelude)
            try:
                cmd, payload = read_response(link)
            except TimeoutError as exc:
                notice("error", f"entering management mode failed: {exc}", file=sys.stderr)
                return 1

            code = payload[1] if cmd == ACK and len(payload) >= 2 else 0xFF
            if cmd != ACK or len(payload) != 2 or payload[0] != CMD_ENTER or code != 0:
                notice("error", f"entering management mode failed: {STATUS_NAMES.get(code, hex(code))}", file=sys.stderr)
                return 1

        if frame[3] == CMD_DEBUG_REPORT:
            if len(frame) != 9 or frame[4] != 3:
                raise ValueError("bad debug report request")
            report_id = frame[5]
            report_name = next((name for name, ident in DEBUG_REPORTS.items()
                                if ident == report_id), None)
            if report_name is None:
                raise ValueError(f"unknown debug report id {report_id}")
            heading(f"ZLRS {report_name.title()} Diagnostics")
            print(read_debug_report(link, report_id).decode(errors="replace"), end="")
            return 0

        link.write(frame)
        if not expect:
            link.flush()
            notice("info", "Recovery requested; waiting for USB re-enumeration")
            return 0

        try:
            cmd, payload = read_response(link)
        except TimeoutError:
            notice("error", "timed out waiting for a response", file=sys.stderr)
            return 1
        if cmd == ACK:
            if len(payload) != 2 or payload[0] != frame[3]:
                raise ValueError("ACK does not match the command")
            code = payload[1]
            if code != 0:
                notice("error", STATUS_NAMES.get(code, hex(code)), file=sys.stderr)
                return 1
            # Read commands must return data; an OK ACK alone is incomplete.
            if frame[3] in (CMD_STATUS, CMD_BUILD, CMD_IMAGE_HASH):
                raise ValueError("expected response data but received only an ACK")
            if not quiet:
                notice("ok", COMMAND_MESSAGES.get(frame[3], "Command accepted"))
            return 0
        expected = {CMD_STATUS: STATUS_RESP, CMD_BUILD: BUILD_RESP,
                    CMD_IMAGE_HASH: IMAGE_HASH_RESP}.get(frame[3], ACK)
        if cmd != expected:
            raise ValueError(f"unexpected response 0x{cmd:02x} for command 0x{frame[3]:02x}")
        if cmd == STATUS_RESP:
            heading("ZLRS Status")
            print(status_text(payload))
            return 0
        if cmd == BUILD_RESP:
            print(payload.decode(errors="replace"))
            return 0
        if cmd == IMAGE_HASH_RESP:
            if len(payload) != 32:
                raise ValueError("image hash must contain 32 bytes")
            print(payload.hex())
            return 0
        print(f"unexpected response 0x{cmd:02x}: {payload.hex()}")
        return 1


def dump_logs(port: str, baud: int, timeout: float, out_dir: str,
              keep: bool = False, no_enter: bool = False) -> int:
    """Read the whole log store over the management port and write it out file by file.

    The device serves one chunk per request; the framing already has a CRC, so a lost byte shows up
    as a bad frame instead of as a hole in the log.
    """
    try:
        import serial  # pyserial
    except ImportError:
        notice("error", "pyserial required: pip install pyserial", file=sys.stderr)
        return 2

    import os

    os.makedirs(out_dir, exist_ok=True)

    with serial.Serial(port, baudrate=baud, timeout=timeout) as link:
        if not no_enter:
            link.write(encode(CMD_ENTER))
            try:
                cmd, payload = read_response(link)
            except TimeoutError as exc:
                notice("error", f"entering management mode failed: {exc}", file=sys.stderr)
                return 1
            if cmd != ACK or len(payload) != 2 or payload != bytes([CMD_ENTER, 0]):
                notice("error", "entering management mode failed", file=sys.stderr)
                return 1

        try:
            link.write(encode(CMD_LOG_INFO))
            try:
                cmd, payload = read_response(link)
            except TimeoutError:
                notice("error", "no answer to the log info request", file=sys.stderr)
                return 1
            if cmd == ACK:
                if len(payload) != 2 or payload[0] != CMD_LOG_INFO:
                    notice("error", "log store: malformed or mismatched ACK", file=sys.stderr)
                    return 1
                code = payload[1] if len(payload) == 2 else 0xFF
                notice("error", f"log store: {STATUS_NAMES.get(code, hex(code))}", file=sys.stderr)
                return 1
            if cmd != LOG_INFO_RESP or len(payload) < 2:
                notice("error", f"unexpected response 0x{cmd:02x}", file=sys.stderr)
                return 1

            count = payload[0]
            newest = payload[1]
            if count > LOG_FILES_MAX or len(payload) < 2 + 6 * count:
                notice("error", "log info is malformed", file=sys.stderr)
                return 1

            files = []
            for i in range(count):
                at = 2 + 6 * i
                num = payload[at] | (payload[at + 1] << 8)
                size = int.from_bytes(payload[at + 2 : at + 6], "little")
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
                        link.write(encode(CMD_LOG_READ, body))
                        try:
                            cmd, payload = read_response(link)
                        except TimeoutError:
                            notice("error", f"{path}: timed out at offset {written}", file=sys.stderr)
                            return 1
                        if cmd == ACK:
                            if len(payload) != 2 or payload[0] != CMD_LOG_READ:
                                reason = "malformed or mismatched ACK"
                            elif payload[1] == 0:
                                reason = "expected log data, received only an OK ACK"
                            else:
                                reason = STATUS_NAMES.get(payload[1], hex(payload[1]))
                            notice("error", f"{path}: read failed at offset {written}: {reason}",
                                   file=sys.stderr)
                            return 1
                        if cmd != LOG_DATA_RESP or len(payload) < 5:
                            notice("error", f"{path}: unexpected response 0x{cmd:02x} at offset {written}",
                                   file=sys.stderr)
                            return 1
                        got = payload[5:]
                        if payload[0] != index or int.from_bytes(payload[1:5], "little") != written:
                            raise ValueError(f"{path}: log chunk index or offset does not match")
                        if not got or len(got) > want:
                            raise ValueError(f"{path}: incomplete or oversized log chunk at {written}")
                        out.write(got)
                        written += len(got)
                print(f"  {written:>10,} bytes  {path}")
                total += written

        finally:
            if not keep:
                link.write(encode(CMD_EXIT))
                cmd, payload = read_response(link)
                if cmd != ACK or payload != bytes([CMD_EXIT, 0]):
                    raise ValueError("could not leave management mode; run exit to restore the radio")

    print()
    notice("ok", f"Exported {total:,} bytes from {count} log file(s)")
    return 0


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


def add_commands(sub, parents=()):
    commands = {}

    def command(name, **kwargs):
        child = sub.add_parser(name, parents=list(parents), formatter_class=HelpFormatter, **kwargs)
        commands[name] = child
        return child

    command("enter", help="enter management mode")
    command("status", help="read link state, pairing and runtime settings", description="read link state, pairing and runtime settings")
    for name in DEBUG_REPORTS:
        description = ("compare per-link TX power limits and setpoints" if name == "power"
                       else f"read detailed {name} diagnostics")
        command(name, help=f"{description} (debug firmware only)")
    command("pair", help="start pairing on this device (start the peer too)", description="start pairing on this device (start the peer too)")
    command("erase-pair", help="erase the peer pairing record", description="erase the peer pairing record")
    phrase = command("set-phrase", help="derive the root key from a binding phrase", description="derive the root key from a binding phrase")
    phrase.add_argument("phrase", help="1..32 UTF-8 bytes; use the same phrase on both peers")
    key = command("set-key-hex", help="set a 32-byte root key", description="set a 32-byte root key")
    key.add_argument("hex_key", help="64 hex characters")
    baud = command("set-baud", help="set the data UART baud rate", description="set the data UART baud rate")
    baud.add_argument("data_baud", type=bounded_int(9600, 4000000), help="data UART baud rate")
    region = command("set-region", help="select a region compatible with the HF/LF board", description="select a region compatible with the HF/LF board")
    region.add_argument("region", type=int, choices=[0, 1, 2, 3],
                        help="0=FCC915 1=EU868 2=CN470 3=CN433")
    power = command("set-power", help="set the regional TX power ceiling", description="set the regional TX power ceiling")
    power.add_argument("dbm", type=bounded_int(-128, 127), help="TX power ceiling (dBm)")
    role = command("set-role", help="select the runtime TX or RX role", description="select the runtime TX or RX role")
    role.add_argument("role", choices=["tx", "rx"], help="runtime role: tx or rx")
    profile = command("set-profile", help="select the authentication profile", description="select the authentication profile")
    profile.add_argument("profile", choices=["p0", "p1", "p2"],
                         help="P0 has no tag (debug images only), P1 is the production profile, "
                              "P2 is not implemented yet")
    band = command("set-band", help="select 2.4 GHz or sub-GHz data (reboot required)",
                   description="Save the data band; reboot both peers to apply.")
    band.add_argument("band", choices=["2.4", "subg"],
                      help="select the data frequency band")
    command("new-dev-id",
                   help="make a fresh device id (drops the pairing record; read status for the new one)")
    command("factory-reset", help="erase configuration and pairing", description="erase configuration and pairing")
    command("recovery", help="reboot into MCUboot serial recovery", description="reboot into MCUboot serial recovery")
    command("build", help="read the build string")
    command("image-hash", help="read the MCUboot hash of the running image (hex)")
    logs = command("logs", help="dump the flash log store to a directory")
    logs.add_argument("out_dir", nargs="?", default="zlrs-logs",
                      help="where the log files are written (default: ./zlrs-logs)")
    command("confirm", help="confirm this upgrade (no confirm means a timeout rollback)")
    command("exit", help="leave management mode and restore the radio", description="leave management mode and restore the radio")
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

    if args.command == "enter":
        frame, expect = encode(CMD_ENTER), True
    elif args.command == "status":
        frame, expect = encode(CMD_STATUS), True
    elif args.command in DEBUG_REPORTS:
        frame = encode(CMD_DEBUG_REPORT, bytes([DEBUG_REPORTS[args.command], 0, 0]))
        expect = True
    elif args.command == "pair":
        frame, expect = encode(CMD_PAIR), True
    elif args.command == "erase-pair":
        frame, expect = encode(CMD_ERASE_PAIR), True
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
    elif args.command == "set-power":
        frame = encode(CMD_SET_POWER, bytes([args.dbm & 0xFF]))
        expect = True
    elif args.command == "set-role":
        frame = encode(CMD_SET_ROLE, bytes([0 if args.role == "tx" else 1]))
        expect = True
    elif args.command == "set-profile":
        frame = encode(CMD_SET_PROFILE, bytes([int(args.profile[1])]))
        expect = True
    elif args.command == "set-band":
        frame = encode(CMD_SET_BAND, bytes([{"2.4": 0, "subg": 1}[args.band]]))
        expect = True
    elif args.command == "new-dev-id":
        frame = encode(CMD_NEW_DEV_ID, b"")
        expect = True
    elif args.command == "factory-reset":
        frame, expect = encode(CMD_FACTORY_RESET), True
    elif args.command == "recovery":
        frame, expect = encode(CMD_RECOVERY), False
    elif args.command == "build":
        frame, expect = encode(CMD_BUILD), True
    elif args.command == "image-hash":
        frame, expect = encode(CMD_IMAGE_HASH), True
    elif args.command == "confirm":
        frame, expect = encode(CMD_CONFIRM), True
    else:  # exit
        frame, expect = encode(CMD_EXIT), True

    if args.dry_run:
        if args.command not in ("enter", "recovery", "exit") and not args.no_enter:
            print(f"enter: {encode(CMD_ENTER).hex()}")
        print(f"cmd  : {frame.hex()}")
        if args.command not in ("enter", "exit", "recovery") and not args.keep:
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
            this = talk(port, args.baud, frame, expect, args.timeout, prelude)
        except (OSError, ValueError, TimeoutError) as exc:
            notice("error", f"{port}: {exc}", file=sys.stderr)
            this = 1
        if this != 0:
            rc = this
        if args.command not in ("enter", "exit", "recovery") and not args.keep:
            try:
                cleanup = talk(port, args.baud, encode(CMD_EXIT), True, args.timeout, None, quiet=True)
            except (OSError, ValueError, TimeoutError) as exc:
                notice("error", f"{port}: {exc}", file=sys.stderr)
                cleanup = 1
            if cleanup != 0:
                notice("error", "could not leave management mode; run exit to restore the radio",
                       file=sys.stderr)
                rc = cleanup

    return rc
