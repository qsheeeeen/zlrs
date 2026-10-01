#!/usr/bin/env bash
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Build the zlrs Zephyr workspace from scratch.
#
#   tools/setup-zephyr.sh [workspace root]    defaults to the directory above the repository
#
# west T2 topology: this repository is the manifest repository and the manifest is west.yml
# (pinning zephyr v4.4.0 and usp_zephyr/usp v1.2.1); usp_zephyr is no longer
# the manifest repository.
#
# Default layout (on this machine: /home/qs/zlrs-worksapce):
#
#   zlrs-worksapce/          <- west workspace root, called $WS below
#   ├── .west/               west workspace marker
#   ├── .venv/               west and the build dependencies (Python venv, ZLRS_VENV overrides)
#   ├── build/               default output directory of west build
#   ├── zlrs/                this repository == manifest repository (west.yml self.path)
#   ├── zephyr/              Zephyr v4.4.0 plus the modules it imports
#   ├── usp_zephyr/          Semtech usp_zephyr v1.2.1
#   ├── bootloader/mcuboot/  MCUboot: the bootloader image and its imgtool (doc/08-firmware.md 5)
#   └── modules/lib/usp/     Semtech usp v1.2.1
#
# The toolchain comes from the Zephyr SDK (default $HOME/zephyr-sdk-1.0.0, ZLRS_SDK_DIR overrides):
# this script only checks and prints the install command when something is missing (a distro
# arm-none-eabi-gcc has no retargetable locking in newlib, so it is no substitute).
#
# The first west update pulls every Zephyr module: about 5 GB.

set -euo pipefail

die() {
	echo "setup-zephyr: $*" >&2
	exit 1
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZLRS_REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MANIFEST="${ZLRS_REPO_ROOT}/west.yml"

# self.path in west.yml has to match the repository's directory name in the workspace.
SELF_PATH="$(awk '/^ *self:/{found=1} found && /^ *path:/{print $2; exit}' "${MANIFEST}")"
[ -n "${SELF_PATH}" ] || die "no self: path: in ${MANIFEST}"

WS="${1:-${ZLRS_WS:-$(dirname "${ZLRS_REPO_ROOT}")}}"
mkdir -p "${WS}"
WS="$(cd "${WS}" && pwd)"
MANIFEST_REPO="${WS}/${SELF_PATH}"

if [ "$(readlink -f "${MANIFEST_REPO}")" != "${ZLRS_REPO_ROOT}" ]; then
	die "the workspace needs this repository, with a directory name matching west.yml self.path:
    expected: ${MANIFEST_REPO}
    actual:   ${ZLRS_REPO_ROOT}
Move the repository there, or create a symlink inside ${WS}:
  ln -s \"${ZLRS_REPO_ROOT}\" \"${MANIFEST_REPO}\"
To avoid moving it, run this script without arguments: the default workspace is its parent. "
fi

VENV_DIR="${ZLRS_VENV:-${WS}/.venv}"

echo "==> workspace: ${WS}   (manifest: ${SELF_PATH}/west.yml)"

echo "==> [1/5] Python venv: ${VENV_DIR}"
if [ ! -x "${VENV_DIR}/bin/python" ]; then
	python3 -m venv --without-pip "${VENV_DIR}"
fi
if ! "${VENV_DIR}/bin/python" -m pip --version > /dev/null 2>&1; then
	# Ubuntu's python3.12 has no ensurepip, so the venv has no pip and it has to be bootstrapped.
	echo "    no pip in the venv, bootstrapping with get-pip.py"
	curl -fsSL -o "${WS}/.get-pip.py" https://bootstrap.pypa.io/get-pip.py
	"${VENV_DIR}/bin/python" "${WS}/.get-pip.py" --no-warn-script-location
	rm -f "${WS}/.get-pip.py"
fi

echo "==> [2/5] west and ninja"
"${VENV_DIR}/bin/python" -m pip install --quiet --no-warn-script-location west ninja
WEST="${VENV_DIR}/bin/west"

echo "==> [3/5] west init"
if [ ! -d "${WS}/.west" ]; then
	(cd "${WS}" && "${WEST}" init -l "${SELF_PATH}")
else
	echo "    ${WS}/.west already exists, skipping"
fi

echo "==> [4/5] west update"
# "west list" resolves the whole manifest, which needs zephyr's imported west.yml, and that file
# only exists once zephyr has been cloned. In a fresh workspace the listing itself fails, and that
# just means nothing has been cloned yet, so go straight to the update instead of bailing out.
if PROJECTS="$(cd "${WS}" && "${WEST}" list -f '{path}' 2>/dev/null)"; then
	INCOMPLETE="$(for project in ${PROJECTS}; do
		[ -e "${WS}/${project}/.git" ] || echo "${project}"
	done)"
