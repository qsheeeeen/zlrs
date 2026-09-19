#!/usr/bin/env python3
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# zlrs config tool: talks to the device over the dedicated config CDC.
#
#   tools/zlrs-config.py --port /dev/ttyACM2 status
#   tools/zlrs-config.py --port /dev/ttyACM2 set-phrase "correct horse battery staple"
#   tools/zlrs-config.py --self-test          # no device: frame byte-for-byte against the C side
#
# Except for `enter`, every command first sends enter-config-mode and waits for the ACK.
# The three USB CDCs are CRSF, data port and config port (/dev/ttyACM0/1/2).
# Framing and the command table: docs/design/10-security.md 10 and include/zlrs/config/config.h.
#
# Standard library only; talking to a device needs pyserial (pip install pyserial).

import argparse
import sys

SOF = b"\xa5\x5a"
VERSION = 2

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
CMD_EXIT = 0x30
CMD_FACTORY_RESET = 0x40
CMD_RECOVERY = 0x41
CMD_CONFIRM = 0x42
CMD_BUILD = 0x12
CMD_IMAGE_HASH = 0x13
ACK = 0x80
STATUS_RESP = 0x81
BUILD_RESP = 0x82
IMAGE_HASH_RESP = 0x83

STATUS_NAMES = {
    0: "ok",
    1: "bad parameter",
    2: "wrong state",
    3: "storage error",
    4: "unknown command",
}

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
    if len(payload) < 20:
        return f"status payload too short: {payload.hex()}"
    flags = payload[3]
    dev = payload[4:8].hex()
    peer = payload[8:12].hex()
    baud = int.from_bytes(payload[16:20], "little")
    return (
        f"role={'TX' if payload[1] == 0 else 'RX'} "
        f"subg={'HF (868/915)' if payload[13] else 'LF (470/433)'} "
        f"state={payload[2]} "
        f"key={'yes' if flags & 0x01 else 'no'} "
        f"paired={'yes' if flags & 0x02 else 'no'} "
        f"session={'yes' if flags & 0x04 else 'no'} "
        f"dev_id={dev} peer_id={peer} lq={payload[12]}% "
        f"region={payload[14]} power={payload[15] - 256 if payload[15] > 127 else payload[15]}dBm "
        f"baud={baud or 115200}"
    )


