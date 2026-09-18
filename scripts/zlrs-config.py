#!/usr/bin/env python3
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# zlrs 配置工具：在配置模式下通过数据口和设备说话。
#
#   scripts/zlrs-config.py --port /dev/ttyACM1 status
#   scripts/zlrs-config.py --port /dev/ttyACM1 set-phrase "correct horse battery staple"
#   scripts/zlrs-config.py --self-test          # 不接设备，校验组帧与 C 侧逐字节一致
#
# 设备进配置模式的办法：按住按键上电（见 doc/08-firmware.md 第 1 节）。
# 数据口二选一：主机把 USB 数据 CDC 打开（DTR）就走 USB，否则走 UART1。
# USB 三个 CDC 依次是 CRSF、数据口、控制台（/dev/ttyACM0/1/2）。
# 帧格式与命令表见 doc/10-security.md 第 10 节、include/zlrs/config/config.h。
#
# 依赖：只用标准库；真的要连设备时需要 pyserial（pip install pyserial）。

import argparse
import sys

SOF = b"\xa5\x5a"
VERSION = 1

CMD_STATUS = 0x01
CMD_PAIR = 0x02
CMD_SET_KEY_HEX = 0x10
CMD_SET_PHRASE = 0x11
CMD_SET_FAILSAFE = 0x20
CMD_SET_BAUD = 0x21
CMD_EXIT = 0x30
ACK = 0x80
STATUS_RESP = 0x81

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
    """DVB-S2 多项式 0xD5，与设备侧 zlrs_crc8() 同一个算法。"""
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
    """返回 (cmd, payload)；帧不完整或 CRC 不对时抛 ValueError。"""
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
    """通道值（11 bit）→ 22 字节载荷，与 CRSF 的通道帧同格式。"""
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
    """'172,992,...' 或 '@min' 之类的简写。"""
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
    if len(payload) < 16:
        return f"status payload too short: {payload.hex()}"
    flags = payload[3]
    dev = payload[4:8].hex()
    peer = payload[8:12].hex()
    return (
        f"role={'TX' if payload[1] == 0 else 'RX'} "
        f"subg={'HF (868/915)' if payload[13] else 'LF (470/433)'} "
        f"state={payload[2]} "
        f"key={'yes' if flags & 0x01 else 'no'} "
        f"paired={'yes' if flags & 0x02 else 'no'} "
        f"session={'yes' if flags & 0x04 else 'no'} "
        f"dev_id={dev} peer_id={peer} lq={payload[12]}%"
    )


def self_test() -> int:
    """组帧结果必须与 C 侧 zlrs_cfg_encode() 的输出逐字节一致。"""
    cases = [
        ("status", CMD_STATUS, b"", "a55a01010088"),
        ("pair", CMD_PAIR, b"", "a55a01020095"),
        (
            "set-phrase",
            CMD_SET_PHRASE,
            b"hello",
            "a55a01110568656c6c6f71",
        ),
        (
            "set-baud",
            CMD_SET_BAUD,
            bytes([0x00, 0x25, 0x06, 0x00]),
            "a55a01210400250600ab",
        ),
        ("exit", CMD_EXIT, b"", "a55a01300086"),
    ]

    failures = 0
    for name, cmd, payload, want in cases:
        got = encode(cmd, payload).hex()
        if got != want:
            print(f"  FAIL {name}: got {got} want {want}")
            failures += 1

    # 单向自检：把上面那帧解回来
    cmd, payload = decode(bytes.fromhex("a55a01110568656c6c6f71"))
    if cmd != CMD_SET_PHRASE or payload != b"hello":
        print("  FAIL decode round trip")
        failures += 1

    # 通道载荷：全 0 应是 22 个 0 字节；默认值给出油门最低 + 其余中位
    if channels_to_payload([0] * 16) != bytes(22):
        print("  FAIL channels all zero")
        failures += 1
    default = channels_to_payload(parse_channels("@default"))
    if len(default) != 22 or default[0] != 0xAC:
        print("  FAIL channels default")
        failures += 1

    print(f"self-test: {'ok' if failures == 0 else str(failures) + ' failure(s)'}")
    return 0 if failures == 0 else 1


def talk(port: str, baud: int, frame: bytes, expect: bool, timeout: float) -> int:
    try:
        import serial  # pyserial
    except ImportError:
        print("需要 pyserial：pip install pyserial", file=sys.stderr)
        return 2

    with serial.Serial(port, baudrate=baud, timeout=timeout) as link:
        link.write(frame)
        if not expect:
            return 0

        # 读一帧：先找 SOF，再按长度字段收满
        buf = bytearray()
        while True:
            byte = link.read(1)
            if not byte:
                print("等响应超时", file=sys.stderr)
                return 1
            buf += byte
            if len(buf) == 1 and buf[0] != 0xA5:
                buf.clear()
            elif len(buf) >= 2 and buf[1] != 0x5A:
                buf.clear()
            elif len(buf) >= 5 and len(buf) == buf[4] + 6:
                break

        cmd, payload = decode(bytes(buf))
        if cmd == ACK:
            code = payload[1] if len(payload) >= 2 else 0xFF
            print(f"ack: {STATUS_NAMES.get(code, hex(code))}")
            return 0 if code == 0 else 1
        if cmd == STATUS_RESP:
            print(status_text(payload))
            return 0
        print(f"unexpected response 0x{cmd:02x}: {payload.hex()}")
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="zlrs 配置工具")
    parser.add_argument("--port", help="数据口设备（进配置模式后）")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=1.0)
    parser.add_argument("--dry-run", action="store_true", help="只打印帧，不连设备")
    parser.add_argument("--self-test", action="store_true", help="校验组帧后退出")

    sub = parser.add_subparsers(dest="command")
    sub.add_parser("status")
    sub.add_parser("pair")
    phrase = sub.add_parser("set-phrase")
    phrase.add_argument("phrase")
    key = sub.add_parser("set-key-hex")
    key.add_argument("hex_key", help="64 个十六进制字符")
    failsafe = sub.add_parser("set-failsafe")
    failsafe.add_argument("channels", help="16 个通道值，或 @default")
    baud = sub.add_parser("set-baud")
    baud.add_argument("baud", type=int)
    sub.add_parser("exit")

    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if args.command is None:
        parser.print_help()
        return 2

    if args.command == "status":
        frame, expect = encode(CMD_STATUS), True
    elif args.command == "pair":
        frame, expect = encode(CMD_PAIR), True
    elif args.command == "set-phrase":
        frame, expect = encode(CMD_SET_PHRASE, args.phrase.encode()), True
    elif args.command == "set-key-hex":
        key_bytes = bytes.fromhex(args.hex_key)
        if len(key_bytes) != 32:
            print("根密钥必须是 64 个十六进制字符（32 字节）", file=sys.stderr)
            return 2
        frame, expect = encode(CMD_SET_KEY_HEX, args.hex_key.encode()), True
    elif args.command == "set-failsafe":
        frame = encode(CMD_SET_FAILSAFE, channels_to_payload(parse_channels(args.channels)))
        expect = True
    elif args.command == "set-baud":
        frame = encode(CMD_SET_BAUD, args.baud.to_bytes(4, "little"))
        expect = True
    else:  # exit
        frame, expect = encode(CMD_EXIT), True

    if args.dry_run:
        print(frame.hex())
        return 0

    if not args.port:
        print("要连设备得给 --port；只想看帧就用 --dry-run", file=sys.stderr)
        return 2

    return talk(args.port, args.baud, frame, expect, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
