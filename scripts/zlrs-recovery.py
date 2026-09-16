#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 让设备进 MCUboot 的串口恢复模式，不用拔插 USB、不用按复位键。
#
# 原理：应用在数据口上监听一个 16 字节魔数（CONFIG_ZLRS_APP_UPDATE_TRIGGER），
# 收到就置 boot mode 并重启；MCUboot 读到标志后停在串口恢复里等镜像上传。
# 见 doc/01-system.md 2.4。
#
# 用法：
#   python3 scripts/zlrs-recovery.py --port /dev/ttyACM1     # USB 数据 CDC
#   python3 scripts/zlrs-recovery.py --port /dev/ttyUSB0     # 数据口 UART（GP8/GP9）
#   python3 scripts/zlrs-recovery.py --self-test             # 不接设备，校验组帧
#
# 之后用 mcumgr 客户端上传签名镜像（工具与参数以你的客户端版本为准）：
#   smpmgr -t serial -p <port> image upload zephyr.signed.bin
#   mcumgr --conntype serial --connstring <port> image upload -e zephyr.signed.bin
#
# 注意端口会变：应用模式三块 CDC（数据口是 ttyACM1），bootloader 模式只有两块
# （恢复口在前、控制台在后）。按 /dev/serial/by-id/...MCUBOOT...-if00 找恢复口，
# 别沿用触发时用的那个端口号。

import argparse
import sys

MAGIC = b"zlrs!bootloader!"
DEFAULT_BAUD = 115200


def build_request() -> bytes:
    """要写到数据口的字节：就是魔数本身（应用侧按字节流匹配）。"""
    return MAGIC


def self_test() -> int:
    """不接设备也能验的：魔数长度、内容、以及"不会被误判成文本"的基本性质。"""
    request = build_request()

    assert request == MAGIC
    assert len(request) == 16, f"魔数必须是 16 字节，当前 {len(request)}"
    assert request.isascii()
    # 应用侧 C 代码里的字符串必须和这里一致（改了要同步改 app/src/main.c）
    with open("app/src/main.c", "rb") as source:
        text = source.read()
    if MAGIC not in text:
        print("self-test: FAIL  app/src/main.c 里没有同样的魔数", file=sys.stderr)
        return 1

    print(f"self-test: ok（{len(request)} 字节魔数，与应用侧一致）")
    return 0


def send_magic(port: str, baud: int, dry_run: bool) -> int:
    if dry_run:
        print(f"[dry-run] 会向 {port}@{baud} 写 {build_request()!r}")
        return 0

    try:
        import serial  # pyserial，只在连设备时需要
    except ImportError:
        print("需要 pyserial：pip install pyserial", file=sys.stderr)
        return 1

    with serial.Serial(port, baud, timeout=1) as dev:
        dev.write(build_request())
        dev.flush()

    print(f"已发送升级触发（{port}@{baud}）：设备会重启并停在 MCUboot 串口恢复")
    print("接下来上传签名镜像：bootloader 模式只剩两块 CDC（恢复口在前、控制台在后），")
    print("按 /dev/serial/by-id/...MCUBOOT... 找恢复口，别沿用刚才这个端口号")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="让设备进 MCUboot 串口恢复模式")
    parser.add_argument("--port", help="数据口：/dev/ttyACM1（USB CDC）或 /dev/ttyUSB0（UART）")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"波特率（默认 {DEFAULT_BAUD}）")
    parser.add_argument("--dry-run", action="store_true", help="只打印要发的字节，不连设备")
    parser.add_argument("--self-test", action="store_true", help="不接设备，校验组帧")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.port is None:
        parser.error("需要 --port（或用 --self-test）")

    return send_magic(args.port, args.baud, args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
