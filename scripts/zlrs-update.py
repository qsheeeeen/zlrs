#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# One command upgrades a healthy device: trigger recovery, wait for the bootloader's recovery
# port, upload the signed image and wait for the application to come back. No replugging, no
#
# Flow (see doc/08-firmware.md 6):
#   1. send the recovery command to the application's config port (scripts/zlrs-recovery.py),
#      the application sets the boot-mode flag and reboots;
#   2. MCUboot stops in serial recovery and enumerates as "ZLRS MCUBOOT" (two CDCs: recovery
#      first, log second); the script finds it by name under /dev/serial/by-id;
#   3. run smpmgr's upgrade (a Python SMP/mcumgr client): upload, mark for the next boot and
#      reboot, so MCUboot swaps the slots;
#   4. wait for the "ZLRS" application ports and report the result.
#
# Usage:
#   python3 scripts/zlrs-update.py --port /dev/ttyACM1            # finds the image itself
#   python3 scripts/zlrs-update.py --port /dev/ttyACM1 --image build/app/zephyr/zephyr.signed.bin
#   python3 scripts/zlrs-update.py --recovery-port /dev/ttyACM0 --no-trigger   # already in recovery
#   python3 scripts/zlrs-update.py --self-test                    # no device needed
#
# Requires pyserial (for the trigger) and smpmgr (for the upload; pip install smpmgr).
# Use --client none to keep the script from uploading: it prints the command instead.

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

# The application's confirm timeout (the default of ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS in
# app/Kconfig). Only used to inform the user, never for the verdict.
CONFIG_CONFIRM_TIMEOUT_HINT = "60 s"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def workspace_root() -> Path:
    return Path(os.environ.get("ZLRS_WS", repo_root().parent))


def default_image() -> Path:
    """The signed image sysbuild produces (the app image directory is app/)."""
    return workspace_root() / "build" / "app" / "zephyr" / "zephyr.signed.bin"


def find_by_id(hint: str, exclude: str | None = None) -> list[str]:
    """Find serial ports by their /dev/serial/by-id name (case-insensitive)."""
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
    """Wait for a port whose name contains hint to appear (USB re-enumeration takes time)."""
    deadline = time.monotonic() + timeout_s
    while True:
        found = find_by_id(hint, exclude)
        if found:
            return found[0]
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.2)


def find_client(name: str = "smpmgr") -> str | None:
    """Look in PATH first, then in the west workspace venv where setup-zephyr.sh installs it."""
    found = shutil.which(name)
    if found:
        return found

    venv = workspace_root() / ".venv" / "bin" / name
    if venv.is_file():
        return str(venv)

    return None


def upload_command(client: str, port: str, image: Path, baud: int) -> list[str]:
    """
    smpmgr's upgrade = upload, mark for the next boot, reboot (since 0.19).

    inactive_slot() works out which slot to write (serial recovery defaults to the primary slot,
    which would overwrite the running image; naming the inactive one is what gives A/B).
    The marker is pending, never permanent: the application decides the outcome, and without a
    confirm command before the timeout the next reset rolls back (doc/08-firmware.md 6).

    This script **never** passes `--confirm` to smpmgr: that marks the image permanent inside the
    bootloader before the image has ever run, cutting rollback off. Only the application marks it
    the application calls `boot_write_img_confirmed()` itself once it is up and the host is talking.
    """
    cmd = [client, "--port", port, "--baudrate", str(baud), "upgrade"]
    cmd.append(str(image))

    return cmd


def slot_upload_command(client: str, port: str, image: Path, baud: int,
                        slot: int) -> list[str]:
    """
    smpmgr's `--slot` uses MCUboot's direct-image numbering: 0/1 -> slot0, 2 -> slot1.
    So physical slot N is passed as N + 1.
    """
    cmd = upload_command(client, port, image, baud)
    cmd.insert(-1, "--slot")
    cmd.insert(-1, str(slot + 1))

    return cmd


def inactive_slot(client: str, port: str, baud: int) -> int | None:
    """
    Ask MCUboot which slot is running (the active one in `image state-read`) and return the other.
    Returns None when unknown (callers fall back to 1: the app links into slot0, so slot1 is free).
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
    Send one config command (status) to the new image's config port; the upgrade flow decides the
    outcome, see steps [5/6] and [6/6] in main().
    """
    return [sys.executable, str(repo_root() / "scripts" / "zlrs-config.py"),
            "--port", port, "status"]


def config_subcmd_command(port: str, subcmd: str) -> list[str]:
    return [sys.executable, str(repo_root() / "scripts" / "zlrs-config.py"),
            "--port", port, "--timeout", "3", subcmd]


def run_config_subcmd(port: str, subcmd: str) -> tuple[int, str]:
    """Run one config command, return (rc, output)."""
    result = subprocess.run(config_subcmd_command(port, subcmd), capture_output=True,
                            text=True, check=False)
    return result.returncode, f"{result.stdout or ''}{result.stderr or ''}"