def self_test() -> int:
    """Frames must match the output of zlrs_cfg_encode() on the C side byte for byte."""
    cases = [
        ("enter", CMD_ENTER, b"", "a55a020000d3"),
        ("status", CMD_STATUS, b"", "a55a020100d8"),
        ("pair", CMD_PAIR, b"", "a55a020200c5"),
        ("erase-pair", CMD_ERASE_PAIR, b"", "a55a020300ce"),
        ("set-phrase", CMD_SET_PHRASE, b"hello", "a55a02110568656c6c6fb4"),
        ("set-key-hex", CMD_SET_KEY_HEX, b"00" * 32, None),
        ("set-baud", CMD_SET_BAUD, bytes([0x00, 0x25, 0x06, 0x00]), "a55a02210400250600c2"),
        ("set-region", CMD_SET_REGION, bytes([3]), "a55a02220103ca"),
        ("set-power", CMD_SET_POWER, bytes([20]), "a55a022301144f"),
        ("factory-reset", CMD_FACTORY_RESET, b"", "a55a0240006c"),
        ("recovery", CMD_RECOVERY, b"", "a55a02410067"),
        ("build", CMD_BUILD, b"", "a55a02120075"),
        ("image-hash", CMD_IMAGE_HASH, b"", "a55a0213007e"),
        ("confirm", CMD_CONFIRM, b"", "a55a0242007a"),
        ("exit", CMD_EXIT, b"", "a55a023000d6"),
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
    cmd, payload = decode(bytes.fromhex("a55a02110568656c6c6fb4"))
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
        elif len(buf) >= 2 and buf[1] != 0x5A:
            buf.clear()
        elif len(buf) >= 5 and len(buf) == buf[4] + 6:
            return decode(bytes(buf))


def talk(port: str, baud: int, frame: bytes, expect: bool, timeout: float,
         prelude: bytes | None) -> int:
    try:
        import serial  # pyserial
    except ImportError:
        print("pyserial required: pip install pyserial", file=sys.stderr)
        return 2

    with serial.Serial(port, baudrate=baud, timeout=timeout) as link:
        if prelude is not None:
            link.write(prelude)
            try:
                cmd, payload = read_response(link)
            except TimeoutError as exc:
                print(f"entering config mode failed: {exc}", file=sys.stderr)
                return 1

            code = payload[1] if cmd == ACK and len(payload) >= 2 else 0xFF
            if cmd != ACK or code != 0:
                print(f"entering config mode failed: {STATUS_NAMES.get(code, hex(code))}", file=sys.stderr)
                return 1

        link.write(frame)
        if not expect:
            return 0

        try:
            cmd, payload = read_response(link)
        except TimeoutError:
            print("timed out waiting for a response", file=sys.stderr)
            return 1
        if cmd == ACK:
            code = payload[1] if len(payload) >= 2 else 0xFF
            print(f"ack: {STATUS_NAMES.get(code, hex(code))}")
            return 0 if code == 0 else 1
        if cmd == STATUS_RESP:
            print(status_text(payload))
            return 0
        if cmd == BUILD_RESP:
            print(payload.decode(errors="replace"))
            return 0
        if cmd == IMAGE_HASH_RESP:
            print(payload.hex())
            return 0
        print(f"unexpected response 0x{cmd:02x}: {payload.hex()}")
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
        # Each module has several CDCs; the tool only talks to the config port (if04)
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
        hit = [p for p in ports if args.serial in p]

        if not hit:
            parser.error(f"no module with serial {args.serial!r}; present: {ports}")
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
    return ports[0]


def main() -> int:
    parser = argparse.ArgumentParser(description="zlrs config tool")
    parser.add_argument("--port", help="config port device (third CDC in application mode, e.g. /dev/ttyACM2)")
    parser.add_argument("--serial", help="pick a module by USB serial substring (with several plugged in)")
    parser.add_argument("--all", action="store_true",
                        help="run this command on every ZLRS module (with several plugged in)")
    parser.add_argument("--list", action="store_true", help="list the modules present, then exit")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=1.0)
    parser.add_argument("--dry-run", action="store_true", help="print the frame instead of talking to the device")
    parser.add_argument("--no-enter", action="store_true",
                        help="skip the enter-config-mode step (when the device is already in it)")
    parser.add_argument("--keep", action="store_true",
                        help="stay in config mode after the command (the default leaves it, which restores the radio)")
    parser.add_argument("--self-test", action="store_true", help="check framing, then exit")

    sub = parser.add_subparsers(dest="command")
    sub.add_parser("enter", help="enter config mode")
    sub.add_parser("status")
    sub.add_parser("pair")
    sub.add_parser("erase-pair")
    phrase = sub.add_parser("set-phrase")
    phrase.add_argument("phrase")
    key = sub.add_parser("set-key-hex")
    key.add_argument("hex_key", help="64 hex characters")
    failsafe = sub.add_parser("set-failsafe")
    failsafe.add_argument("channels", help="16 channel values, or @default")
    baud = sub.add_parser("set-baud")
    baud.add_argument("baud", type=int)
    region = sub.add_parser("set-region")
    region.add_argument("region", type=int, choices=[0, 1, 2, 3],
                        help="0=FCC915 1=EU868 2=CN470 3=CN433")
    power = sub.add_parser("set-power")
    power.add_argument("dbm", type=int, help="TX power ceiling (dBm)")
    sub.add_parser("factory-reset")
    sub.add_parser("recovery")
    sub.add_parser("build", help="read the build string")
    sub.add_parser("image-hash", help="read the MCUboot hash of the running image (hex)")
    sub.add_parser("confirm", help="confirm this upgrade (no confirm means a timeout rollback)")
    sub.add_parser("exit")

    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if args.list:
        ports = app_ports()

        if not ports:
            print("no ZLRS module plugged in")
            return 1
        for i, path in enumerate(ports):
            print(f"{i}: {path}")
        return 0

    if args.command is None:
        parser.print_help()
        return 2

    if args.command == "enter":
        frame, expect = encode(CMD_ENTER), True
    elif args.command == "status":
        frame, expect = encode(CMD_STATUS), True
    elif args.command == "pair":
        frame, expect = encode(CMD_PAIR), True
    elif args.command == "erase-pair":
        frame, expect = encode(CMD_ERASE_PAIR), True
    elif args.command == "set-phrase":
        frame, expect = encode(CMD_SET_PHRASE, args.phrase.encode()), True
    elif args.command == "set-key-hex":
        key_bytes = bytes.fromhex(args.hex_key)
        if len(key_bytes) != 32:
            print("the root key must be 64 hex characters (32 bytes)", file=sys.stderr)
            return 2
        frame, expect = encode(CMD_SET_KEY_HEX, args.hex_key.encode()), True
    elif args.command == "set-failsafe":
        frame = encode(CMD_SET_FAILSAFE, channels_to_payload(parse_channels(args.channels)))
        expect = True
    elif args.command == "set-baud":
        frame = encode(CMD_SET_BAUD, args.baud.to_bytes(4, "little"))
        expect = True
    elif args.command == "set-region":
        frame = encode(CMD_SET_REGION, bytes([args.region]))
        expect = True
    elif args.command == "set-power":
        frame = encode(CMD_SET_POWER, bytes([args.dbm & 0xFF]))
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
        print(f"enter: {encode(CMD_ENTER).hex()}")
        if args.command != "enter":
            print(f"cmd  : {frame.hex()}")
        return 0

    prelude = None
    if args.command not in ("enter", "recovery") and not args.no_enter:
        prelude = encode(CMD_ENTER)

    # With several modules plugged in, decide the targets first (--serial / --port / --all).
    targets = pick_targets(args, parser)

    rc = 0
    for port in targets:
        if len(targets) > 1:
            print(f"-- {port}")
        this = talk(port, args.baud, frame, expect, args.timeout, prelude)
        if this != 0:
            rc = this
            continue
        if args.command not in ("enter", "exit", "recovery") and not args.keep:
            if talk(port, args.baud, encode(CMD_EXIT), True, args.timeout, None) == 0:
                print("left config mode")

    return rc


if __name__ == "__main__":
    sys.exit(main())
