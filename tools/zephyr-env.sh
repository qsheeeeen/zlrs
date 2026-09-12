# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 设置 zlrs 的 Zephyr 构建环境。
#
#   source tools/zephyr-env.sh
#
# 工作区由 tools/setup-zephyr.sh 创建，默认在 /tmp/zws。

ZLRS_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ZLRS_REPO_ROOT

ZLRS_WS="${ZLRS_WS:-/tmp/zws}"
export ZLRS_WS

if [ ! -d "${ZLRS_WS}/ws/zephyr" ]; then
	echo "zlrs: 找不到 Zephyr 工作区 ${ZLRS_WS}/ws" >&2
	echo "zlrs: 先运行 tools/setup-zephyr.sh" >&2
	return 1 2>/dev/null || exit 1
fi

# west 与构建依赖装在独立 venv 里
export PATH="${ZLRS_WS}/venv/bin:${PATH}"

export ZEPHYR_BASE="${ZLRS_WS}/ws/zephyr"

# 用系统自带的 arm-none-eabi-gcc，不装 Zephyr SDK。
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH="${GNUARMEMB_TOOLCHAIN_PATH:-/usr}"

# zlrs 仓库本身是一个 Zephyr 模块。它不在 west 工作区里（仓库在别处），
# 所以用 ZEPHYR_EXTRA_MODULES 挂进去。
export ZEPHYR_EXTRA_MODULES="${ZLRS_REPO_ROOT}"

ZLRS_BOARD="${ZLRS_BOARD:-xiao_nrf54l15/nrf54l15/cpuapp}"
export ZLRS_BOARD

echo "zlrs env:"
echo "  ZEPHYR_BASE = ${ZEPHYR_BASE}"
echo "  toolchain   = ${ZEPHYR_TOOLCHAIN_VARIANT} (${GNUARMEMB_TOOLCHAIN_PATH})"
echo "  board       = ${ZLRS_BOARD}"
