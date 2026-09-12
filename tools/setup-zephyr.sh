#!/usr/bin/env bash
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 从零搭一个 Zephyr 工作区，用于编译 zlrs 与官方 USP 示例。
#
#   tools/setup-zephyr.sh [工作区根目录]      默认 /tmp/zws
#
# 产物：
#   $WS/venv     Python 虚拟环境（west 与构建依赖）
#   $WS/ws       west 工作区（zephyr v4.2.0 + usp_zephyr v1.0.0 + usp）
#
# 之后用 `source tools/zephyr-env.sh` 进入环境。
#
# 不需要 Zephyr SDK：用系统自带的 arm-none-eabi-gcc。
# 因此示例的 CONFIG_NEWLIB_LIBC=y 会失败（发行版 newlib 未开
# retargetable locking），需要改用 Zephyr 自带的 picolibc。

set -euo pipefail

WS="${1:-/tmp/zws}"
ZEPHYR_REV="${ZEPHYR_REV:-v4.2.0}"
USP_ZEPHYR_REV="${USP_ZEPHYR_REV:-v1.0.0}"

echo "==> 工作区: ${WS}"
mkdir -p "${WS}"

echo "==> [1/4] Python 虚拟环境 + pip"
if [ ! -x "${WS}/venv/bin/python" ]; then
	python3 -m venv --without-pip "${WS}/venv"
	curl -sSL -o "${WS}/get-pip.py" https://bootstrap.pypa.io/get-pip.py
	"${WS}/venv/bin/python" "${WS}/get-pip.py" --no-warn-script-location
fi

echo "==> [2/4] 安装 west、ninja 与构建依赖"
"${WS}/venv/bin/pip" install --quiet --no-warn-script-location \
	west ninja pyelftools packaging PyYAML

echo "==> [3/4] 拉 usp_zephyr 并初始化 west 工作区"
if [ ! -d "${WS}/ws/usp_zephyr" ]; then
	mkdir -p "${WS}/ws"
	git clone --quiet --depth 1 -b "${USP_ZEPHYR_REV}" \
		https://github.com/Lora-net/usp_zephyr.git "${WS}/ws/usp_zephyr"
fi
if [ ! -d "${WS}/ws/.west" ]; then
	(cd "${WS}/ws" && "${WS}/venv/bin/west" init -l usp_zephyr)
fi

echo "==> [4/4] west update（首次会下载约 5 GB，需要几分钟）"
if [ ! -d "${WS}/ws/zephyr" ]; then
	(cd "${WS}/ws" && "${WS}/venv/bin/west" update --narrow -o=--depth=1)
fi

ZEPHYR_BASE="${WS}/ws/zephyr" \
	"${WS}/venv/bin/pip" install --quiet --no-warn-script-location \
	-r "${WS}/ws/zephyr/scripts/requirements-base.txt"

echo
echo "完成。"
echo "  Zephyr  : $(sed -n 's/^VERSION_MAJOR = //p' "${WS}/ws/zephyr/VERSION").$(sed -n 's/^VERSION_MINOR = //p' "${WS}/ws/zephyr/VERSION").$(sed -n 's/^PATCHLEVEL = //p' "${WS}/ws/zephyr/VERSION")"
echo "  usp_zephyr: ${USP_ZEPHYR_REV}"
echo
echo "下一步:  source tools/zephyr-env.sh"
