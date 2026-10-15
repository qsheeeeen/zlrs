#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# One command upgrades a healthy device: trigger recovery, wait for the bootloader's recovery
# port, upload the signed image and wait for the application to come back. No replugging, no
#
# Flow (see doc/06-firmware.md):
#   1. send the recovery command to the application's management port (tools/zlrs.py recovery),
#      the application sets the boot-mode flag and reboots;
#   2. MCUboot stops in serial recovery and enumerates as "ZLRS MCUBOOT" (two CDCs: recovery
#      first, log second); the script finds it by name under /dev/serial/by-id;
#   3. run smpmgr's upgrade (a Python SMP/mcumgr client): upload, mark for the next boot and
#      reboot, so MCUboot swaps the slots;
#   4. wait for the "ZLRS" application ports and report the result.
#
# Usage:
#   python3 tools/zlrs.py update --port /dev/ttyACM1            # finds the image itself
#   python3 tools/zlrs.py update --port /dev/ttyACM1 --image build/app/zephyr/zephyr.signed.bin
#   python3 tools/zlrs.py update --recovery-port /dev/ttyACM0 --no-trigger   # already in recovery
#   python3 tests/tools/run.py --module zlrs_update              # no device needed
#
# Requires pyserial (for the trigger) and smpmgr (for the upload; pip install smpmgr).
# Use --client none to keep the script from uploading: it prints the command instead.

import argparse
import glob
import hashlib
import os
import shutil
import shlex
import struct
import subprocess
import sys
import time
from pathlib import Path

from zlrs_cli import HelpFormatter, bounded_int, positive_seconds, fields, heading, notice, step

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
    build = Path(os.environ.get("ZLRS_BUILD_DIR", "build"))
    if not build.is_absolute():
        build = workspace_root() / build
    return build / "app" / "zephyr" / "zephyr.signed.bin"


def find_by_id(hint: str, exclude: str | None = None, serial: str | None = None) -> list[str]:
    """Find serial ports by their /dev/serial/by-id name (case-insensitive)."""
    found = []
    for path in sorted(glob.glob(f"{BY_ID_DIR}/*")):
        name = os.path.basename(path).upper()
        if hint.upper() not in name:
            continue
        if exclude is not None and exclude.upper() in name:
            continue
        if serial is not None and serial.upper() not in name:
            continue
        found.append(path)
    return found


def app_mgmt_ports(serial: str | None = None) -> list[str]:
    """Application management ports (the second CDC, ...-if02), optionally of one module only."""
    return [
        p for p in find_by_id(APP_PORT_HINT, exclude=RECOVERY_PORT_HINT, serial=serial)
        if p.endswith("-if02")
    ]