def run_config_subcmd_retry(port: str, subcmd: str, attempts: int = 5,
                            delay: float = 2.0) -> tuple[int, str]:
    """
    Retrying variant: right after a new image boots the main loop may not serve config sessions
    yet (radio init is still running), so the first attempts time out. That is normal, not a
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
    """Pick the 64 hex characters out of the command output (the image hash)."""
    for line in out.splitlines():
        line = line.strip()

        if len(line) == 64 and all(c in "0123456789abcdefABCDEF" for c in line):
            return line.lower()

    return None


def image_hash_of(path: Path) -> str | None:
    """
    The hex of IMAGE_TLV_SHA256 (32 bytes) that MCUboot wrote into the local signed image.
    The device answers with the same value for the running image: equal means it is the one we sent.
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
    print(f"[1/4] trigger recovery on {port}")
    return subprocess.run(
        [sys.executable, str(script), "--port", port, "--baud", str(baud)], check=False
    ).returncode


def self_test() -> int:
    """Everything verifiable without a device: image path, port matching, client command lines."""
    problems = []

    # Port matching: application patterns must not match the bootloader's ports
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
            problems.append(f"application port matches are wrong: {app_ports}")
        if rec_ports != [fake[1]]:
            problems.append(f"recovery port matches are wrong: {rec_ports}")
    finally:
        glob.glob = original  # type: ignore[assignment]

    # Upload command line
    cmd = upload_command("smpmgr", "/dev/ttyACM0", Path("zephyr.signed.bin"), DEFAULT_BAUD)
    if cmd[0] != "smpmgr" or "upgrade" not in cmd or cmd[-1] != "zephyr.signed.bin":
        problems.append(f"upload command is wrong: {cmd}")
    # No upload command may mark an image permanent: that is the application's job after it boots
    if "--confirm" in cmd:
        problems.append(f"upload command must not pass --confirm (only the app marks permanent): {cmd}")
    # The inactive slot must be named: serial recovery defaults to the primary slot
    slotted = slot_upload_command("smpmgr", "/dev/ttyACM0", Path("zephyr.signed.bin"),
                                  DEFAULT_BAUD, 1)
    if "--slot" not in slotted or slotted[slotted.index("--slot") + 1] != "2":
        problems.append(f"upload command must name the inactive slot: {slotted}")

    probe = config_probe_command("/dev/ttyACM0")
    if "status" not in probe or "/dev/ttyACM0" not in probe:
        problems.append(f"host confirm command is wrong: {probe}")

    # The trigger step must exist and frame identically to the device (its own self-test checks)
    recovery = repo_root() / "scripts" / "zlrs-recovery.py"
    if not recovery.is_file():
        problems.append(f"missing {recovery}")
    else:
        result = subprocess.run(
            [sys.executable, str(recovery), "--self-test"], capture_output=True, check=False
        )
        if result.returncode != 0:
            problems.append("zlrs-recovery.py --self-test failed")

    if problems:
        print("self-test: FAIL")
        for problem in problems:
            print(f"  - {problem}")
        return 1

    print("self-test: ok (port matching, upload command, trigger script all line up)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Trigger recovery and upgrade the device firmware")
    parser.add_argument("--port", help="application config port (for the trigger), e.g. /dev/ttyACM2")
    parser.add_argument("--recovery-port", help="bootloader recovery port; skips the search")
    parser.add_argument("--image", help="signed image path (defaults to <workspace>/build/app/zephyr/zephyr.signed.bin)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"serial baud rate (default {DEFAULT_BAUD})")
    parser.add_argument("--client", choices=["auto", "smpmgr", "none"], default="auto",
                        help="upload client: auto finds smpmgr, none only prints the command")
    parser.add_argument("--no-trigger", action="store_true", help="device is already in recovery, do not trigger")
    parser.add_argument("--no-wait", action="store_true", help="do not wait for the application ports")
    parser.add_argument("--timeout", type=float, default=15.0, help="how long to wait for ports (s, default 15)")
    parser.add_argument("--no-confirm-check", action="store_true",
                        help="skip the host confirm (by default a command goes to the new image's config port)")
    parser.add_argument("--self-test", action="store_true", help="check paths and commands without a device")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    image = Path(args.image) if args.image else default_image()
    if not image.is_file():
        print(f"no signed image at {image}\nbuild one first (zlrs-build-mcuboot) or pass --image", file=sys.stderr)
        return 1

    # 1. trigger
    if not args.no_trigger:
        if args.port is None:
            ports = find_by_id(APP_PORT_HINT, exclude=RECOVERY_PORT_HINT)
            if not ports:
                parser.error("no application config port (...-if04) found; pass --port")
            args.port = next((p for p in ports if p.endswith("-if04")), ports[0])
        if run_trigger(args.port, args.baud) != 0:
            print("trigger failed: the device did not answer on the config port", file=sys.stderr)
            return 1
    else:
        print("[1/4] skipping the trigger (--no-trigger)")

    # 2. wait for the bootloader recovery port
    if args.recovery_port is None:
        print(f"[2/4] waiting for the bootloader recovery port (name contains {RECOVERY_PORT_HINT}, up to {args.timeout:g} s)")
        args.recovery_port = wait_for_by_id(RECOVERY_PORT_HINT, args.timeout)
        if args.recovery_port is None:
            print("no recovery port appeared. Device up but not in recovery? Check the boot-mode flag "
                      "(doc/08-firmware.md 6), "
                  "or name the port with --recovery-port", file=sys.stderr)
            return 1
    print(f"    recovery port: {args.recovery_port}")

    # 3. upload
    client = None
    if args.client == "auto":
        client = find_client("smpmgr")
    elif args.client == "smpmgr":
        client = find_client("smpmgr")
        if client is None:
            print("smpmgr not found: pip install smpmgr", file=sys.stderr)
            return 1

    # The tool names the slot: serial recovery defaults to the primary slot (overwriting the
    # running image), and only naming the inactive one gives A/B. It comes from image state-read.
    # Unknown means slot1 (which is the case while slot0 runs).
    target_slot = None
    if client is not None:
        target_slot = inactive_slot(client, args.recovery_port, args.baud)
    if target_slot is None:
        target_slot = 1

    cmd = slot_upload_command(client or "smpmgr", args.recovery_port, image, args.baud,
                              target_slot)
    if client is None:
        print("[3/5] upload manually (the script was told not to):")
        print("      " + " ".join(cmd))
    else:
        print(f"[3/5] upload and reboot: {image.name} (writes inactive slot{target_slot}, "
              "marked test, rolls back unless confirmed)")
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        out = f"{result.stdout or ''}{result.stderr or ''}"

        if out.strip():
            print(out.rstrip())

        if result.returncode != 0:
            # ENOTSUP almost always means the bootloader lacks the image-state group: without it the
            # "mark for test" step cannot happen, so nothing swaps and nothing rolls back. Fail loudly.
            if "ENOTSUP" in out:
                print("bootloader cannot write image state (IMAGE_STATE): the upgrade cannot be marked test, "
                      "reflash a bootloader built with CONFIG_BOOT_SERIAL_IMG_GRP_IMAGE_STATE",
                      file=sys.stderr)
            else:
                print("upload failed", file=sys.stderr)
            return result.returncode

    # 4. wait for the application
    app_port = None

    if not args.no_wait and client is not None:
        print(f"[4/6] waiting for the application ports (name contains {APP_PORT_HINT}, not {RECOVERY_PORT_HINT})")
        port = wait_for_by_id(APP_PORT_HINT, args.timeout, exclude=RECOVERY_PORT_HINT)
        if port is None:
            print("the application ports never came back. The upgrade runs with test semantics: the next "
                  "reset rolls back to the previous image; retry once it is back on the old one (08-firmware 6)",
                  file=sys.stderr)
            return 1
        app_port = next((p for p in find_by_id(APP_PORT_HINT, exclude=RECOVERY_PORT_HINT)
                         if p.endswith("-if04")), port)
        print(f"    application is up: {app_port}")
    else:
        print("[4/6] skipping the wait for application ports")

    if app_port is None:
        print("[5/6] no application config port: cannot verify or confirm, this image rolls back", file=sys.stderr)
        return 1

    # 5. verify the running image is the one just sent: compare hashes (build strings can match)
    want_hash = image_hash_of(image)
    rc, out = run_config_subcmd_retry(app_port, "image-hash")
    got_hash = parse_hex_hash(out) if rc == 0 else None

    print(f"[5/6] verify the running image: expected {want_hash or '?'}, device reports {got_hash or '?'}")

    if want_hash is None or got_hash != want_hash:
        if not args.no_confirm_check:
            print("the device is not running the image just sent: no confirm, it rolls back by itself ( "
                  f"within {CONFIG_CONFIRM_TIMEOUT_HINT})", file=sys.stderr)
            return 1
        print("(--no-confirm-check: skipping the verification)")

    # 6. confirm: only this command makes the device mark the image permanent; otherwise rollback
    if args.no_confirm_check:
        print("[6/6] skipping the confirm (--no-confirm-check): this image will not stay, it rolls back")
        return 0

    rc, out = run_config_subcmd_retry(app_port, "confirm")
    if out.strip():
        print(out.rstrip())

    if rc != 0:
        print("confirm failed: the device did not accept it, this image rolls back", file=sys.stderr)
        return 1

    rc, out = run_config_subcmd_retry(app_port, "build")
    print(f"    upgrade succeeded: image confirmed{('  firmware ' + out.strip()) if rc == 0 else ''}")
    return 0

    print("upgrade finished")
    return 0


if __name__ == "__main__":
    sys.exit(main())
