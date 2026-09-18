#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 上板自检：一条命令验完"不依赖射频"那一档：
# 三块 CDC → 配置口进配置模式 → 配置协议读写（含 PBKDF2 落 NVS）→
# 退出后再进一次确认 NVS 持久化。射频相关的一律不碰。
#
# 用法：
#   python3 scripts/zlrs-bringup.py                # 自动按 by-id 找端口
#   python3 scripts/zlrs-bringup.py --config /dev/ttyACM2
#   python3 scripts/zlrs-bringup.py --self-test    # 不接设备，校验逻辑
#
# 配置口是应用模式的第三块 CDC（...-if04）。

import argparse
import glob
import subprocess
import sys
import time
from pathlib import Path

BY_ID_DIR = "/dev/serial/by-id"
APP_HINT = "ZLRS"
MCUBOOT_HINT = "MCUBOOT"
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
    """CDC 按 USB 接口号认：if00 CRSF、if02 数据口、if04 配置口、if06 控制台（调试）。"""
    for path in ports:
        if Path(path).name.endswith(iface):
            return path
    return None


def run_config(*args: str) -> tuple[int, str]:
    """
    跑一条配置命令。超时给足：写短语那条要在设备侧做 100000 轮 PBKDF2，
    M33 上十几秒起步；默认 1 秒一定会误判成"设备不应答"。
    """
    tool = repo_root() / "scripts" / "zlrs-config.py"
    proc = subprocess.run([sys.executable, str(tool), "--timeout", "120", *args], check=False,
                          capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def enter_config_mode(data_port: str, tries: int = 8) -> bool:
    """用配置工具探一次：工具会先发"进配置模式"，设备应答就说明在配置模式里。"""
    for attempt in range(1, tries + 1):
        rc, out = run_config("--port", data_port, "status")
        if rc == 0 and "role=" in out:
            print(f"    配置模式已进入（第 {attempt} 次尝试）")
            print(f"    {out}")
            return True
        time.sleep(0.5)
    return False


def self_test() -> int:
    problems = []

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
            problems.append("配置口认错")
    finally:
        glob.glob = original  # type: ignore[assignment]

    # 配置工具必须支持正式入口"进配置模式"
    tool = repo_root() / "scripts" / "zlrs-config.py"
    if "CMD_ENTER" not in tool.read_text(encoding="utf-8"):
        problems.append("配置工具没有 CMD_ENTER")

    for line in problems:
        print(f"  FAIL {line}")
    print(f"self-test: {'ok' if not problems else str(len(problems)) + ' failure(s)'}")
    return 0 if not problems else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="上板自检（非射频那一档）")
    parser.add_argument("--data", help="数据口设备（默认按 by-id 找 ...-if02）")
    parser.add_argument("--config", help="配置口设备（默认按 by-id 找 ...-if04）")
    parser.add_argument("--console", help="调试构建的控制台口（...-if06）")
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
    config = args.config or pick_port(ports, "-if04")
    console = args.console or pick_port(ports, "-if06")
    if data is None:
        print(f"没找到数据口（...-if02）：{ports}", file=sys.stderr)
        return 1
    if config is None:
        print(f"没找到配置口（...-if04）：{ports}", file=sys.stderr)
        return 1

    print("1) 端口")
    for p in ports:
        print(f"    {p}")
    if console is None:
        print("    警告：没找到控制台口（...-if04），继续")

    print("2) 配置口进配置模式")
    if not enter_config_mode(config):
        print("    进不去。检查配置口是不是被别的程序占着，"
              "以及固件是不是带配置协议", file=sys.stderr)
        return 1

    print("3) 配置协议")
    rc, out = run_config("--port", config, "set-phrase", args.phrase)
    print(f"    set-phrase: rc={rc} {out}")
    if rc != 0:
        return 1

    rc, out = run_config("--port", config, "set-failsafe", "@default")
    print(f"    set-failsafe: rc={rc} {out}")
    if rc != 0:
        return 1

    rc, out = run_config("--port", config, "status")
    print(f"    status: {out}")
    if rc != 0 or "key=yes" not in out:
        print("    写完短语之后 key 位还是 no", file=sys.stderr)
        return 1

    rc, out = run_config("--port", config, "exit")
    print(f"    exit: rc={rc} {out}")
    time.sleep(0.3)

    print("4) NVS 持久化（再进一次配置模式）")
    if not enter_config_mode(config):
        print("    第二次进配置模式失败", file=sys.stderr)
        return 1

    rc, out = run_config("--port", config, "status")
    print(f"    status: {out}")
    if rc != 0 or "key=yes" not in out:
        print("    重启后 key 位不是 yes：NVS 没保住", file=sys.stderr)
        return 1

    run_config("--port", data, "exit")

    print("自检通过：端口、配置模式（配置口命令）、配置协议、NVS 持久化都对")
    return 0


if __name__ == "__main__":
    sys.exit(main())