def wait_for_by_id(hint: str, timeout_s: float, exclude: str | None = None,
                   serial: str | None = None, exact_serial: bool = False) -> str | None:
    """Wait for a port whose name contains hint to appear (USB re-enumeration takes time)."""
    deadline = time.monotonic() + timeout_s
    while True:
        found = find_by_id(hint, exclude, serial)
        suffix = "-if00" if hint == RECOVERY_PORT_HINT else "-if02"
        found = [p for p in found if p.lower().endswith(suffix)]
        if exact_serial:
            found = [p for p in found if (serial_of_port(p) or "").upper() == serial.upper()]
        if len(found) > 1:
            raise ValueError("several matching devices found; select one with --serial: "
                             + ", ".join(found))
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
    confirm command before the timeout the next reset rolls back (doc/06-firmware.md).

    This script **never** passes `--confirm` to smpmgr: that marks the image permanent inside the
    bootloader before the image has ever run, cutting rollback off. The application calls
    `boot_write_img_confirmed()` itself once it is up and the host is talking.
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
    Returns 0 when no slot is active (a blank device: the first image goes straight into slot0,
    a pending image in slot1 would never be swapped in), and None when the answer is unreadable
    (callers stop before uploading).
    """
    result = subprocess.run([client, "--port", port, "--baudrate", str(baud),
                             "image", "state-read"],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise ValueError("cannot read MCUboot image state: "
                         + (result.stderr or result.stdout or "client failed").strip())
    active = None
    slot = None

    for line in f"{result.stdout or ''}{result.stderr or ''}".splitlines():
        line = line.strip()

        if line.startswith("slot="):
            slot = int(line.split("=", 1)[1].strip().rstrip(","))
            if slot not in (0, 1):
                raise ValueError(f"unsupported MCUboot slot {slot}")
        elif line.startswith("active=") and line.split("=", 1)[1].strip().rstrip(",") == "True":
            active = slot

    if active is None:
        # No active image: treat it as an install, not an upgrade. This also covers the case
        # where MCUboot answered but every slot had active=None.
        return 0 if slot is not None else None

    return 1 - active


def mgmt_subcmd_command(port: str, subcmd: str) -> list[str]:
    return [sys.executable, str(repo_root() / "tools" / "zlrs.py"),
            subcmd, "--port", port, "--timeout", "10"]


def run_mgmt_subcmd(port: str, subcmd: str) -> tuple[int, str]:
    """Run one management command, return (rc, output)."""
    result = subprocess.run(mgmt_subcmd_command(port, subcmd), capture_output=True,
                            text=True, check=False)
    return result.returncode, f"{result.stdout or ''}{result.stderr or ''}"


def run_mgmt_subcmd_retry(port: str, subcmd: str, attempts: int = 5,
                            delay: float = 2.0) -> tuple[int, str]:
    """
    Retrying variant: right after a new image boots the main loop may not serve management sessions
    yet (radio init is still running), so the first attempts time out. That is normal, not a
    """
    rc, out = 1, ""

    for i in range(attempts):
        rc, out = run_mgmt_subcmd(port, subcmd)
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
    """Read and validate the SHA256 TLV at the MCUboot header's declared boundary."""
    data = path.read_bytes()
    if len(data) < 32:
        return None
    magic, _, header_size, protected_size, image_size = struct.unpack_from("<IIHHI", data)
    if magic != 0x96F3B83D or header_size < 32:
        return None
    offset = header_size + image_size
    if protected_size:
        if offset + protected_size > len(data) or protected_size < 4:
            return None
        if struct.unpack_from("<HH", data, offset) != (0x6908, protected_size):
            return None
        offset += protected_size
    if offset + 4 > len(data):
        return None
    tlv_magic, tlv_size = struct.unpack_from("<HH", data, offset)
    if tlv_magic != 0x6907 or tlv_size < 4 or offset + tlv_size > len(data):
        return None
    expected = hashlib.sha256(data[:offset]).hexdigest()
    end = offset + tlv_size
    offset += 4
    found = None
    while offset < end:
        if offset + 4 > end:
            return None
        tag, size = struct.unpack_from("<HH", data, offset)
        offset += 4
        if offset + size > end:
            return None
        if tag == 0x10:
            if size != 32 or found is not None:
                return None
            found = data[offset:offset + size].hex()
        offset += size
    return found if found == expected else None


def serial_of_port(port: str) -> str | None:
    """Resolve a tty path to its ZLRS USB serial, retained across re-enumeration."""
    paths = [port] + find_by_id(APP_PORT_HINT)
    for path in paths:
        if os.path.realpath(path) != os.path.realpath(port):
            continue
        name = os.path.basename(path)
        upper = name.upper()
        for marker in ("_ZLRS_MCUBOOT_", "_ZLRS_"):
            if marker in upper and "-IF" in upper:
                return name[upper.index(marker) + len(marker):upper.rindex("-IF")]
    return None


def run_trigger(port: str, baud: int) -> int:
    script = repo_root() / "tools" / "zlrs.py"
    step(1, 6, "Enter recovery mode")
    print(fields([("Management", port)]))
    return subprocess.run(
        [sys.executable, str(script), "recovery", "--port", port, "--baud", str(baud)], check=False
    ).returncode


