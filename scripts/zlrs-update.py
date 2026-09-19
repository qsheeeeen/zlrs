#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 一条命令升级一台健康设备：触发进恢复 → 等 bootloader 的恢复口 → 上传签名镜像 →
# 等应用回来。省掉"拔插 USB / 按复位键"和"手动两步"。
#
# 流程（见 doc/08-firmware.md 第 5 节）：
#   1. 往应用模式的配置口发"进恢复"命令（复用 scripts/zlrs-recovery.py），
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

# 应用侧"等上位机确认"的超时（app/Kconfig 的 ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS
# 默认值）。只用于给用户提示，不参与判定。
CONFIG_CONFIRM_TIMEOUT_HINT = "60 s"


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


def upload_command(client: str, port: str, image: Path, baud: int) -> list[str]:
    """
    smpmgr 的 upgrade = 上传 + 标记下次启动 + 重启（0.19 起）。

    写哪个槽由 inactive_slot() 查出来（MCUboot 的串口恢复默认写主槽，也就是直接
    覆盖正在跑的那一版；必须点名非活跃槽才走 A/B），标成 pending（不是永久）。
    换槽后由应用里的确认逻辑决定去留——上位机没在超时内发来确认命令，复位就回滚
    到上一版（见 doc/08-firmware.md 第 6 节）。

    这里**从不**给 smpmgr 传 `--confirm`：那一步是在 bootloader 里把镜像直接标成永久，
    镜像还没跑起来就断了回滚的路。永久标记只由应用在"起来 + 上位机通了"之后自己调
    `boot_write_img_confirmed()`。
    """
    cmd = [client, "--port", port, "--baudrate", str(baud), "upgrade"]
    cmd.append(str(image))

    return cmd


def slot_upload_command(client: str, port: str, image: Path, baud: int,
                        slot: int) -> list[str]:
    """
    smpmgr 的 `--slot` 走的是 MCUboot 的 direct-image 编号：0/1 → slot0，2 → slot1。
    所以物理槽 slot 要传 slot + 1。
    """
    cmd = upload_command(client, port, image, baud)
    cmd.insert(-1, "--slot")
    cmd.insert(-1, str(slot + 1))

    return cmd


def inactive_slot(client: str, port: str, baud: int) -> int | None:
    """
    查 MCUboot 现在跑哪个槽（`image state-read` 里 active 的那个），返回另一个。
    查不到返回 None（调用方兜底用 1：应用链接在 slot0，非活跃的通常是 slot1）。
    """
    result = subprocess.run([client, "--port", port, "--baudrate", str(baud),
                             "image", "state-read"],
                            capture_output=True, text=True, check=False)
    active = None
    slot = None

    for line in f"{result.stdout or ''}{result.stderr or ''}".splitlines():
        line = line.strip()

        if line.startswith("slot="):
            slot = int(line.split("=", 1)[1].strip().rstrip(","))
        elif line.startswith("active=") and line.split("=", 1)[1].strip().rstrip(",") == "True":
            active = slot

    return None if active is None else 1 - active


def config_probe_command(port: str) -> list[str]:
    """
    向新镜像的配置口发一条配置命令（status）；升级成功与否由升级流程自己判断，
    见 main() 里的 [5/6]/[6/6] 两步。
    """
    return [sys.executable, str(repo_root() / "scripts" / "zlrs-config.py"),
            "--port", port, "status"]


def config_subcmd_command(port: str, subcmd: str) -> list[str]:
    return [sys.executable, str(repo_root() / "scripts" / "zlrs-config.py"),
            "--port", port, "--timeout", "3", subcmd]


def run_config_subcmd(port: str, subcmd: str) -> tuple[int, str]:
    """跑一条配置命令，返回 (rc, 输出)。"""
    result = subprocess.run(config_subcmd_command(port, subcmd), capture_output=True,
                            text=True, check=False)
    return result.returncode, f"{result.stdout or ''}{result.stderr or ''}"