else
	INCOMPLETE="zephyr"
fi
if [ -n "${INCOMPLETE}" ]; then
	[ -d "${WS}/zephyr" ] || echo "    the first run pulls Zephyr and its modules: about 5 GB"
	echo "    $(echo "${INCOMPLETE}" | wc -l) projects missing: $(echo "${INCOMPLETE}" | tr '\n' ' ')"
	(cd "${WS}" && "${WEST}" update --narrow -o=--depth=1)
else
	echo "    all projects present, skipping (run (cd ${WS} && west update) to sync)"
fi

echo "==> Applying carried module patches"
for patch_file in "${ZLRS_REPO_ROOT}"/patches/*/*.patch; do
    [ -f "${patch_file}" ] || continue
    module_dir="${WS}/$(basename "$(dirname "${patch_file}")")"
    if git -C "${module_dir}" apply --reverse --check "${patch_file}" 2>/dev/null; then
        echo "    $(basename "${patch_file}"): already applied"
    elif git -C "${module_dir}" apply --check "${patch_file}"; then
        git -C "${module_dir}" apply "${patch_file}"
        echo "    $(basename "${patch_file}"): applied"
    else
        die "patch conflicts with ${module_dir}: ${patch_file}"
    fi
done

echo "==> [5/5] Zephyr's Python build dependencies"
"${VENV_DIR}/bin/python" -m pip install --quiet --no-warn-script-location \
	-r "${WS}/zephyr/scripts/requirements-base.txt"

# Every image is built with `--sysbuild` and signed by imgtool (MCUboot's tool), whose
# dependencies are not in Zephyr's requirements-base: without them the build fails at the
# signing step with "No module named 'cryptography'".
echo "    imgtool (MCUboot signing) dependencies"
"${VENV_DIR}/bin/python" -m pip install --quiet --no-warn-script-location \
	-r "${WS}/bootloader/mcuboot/scripts/requirements.txt"

# Host-side tool scripts: pyserial for management mode and the recovery trigger, smpmgr for upgrades
# (tools/zlrs.py: management, recovery and updates).
echo "    host script dependencies (pyserial / smpmgr)"
"${VENV_DIR}/bin/python" -m pip install --quiet --no-warn-script-location \
	pyserial smpmgr

SDK_DIR="${ZLRS_SDK_DIR:-${HOME}/zephyr-sdk-1.0.0}"
SDK_GCC="${SDK_DIR}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc"
[ -x "${SDK_GCC}" ] || SDK_GCC="${SDK_DIR}/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc"

echo
echo "workspace ready: ${WS}"
echo "  Zephyr     : $(sed -n 's/^VERSION_MAJOR = //p;s/^VERSION_MINOR = //p;s/^PATCHLEVEL = //p' "${WS}/zephyr/VERSION" | paste -sd. -)"

if [ -x "${SDK_GCC}" ]; then
	echo "  Zephyr SDK : ${SDK_DIR} (arm-zephyr-eabi $("${SDK_GCC}" -dumpversion))"
else
	echo "  Zephyr SDK : ${SDK_DIR}  <-- arm-zephyr-eabi toolchain missing"
	echo
	if [ -x "${SDK_DIR}/setup.sh" ]; then
		echo "  install the toolchain (~1 GB download):"
		echo "    cd \"${SDK_DIR}\" && ./setup.sh -t arm-zephyr-eabi -h -c"
	else
		echo "  no SDK yet: fetch the minimal package, then install the arm toolchain:"
		echo "    cd \"$(dirname "${SDK_DIR}")\""
		echo "    curl -LO https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.0/zephyr-sdk-1.0.0_linux-x86_64_minimal.tar.xz"
		echo "    tar xf zephyr-sdk-1.0.0_linux-x86_64_minimal.tar.xz"
		echo "    cd \"${SDK_DIR}\" && ./setup.sh -t arm-zephyr-eabi -h -c"
	fi
fi

echo
echo "next:"
echo "  source \"${ZLRS_REPO_ROOT}/tools/zephyr-env.sh\""
echo "  zlrs-build     # = cd \$ZLRS_WS && west build --sysbuild -b \$ZLRS_BOARD \$ZLRS_APP"