def configure_parser(parser):
    parser.add_argument("--port", help="application management port for the recovery trigger")
    parser.add_argument("--serial", help="USB serial substring; retained across reboot")
    parser.add_argument("--recovery-port", help="explicit MCUboot recovery port (...-if00)")
    parser.add_argument("--image", help="signed image; default: <workspace>/<build-dir>/app/zephyr/zephyr.signed.bin")
    parser.add_argument("--baud", type=bounded_int(1, 4000000), default=DEFAULT_BAUD,
                        help="serial connection baud rate")
    parser.add_argument("--client", choices=["auto", "smpmgr", "none"], default="auto",
                        help="auto/smpmgr require smpmgr; none prints a manual upload command")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate image and print the plan without opening ports or rebooting")
    parser.add_argument("--no-trigger", action="store_true", help="device is already in MCUboot recovery")
    parser.add_argument("--no-wait", action="store_true",
                        help="return after upload; running image is not verified or confirmed")
    parser.add_argument("--timeout", type=positive_seconds, default=15.0,
                        help="USB port discovery timeout in seconds")
    parser.add_argument("--no-confirm-check", action="store_true",
                        help="verify the image but leave it unconfirmed for rollback testing")
    return parser


def main(args=None, parser=None) -> int:
    if args is None:
        parser = argparse.ArgumentParser(
            description="Upgrade ZLRS with a signed MCUboot image, verify it, then confirm.",
            formatter_class=HelpFormatter,
            epilog="""Examples:
      %(prog)s --serial <serial> --image <build>/app/zephyr/zephyr.signed.bin
      %(prog)s --no-trigger --serial <serial> --image <signed.bin>
      %(prog)s --dry-run --image <signed.bin>

    A single connected module is selected automatically. With multiple devices, use --serial.
    Application management: ...-if02; MCUboot recovery: ...-if00.
    Default image follows ZLRS_WS and ZLRS_BUILD_DIR. BOOTSEL installs only the bootloader UF2.
    Success means the running image hash matches and the device accepted confirmation.
    """)
        configure_parser(parser)
        args = parser.parse_args()
    # Complete preflight before asking the application to reboot.
    image = (Path(args.image).expanduser() if args.image else default_image()).resolve()
    if not image.is_file():
        parser.error(f"no signed image at {image}; build with zlrs-build or pass --image")
    want_hash = image_hash_of(image)
    if want_hash is None:
        parser.error("image has no valid MCUboot SHA256 TLV or its content hash is wrong")
    client = None if args.client == "none" else find_client("smpmgr")
    if client is None and args.client != "none" and not args.dry_run:
        parser.error("smpmgr not found; activate the workspace venv or install smpmgr")
    if not args.no_trigger and not args.dry_run:
        try:
            import serial  # pyserial
        except ImportError:
            parser.error("pyserial required; activate the workspace venv or install pyserial")

    if not args.no_trigger and not args.port:
        ports = app_mgmt_ports(args.serial)
        if len(ports) == 1:
            args.port = ports[0]
        elif not args.dry_run:
            parser.error("no unique application management port; use --serial or --port:\n  "
                         + "\n  ".join(ports))
    identity_port = args.port or args.recovery_port
    if args.no_trigger and not identity_port:
        recovery = [p for p in find_by_id(RECOVERY_PORT_HINT, serial=args.serial)
                    if p.lower().endswith("-if00")]
        if len(recovery) == 1:
            identity_port = recovery[0]
            args.recovery_port = identity_port
    identity = serial_of_port(identity_port) if identity_port else None
    if identity:
        if args.serial and args.serial.upper() not in identity.upper():
            parser.error("--serial does not match the explicitly selected port")
        args.serial = identity
    if args.port and args.recovery_port:
        recovery_identity = serial_of_port(args.recovery_port)
        if identity and recovery_identity and identity.upper() != recovery_identity.upper():
            parser.error("application and recovery ports belong to different devices")
    if not args.serial and not args.dry_run:
        parser.error("cannot determine USB serial; pass --serial to track this device across reboot")

    heading("ZLRS Firmware Update")
    print(fields([
        ("Device", args.serial or args.port or "automatic selection"),
        ("Image", image), ("Size", f"{image.stat().st_size:,} bytes"),
        ("SHA256", want_hash),
    ]), flush=True)
    if args.dry_run:
        print("[dry-run] trigger recovery -> wait for the same device -> read inactive slot")
        command = slot_upload_command(client or "smpmgr", args.recovery_port or "<recovery-port>",
                                      image, args.baud, 1)
        print("[dry-run] " + shlex.join(command))
        print("[dry-run] slot shown is an example; the real slot is read before upload")
        print("[dry-run] " + ("return after upload" if args.no_wait else
              "wait for management port -> verify SHA256 -> " +
              ("leave unconfirmed" if args.no_confirm_check else "confirm")))
        return 0

    if not args.no_trigger:
        if run_trigger(args.port, args.baud):
            notice("error", "recovery trigger failed; check the management port", file=sys.stderr)
            return 1
    else:
        step(1, 6, "Enter recovery mode (already in recovery)")
    if args.recovery_port is None:
        step(2, 6, f"Find recovery port (timeout {args.timeout:g} s)")
        args.recovery_port = wait_for_by_id(RECOVERY_PORT_HINT, args.timeout, serial=args.serial,
                                               exact_serial=identity is not None)
        if args.recovery_port is None:
            notice("error", "recovery port did not appear; check USB or pass --recovery-port", file=sys.stderr)
            return 1
    else:
        step(2, 6, "Use selected recovery port")
    print(fields([("Recovery", args.recovery_port)]))
    recovery_identity = serial_of_port(args.recovery_port)
    if recovery_identity:
        args.serial = recovery_identity
        identity = recovery_identity
    if client is None:
        print("Manual upload: read image state first and choose physical slot N (CLI --slot N+1).")
        base = ["smpmgr", "--port", args.recovery_port, "--baudrate", str(args.baud)]
        print("  " + shlex.join(base + ["image", "state-read"]))
        print("  " + shlex.join(base + ["upgrade", "--slot", "<N+1>", str(image)]))
        print("After boot, compare image-hash with SHA256 above, then run zlrs.py confirm.")
        notice("info", "Manual upload prepared; firmware has not been uploaded or confirmed")
        return 0
    target_slot = inactive_slot(client, args.recovery_port, args.baud)
    if target_slot is None:
        notice("error", "cannot determine slot from MCUboot image state; upload stopped", file=sys.stderr)
        return 1
    cmd = slot_upload_command(client, args.recovery_port, image, args.baud, target_slot)
    step(3, 6, f"Upload image to slot {target_slot} and reboot")
    # Let smpmgr render its progress while the upload is running.
    result = subprocess.run(cmd, check=False)
    if result.returncode:
        notice("error", "upload failed; check client output and MCUboot image-state support", file=sys.stderr)
        return result.returncode
    if args.no_wait:
        notice("warn", "Upload completed; verification and confirmation are pending. Test upgrades roll back.")
        return 0
    step(4, 6, f"Wait for application (timeout {args.timeout:g} s)")
    app_port = wait_for_by_id(APP_PORT_HINT, args.timeout, exclude=RECOVERY_PORT_HINT,
                             serial=args.serial, exact_serial=identity is not None)
    if app_port is None:
        notice("error", "application did not return; image remains unconfirmed", file=sys.stderr)
        return 1
    print(fields([("Management", app_port)]))
    step(5, 6, "Verify running image")
    rc, out = run_mgmt_subcmd_retry(app_port, "image-hash")
    got_hash = parse_hex_hash(out) if rc == 0 else None
    print(fields([("Expected", want_hash), ("Running", got_hash or "unavailable")]))
    if got_hash != want_hash:
        if out.strip():
            print(out.rstrip(), file=sys.stderr)
        notice("error", "running image hash does not match; image remains unconfirmed", file=sys.stderr)
        return 1
    if args.no_confirm_check:
        step(6, 6, "Leave image unconfirmed (--no-confirm-check)")
        notice("warn", "Image verified; test upgrades roll back without confirmation")
        return 0
    step(6, 6, "Confirm verified image")
    rc, out = run_mgmt_subcmd_retry(app_port, "confirm")
    if rc:
        print(out.rstrip(), file=sys.stderr)
        notice("error", "confirmation failed; image remains unconfirmed", file=sys.stderr)
        return 1
    print()
    notice("ok", "Upgrade succeeded: running image verified and confirmed")
    return 0
