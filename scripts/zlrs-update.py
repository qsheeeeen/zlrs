#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 一条命令升级一台健康设备：触发进恢复 → 等 bootloader 的恢复口 → 上传签名镜像 →
# 等应用回来。省掉"拔插 USB / 按复位键"和"手动两步"。
#
# 流程（见 doc/01-system.md 2.4）：
#   1. 往应用模式的数据口发 16 字节魔数（复用 scripts/zlrs-recovery.py），
#      应用置 boot mode 后重启；
#   2. MCUboot 停在串口恢复，USB 重新枚举成 "ZLRS MCUBOOT"（两块 CDC：
#      第 1 块恢复、第 2 块日志），脚本按 /dev/serial/by-id 里的名字找恢复口；
#   3. 用 smpmgr（Python 的 SMP/mcumgr 客户端）upgrade：上传 + 标记下次启动 +
#      重启，MCUboot 换槽；
#   4. 等 "ZLRS" 的应用模式端口回来，报告结果。
#
# 用法：
#   python3 scripts/zlrs-update.py --port /dev/ttyACM1            # 镜像自动找
#   python3 scripts/zlrs-update.py --port /dev/ttyACM1 --image build/app/zephyr/zephyr.signed.bin
#   python3 scripts/zlrs-update.py --recovery-port /dev/ttyACM0 --no-trigger   # 已经在恢复模式
#   python3 scripts/zlrs-update.py --self-test                    # 不接设备
#
# 依赖：pyserial（触发那一步）与 smpmgr（上传；pip install smpmgr）。
# 不想让脚本自己上传时用 --client none，它会打印要执行的命令。

import argparse
import glob
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_BAUD = 115200
APP_PORT_HINT = "ZLRS"
RECOVERY_PORT_HINT = "MCUBOOT"
BY_ID_DIR = "/dev/serial/by-id"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def workspace_root() -> Path:
    return Path(os.environ.get("ZLRS_WS", repo_root().parent))


def default_image() -> Path:
    """sysbuild 构建出来的签名镜像（app 镜像目录名就是 app/）。"""
    return workspace_root() / "build" / "app" / "zephyr" / "zephyr.signed.bin"


def find_by_id(hint: str, exclude: str | None = None) -> list[str]:
    """按 /dev/serial/by-id 里的名字找串口（大小写不敏感）。"""
    found = []
    for path in sorted(glob.glob(f"{BY_ID_DIR}/*")):
        name = os.path.basename(path).upper()
        if hint.upper() not in name:
            continue
        if exclude is not None and exclude.upper() in name:
            continue
        found.append(path)
    return found


def wait_for_by_id(hint: str, timeout_s: float, exclude: str | None = None) -> str | None:
    """等一个名字里带 hint 的串口出现（USB 重新枚举要时间）。"""
    deadline = time.monotonic() + timeout_s
    while True:
        found = find_by_id(hint, exclude)
        if found:
            return found[0]
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.2)


def find_client(name: str = "smpmgr") -> str | None:
    """先在 PATH 里找，再找 west 工作区 venv 里的（setup-zephyr.sh 装在那里）。"""
    found = shutil.which(name)
    if found:
        return found

    venv = workspace_root() / ".venv" / "bin" / name
    if venv.is_file():
        return str(venv)

    return None


def upload_command(client: str, port: str, image: Path, baud: int,
                   confirm: bool = False) -> list[str]:
    """
    smpmgr 的 upgrade = 上传 + 标记下次启动 + 重启（0.19 起）。

    confirm=False（默认）走 MCUboot 的 test 语义：换槽进新镜像，没确认就回滚——
    有回滚兜底，但要走一次换槽（这块板上 `swap using offset` 还没验证通过，见
    doc/08-roadmap.md 的 M1）。
    confirm=True 直接写 slot0 并标记为永久：不换槽、没有回滚，上板调试时用来
    绕开换槽这条路。
    """
    cmd = [client, "--port", port, "--baudrate", str(baud), "upgrade"]
    if confirm:
        cmd.append("--confirm")
    cmd.append(str(image))

    return cmd


def run_trigger(port: str, baud: int) -> int:
    script = repo_root() / "scripts" / "zlrs-recovery.py"
    print(f"[1/4] 触发进恢复模式：{port}")
    return subprocess.run(
        [sys.executable, str(script), "--port", port, "--baud", str(baud)], check=False
    ).returncode


