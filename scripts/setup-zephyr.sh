#!/usr/bin/env bash
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 从零搭 zlrs 的 Zephyr 工作区。
#
#   scripts/setup-zephyr.sh [工作区根目录]     默认是仓库的上一级目录
#
# west T2 拓扑：本仓库自己就是 manifest 仓库，清单用仓库根的 west.yml
# （pin 了 zephyr v4.4.0 + usp_zephyr/usp v1.1.2-feature-202604），不再拿
# usp_zephyr 当 manifest。
#
# 默认布局（当前机器上是 /home/qs/zlrs-worksapce）：
#
#   zlrs-worksapce/          <- west 工作区根，下称 $WS
#   ├── .west/               west 工作区标记
#   ├── .venv/               west 与构建依赖（Python venv，ZLRS_VENV 可改）
#   ├── build/               west build 的默认输出目录
#   ├── zlrs/                本仓库 == manifest 仓库（west.yml 的 self.path）
#   ├── zephyr/              Zephyr v4.4.0（外加 import 进来的一堆 module）
#   ├── usp_zephyr/          Semtech usp_zephyr v1.1.2-feature-202604
#   └── modules/lib/usp/     Semtech usp v1.1.2-feature-202604
#
# 工具链走 Zephyr SDK（默认 $HOME/zephyr-sdk-1.0.0，ZLRS_SDK_DIR 可改）：
# 本脚本只检查，缺了会打印安装命令（发行版 arm-none-eabi-gcc 的 newlib 没开
# retargetable locking，别拿它替）。
#
# 首次 west update 要把 Zephyr 的 module 全拉下来，约 5 GB。

set -euo pipefail

die() {
	echo "setup-zephyr: $*" >&2
	exit 1
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZLRS_REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MANIFEST="${ZLRS_REPO_ROOT}/west.yml"

# west.yml 里 self.path 必须和仓库在工作区里的目录名一致。
SELF_PATH="$(awk '/^ *self:/{found=1} found && /^ *path:/{print $2; exit}' "${MANIFEST}")"
[ -n "${SELF_PATH}" ] || die "在 ${MANIFEST} 里找不到 self: path:"

WS="${1:-${ZLRS_WS:-$(dirname "${ZLRS_REPO_ROOT}")}}"
mkdir -p "${WS}"
WS="$(cd "${WS}" && pwd)"
MANIFEST_REPO="${WS}/${SELF_PATH}"

if [ "$(readlink -f "${MANIFEST_REPO}")" != "${ZLRS_REPO_ROOT}" ]; then
	die "工作区里得有本仓库，目录名还要跟 west.yml 的 self.path 对得上：
  期望: ${MANIFEST_REPO}
  实际: ${ZLRS_REPO_ROOT}
把仓库放到期望位置，或者在 ${WS} 里建个软链接：
  ln -s \"${ZLRS_REPO_ROOT}\" \"${MANIFEST_REPO}\"
不想挪仓库就直接不带参数跑本脚本——默认工作区就是仓库的上一级目录。"
fi

VENV_DIR="${ZLRS_VENV:-${WS}/.venv}"

echo "==> 工作区: ${WS}   （manifest: ${SELF_PATH}/west.yml）"

echo "==> [1/5] Python venv: ${VENV_DIR}"
if [ ! -x "${VENV_DIR}/bin/python" ]; then
	python3 -m venv --without-pip "${VENV_DIR}"
fi
if ! "${VENV_DIR}/bin/python" -m pip --version > /dev/null 2>&1; then
	# Ubuntu 的 python3.12 不带 ensurepip，venv 里也就没有 pip，得自己引导。
	echo "    venv 里没有 pip，用 get-pip.py 引导"
	curl -fsSL -o "${WS}/.get-pip.py" https://bootstrap.pypa.io/get-pip.py
	"${VENV_DIR}/bin/python" "${WS}/.get-pip.py" --no-warn-script-location
	rm -f "${WS}/.get-pip.py"
fi

echo "==> [2/5] west 与 ninja"
"${VENV_DIR}/bin/python" -m pip install --quiet --no-warn-script-location west ninja
WEST="${VENV_DIR}/bin/west"

echo "==> [3/5] west init"
if [ ! -d "${WS}/.west" ]; then
	(cd "${WS}" && "${WEST}" init -l "${SELF_PATH}")
else
	echo "    ${WS}/.west 已存在，跳过"
fi

echo "==> [4/5] west update"
PROJECTS="$(cd "${WS}" && "${WEST}" list -f '{path}')" || die "west list 失败，${WS} 这个工作区可能坏了"
INCOMPLETE="$(for project in ${PROJECTS}; do
	[ -e "${WS}/${project}/.git" ] || echo "${project}"
done)"
if [ -n "${INCOMPLETE}" ]; then
	[ -d "${WS}/zephyr" ] || echo "    首次要把 Zephyr 及其 module 拉下来，约 5 GB"
	echo "    还缺 $(echo "${INCOMPLETE}" | wc -l) 个项目: $(echo "${INCOMPLETE}" | tr '\n' ' ')"
	(cd "${WS}" && "${WEST}" update --narrow -o=--depth=1)
else
	echo "    项目都在了，跳过（要同步就跑 (cd ${WS} && west update)）"
fi

echo "==> [5/5] Zephyr 的 Python 构建依赖"
"${VENV_DIR}/bin/python" -m pip install --quiet --no-warn-script-location \
	-r "${WS}/zephyr/scripts/requirements-base.txt"

SDK_DIR="${ZLRS_SDK_DIR:-${HOME}/zephyr-sdk-1.0.0}"
SDK_GCC="${SDK_DIR}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc"
[ -x "${SDK_GCC}" ] || SDK_GCC="${SDK_DIR}/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc"

echo
echo "工作区就绪: ${WS}"
echo "  Zephyr     : $(sed -n 's/^VERSION_MAJOR = //p;s/^VERSION_MINOR = //p;s/^PATCHLEVEL = //p' "${WS}/zephyr/VERSION" | paste -sd. -)"

if [ -x "${SDK_GCC}" ]; then
	echo "  Zephyr SDK : ${SDK_DIR} (arm-zephyr-eabi $("${SDK_GCC}" -dumpversion))"
else
	echo "  Zephyr SDK : ${SDK_DIR}  <-- 缺 arm-zephyr-eabi 工具链"
	echo
	if [ -x "${SDK_DIR}/setup.sh" ]; then
		echo "  装工具链（下载约 1 GB）："
		echo "    cd \"${SDK_DIR}\" && ./setup.sh -t arm-zephyr-eabi -h -c"
	else
		echo "  还没有 SDK，先下 minimal 包再装 arm 工具链："
		echo "    cd \"$(dirname "${SDK_DIR}")\""
		echo "    curl -LO https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.0/zephyr-sdk-1.0.0_linux-x86_64_minimal.tar.xz"
		echo "    tar xf zephyr-sdk-1.0.0_linux-x86_64_minimal.tar.xz"
		echo "    cd \"${SDK_DIR}\" && ./setup.sh -t arm-zephyr-eabi -h -c"
	fi
fi

echo
echo "下一步:"
echo "  source \"${ZLRS_REPO_ROOT}/scripts/zephyr-env.sh\""
echo "  zlrs-build     # = cd \$ZLRS_WS && west build -b \$ZLRS_BOARD \$ZLRS_APP"
