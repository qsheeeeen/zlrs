#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 把 MCUboot 构建出来的**签名镜像**打成可拖进 BOOTSEL 盘的 UF2。
#
# 为什么要这一步：Zephyr 自带的 BUILD_OUTPUT_UF2 打的是**未签名**的 zephyr.bin，
# 而且基址是 flash 起始地址——对带 MCUboot 的应用镜像两个都不对（MCUboot 不认
# 未签名的镜像，位置也不在 slot0）。所以这里从 zephyr.signed.bin 重新打一份
# zephyr.signed.uf2，基址取 slot0 的地址（flash 基址 + code 分区偏移）。
#
# 用法：
#   west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/hf/mcuboot app
#   python3 scripts/make-signed-uf2.py            # 默认读 $ZLRS_WS/build
#   python3 scripts/make-signed-uf2.py <build-dir>
#
# 第一次烧写（没有调试器）：
#   1. 按住 BOOTSEL 插 USB，把 <build>/mcuboot/zephyr/zephyr.uf2 拖进 RPI-RP2
#   2. 再把本脚本生成的 <build>/<app>/zephyr/zephyr.signed.uf2 拖进 RPI-RP2
# 之后升级走 MCUboot 串口恢复（数据口 UART1），见 README 与 doc/01-system.md。

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def find_zephyr_base() -> Path:
    """Zephyr 树的位置：优先 $ZEPHYR_BASE，其次从 west 工作区推断。"""
    env = os.environ.get("ZEPHYR_BASE")
    if env:
        return Path(env)

    here = Path(__file__).resolve().parent
    for parent in (here, *here.parents):
        candidate = parent / "zephyr" / "scripts" / "build" / "uf2conv.py"
        if candidate.is_file():
            return parent / "zephyr"

    raise SystemExit("找不到 Zephyr 树：先 source scripts/zephyr-env.sh，或用 ZEPHYR_BASE 指明")


def read_kconfig(config_path: Path) -> dict:
    values = {}
    for line in config_path.read_text().splitlines():
        match = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line.strip())
        if match:
            values[match.group(1)] = match.group(2).strip('"')
    return values


def find_signed_image(build_dir: Path) -> Path:
    """应用镜像的 zephyr.signed.bin：排除 bootloader 自己那份。"""
    candidates = [
        path
        for path in build_dir.glob("*/zephyr/zephyr.signed.bin")
        if path.parent.parent.name != "mcuboot"
    ]
    if not candidates:
        raise SystemExit(
            f"{build_dir} 里没有找到应用镜像的 zephyr.signed.bin——"
            "先 `west build --sysbuild -b <board>/mcuboot app`"
        )
    if len(candidates) > 1:
        raise SystemExit(
            "找到多个候选：" + ", ".join(str(path) for path in candidates) +
            "（用第一个参数指定具体目录，或先删掉旧的构建目录）"
        )
    return candidates[0]


def main() -> int:
    default_build = Path(os.environ.get("ZLRS_WS", ".")) / "build"
    parser = argparse.ArgumentParser(description="把签名镜像打成 BOOTSEL 可拖的 UF2")
    parser.add_argument("build_dir", nargs="?", default=str(default_build))
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    if not build_dir.is_dir():
        raise SystemExit(f"构建目录不存在：{build_dir}")

    signed_bin = find_signed_image(build_dir)
    app_config = read_kconfig(signed_bin.parent / ".config")

    try:
        flash_base = int(app_config["CONFIG_FLASH_BASE_ADDRESS"], 0)
        load_offset = int(app_config.get("CONFIG_FLASH_LOAD_OFFSET", "0"), 0)
        family = app_config["CONFIG_BUILD_OUTPUT_UF2_FAMILY_ID"]
    except KeyError as missing:
        raise SystemExit(f"构建配置里缺少 {missing}：这不像一个 MCUboot 构建目录")

    base_addr = flash_base + load_offset
    out_path = signed_bin.with_suffix(".uf2")
    uf2conv = find_zephyr_base() / "scripts" / "build" / "uf2conv.py"

    cmd = [
        sys.executable,
        str(uf2conv),
        "-c",
        "-f",
        family,
        "-b",
        hex(base_addr),
        "-o",
        str(out_path),
        str(signed_bin),
    ]
    subprocess.run(cmd, check=True)

    print(f"应用镜像 : {signed_bin}")
    print(f"落点     : {base_addr:#x}（flash 基址 {flash_base:#x} + 分区偏移 {load_offset:#x}）")
    print(f"UF2      : {out_path}")
    print("提醒     : 同一个目录里的 zephyr.uf2 是未签名镜像，拖进去 MCUboot 不会认")

    return 0


if __name__ == "__main__":
    sys.exit(main())