def self_test() -> int:
    """不接设备也能验的：镜像路径、端口匹配、客户端命令拼装。"""
    problems = []

    # 端口匹配：应用模式的端口不能把 bootloader 的端口抢过去
    fake = [
        "/dev/serial/by-id/usb-zlrs_ZLRS_1234-if01",
        "/dev/serial/by-id/usb-zlrs_ZLRS_MCUBOOT_1234-if01",
    ]
    original = glob.glob
    glob.glob = lambda pattern: list(fake)  # type: ignore[assignment]
    try:
        app_ports = find_by_id(APP_PORT_HINT, exclude=RECOVERY_PORT_HINT)
        rec_ports = find_by_id(RECOVERY_PORT_HINT)
        if app_ports != [fake[0]]:
            problems.append(f"应用口匹配错：{app_ports}")
        if rec_ports != [fake[1]]:
            problems.append(f"恢复口匹配错：{rec_ports}")
    finally:
        glob.glob = original  # type: ignore[assignment]

    # 上传命令拼装
    cmd = upload_command("smpmgr", "/dev/ttyACM0", Path("zephyr.signed.bin"), DEFAULT_BAUD)
    if cmd[0] != "smpmgr" or "upgrade" not in cmd or cmd[-1] != "zephyr.signed.bin":
        problems.append(f"上传命令不对：{cmd}")
    if "--confirm" in cmd:
        problems.append(f"默认上传不该带 --confirm：{cmd}")

    cmd_confirm = upload_command("smpmgr", "/dev/ttyACM0", Path("zephyr.signed.bin"),
                                 DEFAULT_BAUD, confirm=True)
    if "--confirm" not in cmd_confirm or cmd_confirm[-1] != "zephyr.signed.bin":
        problems.append(f"--confirm 上传命令不对：{cmd_confirm}")

    # 触发那一步必须存在，且魔数与应用侧一致（交给它自己的 self-test 查）
    recovery = repo_root() / "scripts" / "zlrs-recovery.py"
    if not recovery.is_file():
        problems.append(f"缺少 {recovery}")
    else:
        result = subprocess.run(
            [sys.executable, str(recovery), "--self-test"], capture_output=True, check=False
        )
        if result.returncode != 0:
            problems.append("zlrs-recovery.py --self-test 失败")

    if problems:
        print("self-test: FAIL")
        for problem in problems:
            print(f"  - {problem}")
        return 1

    print("self-test: ok（端口匹配、上传命令、触发脚本都对得上）")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="触发恢复并升级设备上的固件")
    parser.add_argument("--port", help="应用模式的数据口（触发用），如 /dev/ttyACM1")
    parser.add_argument("--recovery-port", help="bootloader 的恢复口；给了就跳过查找")
    parser.add_argument("--image", help="签名镜像路径（默认找 <workspace>/build/app/zephyr/zephyr.signed.bin）")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"串口波特率（默认 {DEFAULT_BAUD}）")
    parser.add_argument("--client", choices=["auto", "smpmgr", "none"], default="auto",
                        help="上传客户端：auto = 找 smpmgr，none = 只打印命令")
    parser.add_argument("--no-trigger", action="store_true", help="设备已经在恢复模式，不触发")
    parser.add_argument("--no-wait", action="store_true", help="不等应用模式端口回来")
    parser.add_argument("--timeout", type=float, default=15.0, help="等端口出现的时间（秒，默认 15）")
    parser.add_argument("--confirm", action="store_true",
                        help="直接写 slot0 并标记永久（不换槽、没有回滚；绕开还没验证的换槽路径）")
    parser.add_argument("--self-test", action="store_true", help="不接设备，校验路径与命令")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    image = Path(args.image) if args.image else default_image()
    if not image.is_file():
        print(f"找不到签名镜像 {image}\n先跑：zlrs-build-mcuboot，或用 --image 指明路径", file=sys.stderr)
        return 1

    # 1. 触发
    if not args.no_trigger:
        if args.port is None:
            ports = find_by_id(APP_PORT_HINT, exclude=RECOVERY_PORT_HINT)
            if not ports:
                parser.error("没找到应用模式的数据口（/dev/serial/by-id 里没有 ZLRS）——用 --port 指明")
            args.port = ports[0]
        if run_trigger(args.port, args.baud) != 0:
            print("触发失败：设备没有回应数据口", file=sys.stderr)
            return 1
    else:
        print("[1/4] 跳过触发（--no-trigger）")

    # 2. 等 bootloader 的恢复口
    if args.recovery_port is None:
        print(f"[2/4] 等 bootloader 的恢复口（名字里有 {RECOVERY_PORT_HINT}，最多 {args.timeout:g} s）")
        args.recovery_port = wait_for_by_id(RECOVERY_PORT_HINT, args.timeout)
        if args.recovery_port is None:
            print("没等到恢复口。设备起来了但没进恢复？检查 boot mode 那块标志（见 doc/01-system.md 2.4），"
                  "或用 --recovery-port 指明端口", file=sys.stderr)
            return 1
    print(f"    恢复口：{args.recovery_port}")

    # 3. 上传
    client = None
    if args.client == "auto":
        client = find_client("smpmgr")
    elif args.client == "smpmgr":
        client = find_client("smpmgr")
        if client is None:
            print("找不到 smpmgr：pip install smpmgr", file=sys.stderr)
            return 1

    cmd = upload_command(client or "smpmgr", args.recovery_port, image, args.baud,
                         confirm=args.confirm)
    if client is None:
        print("[3/4] 请手动上传（没让脚本自己跑）：")
        print("      " + " ".join(cmd))
    else:
        print(f"[3/4] 上传并重启：{image.name}")
        result = subprocess.run(cmd, check=False)
        if result.returncode != 0:
            print("上传失败", file=sys.stderr)
            return result.returncode

    # 4. 等应用回来
    if not args.no_wait and client is not None:
        print(f"[4/4] 等应用模式端口回来（名字里有 {APP_PORT_HINT}、不含 {RECOVERY_PORT_HINT}）")
        port = wait_for_by_id(APP_PORT_HINT, args.timeout, exclude=RECOVERY_PORT_HINT)
        if port is None:
            print("应用端口没回来：看控制台日志（第二个 CDC），或再触发一次恢复重试", file=sys.stderr)
            return 1
        print(f"    应用起来了：{port}")
    else:
        print("[4/4] 跳过等待应用端口")

    print("升级完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
