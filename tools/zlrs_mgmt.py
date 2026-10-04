#!/usr/bin/env python3
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# zlrs management tool: talks to the device over the dedicated management CDC.
#
#   tools/zlrs.py status --port /dev/ttyACM2
#   tools/zlrs.py set-phrase --port /dev/ttyACM2 "correct horse battery staple"
#   tools/zlrs.py --self-test          # no device: frame byte-for-byte against the C side
#
# Except for `enter`, every command first sends enter-management-mode and waits for the ACK.
# The three USB CDCs are CRSF, data port and management port (/dev/ttyACM0/1/2).
# Framing and the command table: doc/09-security.md 9 and include/zlrs/mgmt/mgmt.h.
#
# Standard library only; talking to a device needs pyserial (pip install pyserial).

import argparse
import os
import sys

from zlrs_cli import HelpFormatter, bounded_int, positive_seconds, fields, heading, notice

SOF = b"\xa5\x5a"
VERSION = 3

CMD_ENTER = 0x00
CMD_STATUS = 0x01
CMD_PAIR = 0x02
CMD_ERASE_PAIR = 0x03
CMD_SET_KEY_HEX = 0x10
CMD_SET_PHRASE = 0x11
CMD_SET_FAILSAFE = 0x20
CMD_SET_BAUD = 0x21
CMD_SET_REGION = 0x22
CMD_SET_POWER = 0x23
CMD_SET_ROLE = 0x24
CMD_SET_PROFILE = 0x25
CMD_NEW_DEV_ID = 0x26
CMD_SET_MODE = 0x27
CMD_EXIT = 0x30
CMD_FACTORY_RESET = 0x40
CMD_RECOVERY = 0x41
CMD_CONFIRM = 0x42
CMD_LOG_INFO = 0x50
CMD_LOG_READ = 0x51
CMD_BUILD = 0x12
CMD_IMAGE_HASH = 0x13
ACK = 0x80
STATUS_RESP = 0x81
BUILD_RESP = 0x82
IMAGE_HASH_RESP = 0x83
LOG_INFO_RESP = 0x90
LOG_DATA_RESP = 0x91

