# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Enter the zlrs Zephyr build environment.
#
#   source tools/zephyr-env.sh
#
# The workspace comes from tools/setup-zephyr.sh and defaults to the parent directory of the
# repository (/home/qs/zlrs-worksapce here). The toolchain is Zephyr SDK 1.0.0, by default at
# $HOME/zephyr-sdk-1.0.0. Every variable can be preset to override:
#
#   ZLRS_WS  ZLRS_VENV  ZLRS_SDK_DIR  ZLRS_BOARD  ZLRS_APP

ZLRS_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ZLRS_WS="${ZLRS_WS:-$(dirname "${ZLRS_REPO_ROOT}")}"
ZLRS_VENV="${ZLRS_VENV:-${ZLRS_WS}/.venv}"
ZLRS_SDK_DIR="${ZLRS_SDK_DIR:-${HOME}/zephyr-sdk-1.0.0}"

# Board: Waveshare RP2350-Zero plus Core2021, defined under boards/ in this repository.
# sub-G has two SKUs (two variants): hf = 850-930 MHz (868/915) and
# lf = 410-510 MHz (470/433). The default follows the HF module on hand.
ZLRS_BOARD="${ZLRS_BOARD:-rp2350_zero_core2021/rp2350a/m33/hf}"
ZLRS_APP="${ZLRS_APP:-${ZLRS_REPO_ROOT}/app}"

export ZLRS_REPO_ROOT ZLRS_WS ZLRS_VENV ZLRS_SDK_DIR
export ZLRS_BOARD ZLRS_APP

# west and the build dependencies live in their own venv (re-sourcing does not duplicate PATH)
case ":${PATH}:" in
	*":${ZLRS_VENV}/bin:"*) ;;
	*) export PATH="${ZLRS_VENV}/bin:${PATH}" ;;
esac

export ZEPHYR_BASE="${ZLRS_WS}/zephyr"

# Zephyr SDK: the target toolchain and the host tools (dtc, openocd, qemu...) come from here.
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="${ZLRS_SDK_DIR}"

# This repository is the manifest repository and a Zephyr module (zephyr/module.yml) at once,
# so the workspace picks it up automatically; no ZEPHYR_EXTRA_MODULES needed.

# Convenience wrapper: zlrs-build [extra west build arguments]
zlrs-build() {
	(cd "${ZLRS_WS}" && west build -b "${ZLRS_BOARD}" "${ZLRS_APP}" "$@")
}

# MCUboot builds add a /mcuboot qualifier and need --sysbuild to pull the bootloader in
# (doc/08-firmware.md 6). LF SKU: ZLRS_BOARD=.../lf zlrs-build-mcuboot
zlrs-build-mcuboot() {
	(cd "${ZLRS_WS}" && west build --sysbuild -b "${ZLRS_BOARD}/mcuboot" "${ZLRS_APP}" "$@")
}

if [ ! -d "${ZLRS_WS}/.west" ]; then
	echo "zlrs: no west workspace at ${ZLRS_WS}/.west; run \"${ZLRS_REPO_ROOT}/tools/setup-zephyr.sh\" first" >&2
elif [ ! -d "${ZEPHYR_BASE}" ] || [ ! -x "${ZLRS_VENV}/bin/west" ]; then
	echo "zlrs: incomplete workspace (no zephyr/ or .venv/bin/west); re-run \"${ZLRS_REPO_ROOT}/tools/setup-zephyr.sh\"" >&2
fi

if [ ! -x "${ZLRS_SDK_DIR}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" ] &&
   [ ! -x "${ZLRS_SDK_DIR}/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" ]; then
	echo "zlrs: no arm-zephyr-eabi toolchain in ${ZLRS_SDK_DIR}, install it:
  cd \"${ZLRS_SDK_DIR}\" && ./setup.sh -t arm-zephyr-eabi -h -c" >&2
fi

echo "zlrs env:"
echo "  workspace   = ${ZLRS_WS}"
echo "  ZEPHYR_BASE = ${ZEPHYR_BASE}"
echo "  toolchain   = ${ZEPHYR_TOOLCHAIN_VARIANT} (Zephyr SDK $(cat "${ZLRS_SDK_DIR}/sdk_version" 2>/dev/null || echo '?') @ ${ZLRS_SDK_DIR})"
echo "  board       = ${ZLRS_BOARD}"
echo "  build       = zlrs-build  ->  ${ZLRS_WS}/build"
