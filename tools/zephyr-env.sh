# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 设置 zlrs 的 Zephyr 构建环境。
#
#   source tools/zephyr-env.sh
#
# 工作区由 tools/setup-zephyr.sh 创建，默认是仓库的上一级目录
# （当前机器上是 /home/qs/zlrs-worksapce）。工具链用 Zephyr SDK，
# 默认 $HOME/zephyr-sdk-1.0.0。变量都能提前设好覆盖：
#
#   ZLRS_WS  ZLRS_VENV  ZLRS_SDK_DIR  ZLRS_BOARD  ZLRS_SHIELD  ZLRS_APP

ZLRS_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ZLRS_WS="${ZLRS_WS:-$(dirname "${ZLRS_REPO_ROOT}")}"
ZLRS_VENV="${ZLRS_VENV:-${ZLRS_WS}/.venv}"
ZLRS_SDK_DIR="${ZLRS_SDK_DIR:-${HOME}/zephyr-sdk-1.0.0}"

ZLRS_BOARD="${ZLRS_BOARD:-rpi_pico2/rp2350a/m33}"
ZLRS_SHIELD="${ZLRS_SHIELD:-}"
ZLRS_APP="${ZLRS_APP:-${ZLRS_WS}/zlrs/app}"

export ZLRS_REPO_ROOT ZLRS_WS ZLRS_VENV ZLRS_SDK_DIR
export ZLRS_BOARD ZLRS_SHIELD ZLRS_APP

# west 与构建依赖装在独立 venv 里（重复 source 不重复往 PATH 里塞）
case ":${PATH}:" in
	*":${ZLRS_VENV}/bin:"*) ;;
	*) export PATH="${ZLRS_VENV}/bin:${PATH}" ;;
esac

export ZEPHYR_BASE="${ZLRS_WS}/zephyr"

# Zephyr SDK：目标工具链和 host tools（dtc、openocd、qemu…）都从这里找。
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="${ZLRS_SDK_DIR}"

# 本仓库是 manifest 仓库，同时也是 Zephyr 模块（zephyr/module.yml），
# 在工作区里会被自动收进去，不用再挂 ZEPHYR_EXTRA_MODULES。

# 便捷构建：zlrs-build [west build 的额外参数]
zlrs-build() {
	if [ -n "${ZLRS_SHIELD}" ]; then
		(cd "${ZLRS_WS}" && west build -b "${ZLRS_BOARD}" --shield "${ZLRS_SHIELD}" \
			"${ZLRS_APP}" "$@")
	else
		(cd "${ZLRS_WS}" && west build -b "${ZLRS_BOARD}" "${ZLRS_APP}" "$@")
	fi
}

if [ ! -d "${ZLRS_WS}/.west" ]; then
	echo "zlrs: 没找到 west 工作区 ${ZLRS_WS}/.west，先跑 \"${ZLRS_REPO_ROOT}/tools/setup-zephyr.sh\"" >&2
elif [ ! -d "${ZEPHYR_BASE}" ] || [ ! -x "${ZLRS_VENV}/bin/west" ]; then
	echo "zlrs: 工作区不完整（缺 zephyr/ 或 .venv/bin/west），再跑一遍 \"${ZLRS_REPO_ROOT}/tools/setup-zephyr.sh\"" >&2
fi

if [ ! -x "${ZLRS_SDK_DIR}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" ] &&
   [ ! -x "${ZLRS_SDK_DIR}/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" ]; then
	echo "zlrs: ${ZLRS_SDK_DIR} 里没有 arm-zephyr-eabi 工具链，装一下：
  cd \"${ZLRS_SDK_DIR}\" && ./setup.sh -t arm-zephyr-eabi -h -c" >&2
fi

echo "zlrs env:"
echo "  workspace   = ${ZLRS_WS}"
echo "  ZEPHYR_BASE = ${ZEPHYR_BASE}"
echo "  toolchain   = ${ZEPHYR_TOOLCHAIN_VARIANT} (Zephyr SDK $(cat "${ZLRS_SDK_DIR}/sdk_version" 2>/dev/null || echo '?') @ ${ZLRS_SDK_DIR})"
echo "  board       = ${ZLRS_BOARD}${ZLRS_SHIELD:+ --shield ${ZLRS_SHIELD}}"
echo "  构建        = zlrs-build  ->  ${ZLRS_WS}/build"