def run_config_subcmd_retry(port: str, subcmd: str, attempts: int = 5,
                            delay: float = 2.0) -> tuple[int, str]:
    """
    重试版：新镜像刚起来时主循环可能还没开始服务配置会话（射频初始化还在跑），
    头一两次查询会超时——那是正常的，不是"跑的不是新版"。
    """
    rc, out = 1, ""

    for i in range(attempts):
        rc, out = run_config_subcmd(port, subcmd)
        if rc == 0:
            return rc, out
        if i + 1 < attempts:
            time.sleep(delay)

    return rc, out


def parse_hex_hash(out: str) -> str | None:
    """从命令输出里挑出那一行 64 个十六进制字符（就是镜像哈希）。"""
    for line in out.splitlines():
        line = line.strip()

        if len(line) == 64 and all(c in "0123456789abcdefABCDEF" for c in line):
            return line.lower()

    return None


def image_hash_of(path: Path) -> str | None:
    """
    本地签名镜像里 MCUboot 写的 IMAGE_TLV_SHA256（32 字节）的十六进制。
    设备侧同一条命令回的是运行中镜像的同一个值——两者相等才算"跑的是我刚传的"。
    """
    data = path.read_bytes()

    for off in range(len(data) - 36, 63, -1):
        tag = data[off] | (data[off + 1] << 8)
        length = data[off + 2] | (data[off + 3] << 8)
        if tag == 0x10 and length == 32:
            return data[off + 4:off + 36].hex()

    return None


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
    # 任何一条上传命令都不许把镜像标成永久：那是应用起来之后自己做的事
    if "--confirm" in cmd:
        problems.append(f"上传命令不该带 --confirm（永久标记只由应用做）：{cmd}")
    # 必须点名非活跃槽：MCUboot 的串口恢复默认写主槽（直接盖掉当前版本）
    slotted = slot_upload_command("smpmgr", "/dev/ttyACM0", Path("zephyr.signed.bin"),
                                  DEFAULT_BAUD, 1)
    if "--slot" not in slotted or slotted[slotted.index("--slot") + 1] != "2":
        problems.append(f"上传命令必须点名非活跃槽：{slotted}")

    probe = config_probe_command("/dev/ttyACM0")
    if "status" not in probe or "/dev/ttyACM0" not in probe:
        problems.append(f"上位机确认命令不对：{probe}")

    # 触发那一步必须存在，且组帧与设备侧一致（交给它自己的 self-test 查）
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
    parser.add_argument("--port", help="应用模式的配置口（触发用），如 /dev/ttyACM2")
    parser.add_argument("--recovery-port", help="bootloader 的恢复口；给了就跳过查找")
    parser.add_argument("--image", help="签名镜像路径（默认找 <workspace>/build/app/zephyr/zephyr.signed.bin）")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"串口波特率（默认 {DEFAULT_BAUD}）")
    parser.add_argument("--client", choices=["auto", "smpmgr", "none"], default="auto",
                        help="上传客户端：auto = 找 smpmgr，none = 只打印命令")
    parser.add_argument("--no-trigger", action="store_true", help="设备已经在恢复模式，不触发")
    parser.add_argument("--no-wait", action="store_true", help="不等应用模式端口回来")
    parser.add_argument("--timeout", type=float, default=15.0, help="等端口出现的时间（秒，默认 15）")
    parser.add_argument("--no-confirm-check", action="store_true",
                        help="不上位机确认（默认会向新镜像的配置口发一条 status 命令，成功才算升级成功）")
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
                parser.error("没找到应用模式的配置口（...-if04）——用 --port 指明")
            args.port = next((p for p in ports if p.endswith("-if04")), ports[0])
        if run_trigger(args.port, args.baud) != 0:
            print("触发失败：设备没有回应配置口", file=sys.stderr)
            return 1
    else:
        print("[1/4] 跳过触发（--no-trigger）")

    # 2. 等 bootloader 的恢复口
    if args.recovery_port is None:
        print(f"[2/4] 等 bootloader 的恢复口（名字里有 {RECOVERY_PORT_HINT}，最多 {args.timeout:g} s）")
        args.recovery_port = wait_for_by_id(RECOVERY_PORT_HINT, args.timeout)
        if args.recovery_port is None:
            print("没等到恢复口。设备起来了但没进恢复？检查 boot mode 那块标志（见 doc/08-firmware.md 第 5 节），"
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

    # 不指定槽：MCUboot 自己把镜像写进非活跃槽并标成待试跑
    # 写哪个槽由工具查：MCUboot 的串口恢复默认写主槽（直接盖掉当前版本），
    # 必须点名非活跃槽才走 A/B。
    target_slot = None
    if client is not None:
        target_slot = inactive_slot(client, args.recovery_port, args.baud)
    if target_slot is None:
        target_slot = 1

    cmd = slot_upload_command(client or "smpmgr", args.recovery_port, image, args.baud,
                              target_slot)
    if client is None:
        print("[3/5] 请手动上传（没让脚本自己跑）：")
        print("      " + " ".join(cmd))
    else:
        print(f"[3/5] 上传并重启：{image.name}（写非活跃槽 slot{target_slot}，"
              "标 test，未确认就回滚）")
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        out = f"{result.stdout or ''}{result.stderr or ''}"

        if out.strip():
            print(out.rstrip())

        if result.returncode != 0:
            # ENOTSUP 基本都是 bootloader 没编进镜像状态那一组：没有它，
            # "标成待试跑"这一步做不了，升级会既没换槽也没回滚，必须当失败报出来。
            if "ENOTSUP" in out:
                print("bootloader 不支持写镜像状态（IMAGE_STATE）：升级没法标成 test，"
                      "请用带 CONFIG_BOOT_SERIAL_IMG_GRP_IMAGE_STATE 的 bootloader 重刷",
                      file=sys.stderr)
            else:
                print("上传失败", file=sys.stderr)
            return result.returncode

    # 4. 等应用回来
    app_port = None

    if not args.no_wait and client is not None:
        print(f"[4/6] 等应用模式端口回来（名字里有 {APP_PORT_HINT}、不含 {RECOVERY_PORT_HINT}）")
        port = wait_for_by_id(APP_PORT_HINT, args.timeout, exclude=RECOVERY_PORT_HINT)
        if port is None:
            print("应用端口没回来。升级走 test 语义：设备下次复位会自己回滚到上一版；"
                  "要重试就等它回到旧版再跑一次（见 doc/08-firmware.md 第 6 节）",
                  file=sys.stderr)
            return 1
        app_port = next((p for p in find_by_id(APP_PORT_HINT, exclude=RECOVERY_PORT_HINT)
                         if p.endswith("-if04")), port)
        print(f"    应用起来了：{app_port}")
    else:
        print("[4/6] 跳过等待应用端口")

    if app_port is None:
        print("[5/6] 没拿到应用配置口：无法核对/确认，这一版会被回滚", file=sys.stderr)
        return 1

    # 5. 核对跑的是不是刚传的那一版：比对镜像哈希（版本串可能一样，哈希一定不一样）
    want_hash = image_hash_of(image)
    rc, out = run_config_subcmd_retry(app_port, "image-hash")
    got_hash = parse_hex_hash(out) if rc == 0 else None

    print(f"[5/6] 核对运行的镜像：期望 {want_hash or '?'}，设备报 {got_hash or '?'}")

    if want_hash is None or got_hash != want_hash:
        if not args.no_confirm_check:
            print("跑的不是刚传的那一版：不确认，等设备自己回滚到上一版（"
                  f"{CONFIG_CONFIRM_TIMEOUT_HINT} 之内）", file=sys.stderr)
            return 1
        print("（--no-confirm-check：跳过核对）")

    # 6. 确认：设备收到这条命令才会把这一版标成永久，否则超时回滚
    if args.no_confirm_check:
        print("[6/6] 跳过确认（--no-confirm-check）：这一版不会保留，会回滚")
        return 0

    rc, out = run_config_subcmd_retry(app_port, "confirm")
    if out.strip():
        print(out.rstrip())

    if rc != 0:
        print("确认失败：设备没有接受，这一版会回滚", file=sys.stderr)
        return 1

    rc, out = run_config_subcmd_retry(app_port, "build")
    print(f"    升级成功：镜像已确认{('，固件 ' + out.strip()) if rc == 0 else ''}")
    return 0

    print("升级完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
