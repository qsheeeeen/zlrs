#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 上板自检：一条命令把"不依赖射频"的那一档验一遍（roadmap M1/M3 里能在板子上
# 独立跑的部分）。不做射频相关的事——射频那档要等 Core2021 接上。
#
# 验什么：
#   1. 应用模式的三块 CDC 都在（CRSF / 数据口 / 控制台，按 by-id 名字认）
#   2. 数据口魔数 `zlrs!configmode!` 能把应用切进配置模式
#   3. 配置协议：status（读状态）→ set-phrase（写短语，PBKDF2 落 NVS）→
#      set-failsafe → status（确认 key 位变 yes）
#   4. EXIT 回正常模式
#   5. 再进一次配置模式，status 里 key 仍是 yes —— NVS 持久化
#
# 用法：
#   python3 scripts/zlrs-bringup.py                # 自动按 by-id 找端口
#   python3 scripts/zlrs-bringup.py --data /dev/ttyACM1 --console /dev/ttyACM2
#   python3 scripts/zlrs-bringup.py --self-test    # 不接设备，校验逻辑
#
# 需要设备侧打开 CONFIG_ZLRS_APP_CONFIG_MAGIC（板上没有用户按键时的入口）。

import argparse
import glob
import subprocess
import sys
import time
from pathlib import Path

BY_ID_DIR = "/dev/serial/by-id"
APP_HINT = "ZLRS"
MCUBOOT_HINT = "MCUBOOT"
CONFIG_MAGIC = b"zlrs!configmode!"
DEFAULT_BAUD = 115200


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def find_by_id(hint: str, exclude: str | None = None) -> list[str]:
    out = []
    for path in sorted(glob.glob(f"{BY_ID_DIR}/*")):
        name = Path(path).name.upper()
        if hint.upper() not in name:
            continue
        if exclude and exclude.upper() in name:
            continue
        out.append(path)
    return out


def app_ports() -> list[str]:
    return find_by_id(APP_HINT, exclude=MCUBOOT_HINT)


def pick_port(ports: list[str], iface: str) -> str | None:
    """三块 CDC 按 USB 接口号认：if00 CRSF、if02 数据口、if04 控制台。"""
    for path in ports:
        if Path(path).name.endswith(iface):
            return path
    return None


def send_magic(port: str, baud: int = DEFAULT_BAUD) -> int:
    try:
        import serial  # pyserial
    except ImportError:
        print("需要 pyserial：pip install pyserial", file=sys.stderr)
        return 2

    with serial.Serial(port, baudrate=baud, timeout=1) as link:
        link.write(CONFIG_MAGIC)
        link.flush()

    return 0


def run_config(*args: str) -> tuple[int, str]:
    tool = repo_root() / "scripts" / "zlrs-config.py"
    proc = subprocess.run([sys.executable, str(tool), *args], check=False,
                          capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def enter_config_mode(data_port: str, tries: int = 8) -> bool:
    """发魔数 + 用 status 探测，直到配置模式应答。"""
    for attempt in range(1, tries + 1):
        if send_magic(data_port) != 0:
            return False
        time.sleep(0.3)
        rc, out = run_config("--port", data_port, "status")
        if rc == 0 and "role=" in out:
            print(f"    配置模式已进入（第 {attempt} 次尝试）")
            print(f"    {out}")
            return True
    return False


def self_test() -> int:
    problems = []

    if len(CONFIG_MAGIC) != 16:
        problems.append(f"魔数必须是 16 字节：{CONFIG_MAGIC!r}")

    fake = [
        "/dev/serial/by-id/usb-zlrs_ZLRS_1234-if00",
        "/dev/serial/by-id/usb-zlrs_ZLRS_1234-if02",
        "/dev/serial/by-id/usb-zlrs_ZLRS_1234-if04",
        "/dev/serial/by-id/usb-zlrs_ZLRS_MCUBOOT_1234-if00",
    ]
    original = glob.glob
    glob.glob = lambda pattern: list(fake)  # type: ignore[assignment]
    try:
        ports = app_ports()
        if len(ports) != 3:
            problems.append(f"应用模式端口识别错：{ports}")
        if pick_port(ports, "-if02") != fake[1]:
            problems.append("数据口认错")
        if pick_port(ports, "-if04") != fake[2]:
            problems.append("控制台认错")
    finally:
        glob.glob = original  # type: ignore[assignment]

    # 应用侧 C 代码里的魔数必须一致（改了要同步改 app/src/main.c）
    with open(repo_root() / "app" / "src" / "main.c", "rb") as source:
        if CONFIG_MAGIC not in source.read():
            problems.append("app/src/main.c 里没有同一个魔数")

    for line in problems:
        print(f"  FAIL {line}")
    print(f"self-test: {'ok' if not problems else str(len(problems)) + ' failure(s)'}")
    return 0 if not problems else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="上板自检（非射频那一档）")
    parser.add_argument("--data", help="数据口设备（默认按 by-id 找 ...-if02）")
    parser.add_argument("--console", help="控制台设备（默认按 by-id 找 ...-if04）")
    parser.add_argument("--phrase", default="zlrs bring-up phrase",
                        help="set-phrase 用的短语")
    parser.add_argument("--self-test", action="store_true", help="不接设备，校验逻辑")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    ports = app_ports()
    if not ports:
        print(f"没找到应用端口（{BY_ID_DIR} 里没有 {APP_HINT}）——板子插好了吗？",
              file=sys.stderr)
        return 1

    data = args.data or pick_port(ports, "-if02")
    console = args.console or pick_port(ports, "-if04")
    if data is None:
        print(f"没找到数据口（...-if02）：{ports}", file=sys.stderr)
        return 1

    print("1) 端口")
    for p in ports:
        print(f"    {p}")
    if console is None:
        print("    警告：没找到控制台口（...-if04），继续")

    print("2) 数据口魔数进配置模式")
    if not enter_config_mode(data):
        print("    进不去。检查 CONFIG_ZLRS_APP_CONFIG_MAGIC 是否编进去了，"
              "以及数据口是不是被别的程序占着", file=sys.stderr)
        return 1

    print("3) 配置协议")
    rc, out = run_config("--port", data, "set-phrase", args.phrase)
    print(f"    set-phrase: rc={rc} {out}")
    if rc != 0:
        return 1

    rc, out = run_config("--port", data, "set-failsafe", "@default")
    print(f"    set-failsafe: rc={rc} {out}")
    if rc != 0:
        return 1

    rc, out = run_config("--port", data, "status")
    print(f"    status: {out}")
    if rc != 0 or "key=yes" not in out:
        print("    写完短语之后 key 位还是 no", file=sys.stderr)
        return 1

    rc, out = run_config("--port", data, "exit")
    print(f"    exit: rc={rc} {out}")
    time.sleep(0.3)

    print("4) NVS 持久化（再进一次配置模式）")
    if not enter_config_mode(data):
        print("    第二次进配置模式失败", file=sys.stderr)
        return 1

    rc, out = run_config("--port", data, "status")
    print(f"    status: {out}")
    if rc != 0 or "key=yes" not in out:
        print("    重启后 key 位不是 yes：NVS 没保住", file=sys.stderr)
        return 1

    run_config("--port", data, "exit")

    print("自检通过：端口、配置模式（魔数入口）、配置协议、NVS 持久化都对")
    return 0


if __name__ == "__main__":
    sys.exit(main())