COMMAND_MESSAGES = {
    CMD_ENTER: "Management mode entered; radio paused",
    CMD_PAIR: "Pairing started",
    CMD_ERASE_PAIR: "Pairing record erased",
    CMD_SET_KEY_HEX: "Root key saved",
    CMD_SET_PHRASE: "Binding phrase saved",
    CMD_SET_FAILSAFE: "Failsafe channels saved",
    CMD_SET_BAUD: "Data UART baud rate saved",
    CMD_SET_REGION: "Region saved",
    CMD_SET_POWER: "TX power ceiling saved",
    CMD_SET_ROLE: "Role saved",
    CMD_SET_PROFILE: "Security profile saved",
    CMD_SET_MODE: "Operating mode saved; reboot to apply",
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

CHANNEL_MIN = 172
CHANNEL_MID = 992


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


def channels_to_payload(channels) -> bytes:
    """Channel values (11 bit) -> 22-byte payload, the same layout as a CRSF channel frame."""
    acc = 0
    bits = 0
    out = bytearray()
    for value in channels:
        acc |= value & 0x7FF
        bits += 11
        while bits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            bits -= 8
    return bytes(out)


def parse_channels(text: str):
    """Shorthands such as '172,992,...' or '@min'."""
    if text.startswith("@"):
        count = 16
        if text != "@default":
            raise ValueError(f"unknown preset {text}")
        return [CHANNEL_MIN] + [CHANNEL_MID] * (count - 1)

    values = [int(part, 0) for part in text.split(",")]
    if len(values) != 16:
        raise ValueError("need exactly 16 channel values")
    for value in values:
        if not CHANNEL_MIN <= value <= 1811:
            raise ValueError(f"channel {value} out of range {CHANNEL_MIN}..1811")
    return values


def status_text(payload: bytes) -> str:
    if len(payload) < 21:
        raise ValueError(f"status payload too short: {payload.hex()}")
    flags = payload[3]
    dev = payload[4:8].hex()
    peer = payload[8:12].hex()
    baud = int.from_bytes(payload[16:20], "little")
    power = payload[15] - 256 if payload[15] > 127 else payload[15]
    sections = [
        ("Device", [("Role", "TX" if payload[1] == 0 else "RX"),
                    ("Hardware", "HF (868/915 MHz)" if payload[13] else "LF (470/433 MHz)"),
                    ("Device ID", dev), ("Peer ID", peer)]),
        ("Link", [("Mode", "data24 (2.4 GHz data only)" if flags & 0x08 else "mixed"),
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
    return "\n\n".join(title + "\n" + fields(rows) for title, rows in sections)



def self_test() -> int:
    """Frames must match the output of zlrs_mgmt_encode() on the C side byte for byte."""
    cases = [
        ("enter", CMD_ENTER, b"", "a55a03000050"),
        ("status", CMD_STATUS, b"", "a55a0301005b"),
        ("pair", CMD_PAIR, b"", "a55a03020046"),
        ("erase-pair", CMD_ERASE_PAIR, b"", "a55a0303004d"),
        ("set-phrase", CMD_SET_PHRASE, b"hello", "a55a03110568656c6c6ff7"),
        ("set-key-hex", CMD_SET_KEY_HEX, b"00" * 32, None),
        ("set-baud", CMD_SET_BAUD, bytes([0x00, 0x25, 0x06, 0x00]), "a55a0321040025060056"),
        ("set-region", CMD_SET_REGION, bytes([3]), "a55a032201038f"),
        ("set-power", CMD_SET_POWER, bytes([20]), "a55a032301140a"),
        ("set-role", CMD_SET_ROLE, bytes([1]), "a55a0324010150"),
        ("set-profile", CMD_SET_PROFILE, bytes([2]), "a55a0325010279"),
        ("new-dev-id", CMD_NEW_DEV_ID, b"", "a55a032600df"),
        ("factory-reset", CMD_FACTORY_RESET, b"", "a55a034000ef"),
        ("recovery", CMD_RECOVERY, b"", "a55a034100e4"),
        ("build", CMD_BUILD, b"", "a55a031200f6"),
        ("image-hash", CMD_IMAGE_HASH, b"", "a55a031300fd"),
        ("confirm", CMD_CONFIRM, b"", "a55a034200f9"),
        ("exit", CMD_EXIT, b"", "a55a03300055"),
    ]

    failures = 0
    for name, cmd, payload, want in cases:
        if want is None:
            # Only check that a legal frame comes out and decodes again
            frame = encode(cmd, payload)
            got_cmd, got_payload = decode(frame)
            if got_cmd != cmd or got_payload != payload:
                print(f"  FAIL {name}: round trip")
                failures += 1
            continue
        got = encode(cmd, payload).hex()
        if got != want:
            print(f"  FAIL {name}: got {got} want {want}")
            failures += 1

    # One-way check: decode that frame again
    cmd, payload = decode(bytes.fromhex("a55a03110568656c6c6ff7"))
    if cmd != CMD_SET_PHRASE or payload != b"hello":
        print("  FAIL decode round trip")
        failures += 1

    # Channel payload: all zeros gives 22 zero bytes; the default is throttle low, the rest centred
    if channels_to_payload([0] * 16) != bytes(22):
        print("  FAIL channels all zero")
        failures += 1
    default = channels_to_payload(parse_channels("@default"))
    if len(default) != 22 or default[0] != 0xAC:
        print("  FAIL channels default")
        failures += 1

    # Target picking: with exactly one module present and no flags the result has to be a
    # one-element list. Returning the path itself made the caller iterate its characters.
    fake = argparse.ArgumentParser()
    fake.add_argument("--port")
    fake.add_argument("--serial")
    fake.add_argument("--all", action="store_true")
    plain = argparse.Namespace(port=None, serial=None, all=False)
    saved = globals()["app_ports"]
    globals()["app_ports"] = lambda: ["/dev/serial/by-id/usb-fake-if04"]

    try:
        picked = pick_targets(plain, fake)
    finally:
        globals()["app_ports"] = saved

    if picked != ["/dev/serial/by-id/usb-fake-if04"]:
        print(f"  FAIL pick one module: {picked!r}")
        failures += 1

    print(f"self-test: {'ok' if failures == 0 else str(failures) + ' failure(s)'}")
    return 0 if failures == 0 else 1


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
        # Each module has several CDCs; the tool only talks to the management port (if04)
        if not name.endswith("-IF04"):
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

    target.add_argument("--port", help="management port device (third CDC in application mode, e.g. /dev/ttyACM2)")
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
    parser.add_argument("--self-test", action="store_true", help="check framing, then exit")


def add_commands(sub, parents=()):
    commands = {}

    def command(name, **kwargs):
        child = sub.add_parser(name, parents=list(parents), formatter_class=HelpFormatter, **kwargs)
        commands[name] = child
        return child

    command("enter", help="enter management mode")
    command("status", help="read link state, pairing and runtime settings", description="read link state, pairing and runtime settings")
    command("pair", help="start pairing on this device (start the peer too)", description="start pairing on this device (start the peer too)")
    command("erase-pair", help="erase the peer pairing record", description="erase the peer pairing record")
    phrase = command("set-phrase", help="derive the root key from a binding phrase", description="derive the root key from a binding phrase")
    phrase.add_argument("phrase", help="1..32 UTF-8 bytes; use the same phrase on both peers")
    key = command("set-key-hex", help="set a 32-byte root key", description="set a 32-byte root key")
    key.add_argument("hex_key", help="64 hex characters")
    failsafe = command("set-failsafe", help="set all 16 failsafe channel values", description="set all 16 failsafe channel values")
    failsafe.add_argument("channels", help="16 channel values, or @default")
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
    mode = command("set-mode", help="select mixed or exclusive 2.4 GHz data mode (reboot required)",
                   description="Save the operating mode. Reboot both peers to apply; data24 pairs on 2.4 GHz.")
    mode.add_argument("mode", choices=["mixed", "data24"],
                      help="mixed=control and data; data24=data and pairing exclusively on 2.4 GHz")
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

    if args.self_test:
        return self_test()

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
                heading(f"Device: {port}")
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
    elif args.command == "set-failsafe":
        try:
            channels = parse_channels(args.channels)
        except ValueError as exc:
            parser.error(str(exc))
        frame = encode(CMD_SET_FAILSAFE, channels_to_payload(channels))
        expect = True
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
    elif args.command == "set-mode":
        frame = encode(CMD_SET_MODE, bytes([1 if args.mode == "data24" else 0]))
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
            heading(f"Device: {port}")
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
