#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 让设备进 MCUboot 的串口恢复模式，不用拔插 USB、不用按复位键。
#
# 原理：配置工具在配置口上发"进恢复"命令（配置协议 0x41），设备置 boot mode 并
# 重启；MCUboot 读到标志后停在串口恢复里等镜像上传。见 doc/10-security.md 第 10 节。
#
# 用法：
#   python3 scripts/zlrs-recovery.py --port /dev/ttyACM2   # 配置口（应用模式第三块 CDC）
#   python3 scripts/zlrs-recovery.py --self-test           # 不接设备，校验组帧
#
# 之后用 mcumgr 客户端上传签名镜像：
#   smpmgr --port <恢复口> upgrade zephyr.signed.bin
#
# 端口会变：应用模式三块 CDC（配置口是 ttyACM2），bootloader 模式只有两块
# （恢复口在前、控制台在后）。按 /dev/serial/by-id/...MCUBOOT...-if00 找恢复口。

import argparse
import sys

SOF = b"\xa5\x5a"
VERSION = 2
CMD_RECOVERY = 0x41
DEFAULT_BAUD = 115200


def crc8(data: bytes) -> int:
    """DVB-S2 多项式 0xD5，与设备侧 zlrs_crc8() 同一个算法。"""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def build_request() -> bytes:
    """配置协议的"进恢复"帧：SOF + ver + cmd + len + crc。"""
    body = bytes([VERSION, CMD_RECOVERY, 0])

    return SOF + body + bytes([crc8(body)])


def self_test() -> int:
    request = build_request()
    want = bytes.fromhex("a55a02410067")

    if request != want:
        print(f"self-test: FAIL 组帧不对：{request.hex()} != {want.hex()}", file=sys.stderr)
        return 1
    if request[3] != CMD_RECOVERY:
        print("self-test: FAIL 命令号不对", file=sys.stderr)
        return 1

    print("self-test: ok（配置协议进恢复帧，与 C 侧一致）")
    return 0


def send_recovery(port: str, baud: int, dry_run: bool) -> int:
    request = build_request()

    if dry_run:
        print(f"[dry-run] 会向 {port}@{baud} 写 {request.hex()}")
        return 0

    try:
        import serial  # pyserial
    except ImportError:
        print("需要 pyserial：pip install pyserial", file=sys.stderr)
        return 2

    try:
        with serial.Serial(port, baudrate=baud, timeout=1) as link:
            link.write(request)
            link.flush()
    except Exception as exc:  # noqa: BLE001
        print(f"写 {port} 失败：{exc}", file=sys.stderr)
        return 1

    print(f"已发送进恢复命令到 {port}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="让设备进 MCUboot 串口恢复")
    parser.add_argument("--port", help="配置口（应用模式第三块 CDC，如 /dev/ttyACM2）")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--dry-run", action="store_true", help="只打印要发的帧")
    parser.add_argument("--self-test", action="store_true", help="不接设备，校验组帧")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.port:
        parser.error("要连设备得给 --port")

    return send_recovery(args.port, args.baud, args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
