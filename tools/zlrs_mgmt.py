#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Product operations over native MCUmgr; OS, IMG and FS use standard smpclient requests."""
import argparse
import glob
from collections import namedtuple
from contextlib import contextmanager
import asyncio
import os
import sys

from zlrs_cli import HelpFormatter, bounded_int, positive_seconds, fields, heading, notice
from zlrs_smp import request_type

STATUS_FIELDS = ('role', 'state', 'flags', 'dev_id', 'peer_id', 'lq_percent', 'band', 'region',
 'tx_power_dbm', 'data_baud', 'profile', 'rssi_dbm', 'peer_rssi_dbm', 'peer_lq_percent',
 'active_power_dbm', 'power_floor_dbm', 'temperature_centi_c', 'tx_frames', 'rx_frames',
 'rx_crc_errors', 'rx_bad_tag', 'tx_retries', 'data_drop_bytes', 'missed_slots', 'radio_fail',
 'frequency_hz', 'hop_index', 'frame_period_us', 'radio_temperature_centi_c',
 'radio_xosc_temperature_centi_c', 'radio_vbat_mv', 'radio_version_major',
 'radio_version_minor', 'radio_chip_mode', 'radio_command_status', 'radio_reset_status',
 'radio_valid', 'radio_errors', 'radio_irq', 'data_phy',
 'lq_generation', 'peer_lq_generation', 'power_raises', 'power_lowers', 'power_fallbacks', 'power_changed_ms', 'peer_feedback_ms', 'power_rssi_dbm')
Status = namedtuple("Status", STATUS_FIELDS)

CMD_ENTER = 0x00
CMD_STATUS = 0x01
CMD_PAIR = 0x02
CMD_ERASE_PAIR = 0x03
CMD_SET_KEY_HEX = 0x04
CMD_SET_PHRASE = 0x05
CMD_BUILD = 0x06
CMD_IMAGE_HASH = 0x07
CMD_SET_BAUD = 0x08
CMD_SET_REGION = 0x09
CMD_SET_ROLE = 0x0A
CMD_SET_PROFILE = 0x0B
CMD_SET_BAND = 0x0C
CMD_NEW_DEV_ID = 0x0D
CMD_EXIT = 0x0E
CMD_FACTORY_RESET = 0x0F
CMD_RECOVERY = 0x10
CMD_CONFIRM = 0x11
CMD_LOG_INFO = 0x12
CMD_DEBUG_REPORT = 0x14
CMD_OBSERVE = 0x15
ONLINE_COMMANDS = {CMD_STATUS, CMD_BUILD, CMD_IMAGE_HASH}

# No-payload commands share registration and encoding metadata.
SIMPLE_COMMANDS = {
    "enter": (CMD_ENTER, "enter management mode"),
    "status": (CMD_STATUS, "read link, modulation, signal/power and configuration"),
    "pair": (CMD_PAIR, "start pairing on this device (start the peer too)"),
    "erase-pair": (CMD_ERASE_PAIR, "erase the peer pairing record"),
    "new-dev-id": (CMD_NEW_DEV_ID, "make a fresh device id (drops the pairing record)"),
    "factory-reset": (CMD_FACTORY_RESET, "erase configuration and pairing"),
    "recovery": (CMD_RECOVERY, "reboot into MCUboot serial recovery"),
    "build": (CMD_BUILD, "read the build string"),
    "image-hash": (CMD_IMAGE_HASH, "read the MCUboot hash of the running image (hex)"),
    "confirm": (CMD_CONFIRM, "confirm this upgrade (no confirm means a timeout rollback)"),
    "exit": (CMD_EXIT, "leave management mode and restore the radio"),
}

DEBUG_REPORTS = {"link": 0, "rate": 1, "radio": 2, "power": 3}

COMMAND_MESSAGES = {
    CMD_ENTER: "Management mode entered; radio paused",
    CMD_PAIR: "Pairing started",
    CMD_ERASE_PAIR: "Pairing record erased",
    CMD_SET_KEY_HEX: "Root key saved",
    CMD_SET_PHRASE: "Binding phrase saved",
    CMD_SET_BAUD: "Data UART baud rate saved",
    CMD_SET_REGION: "Region saved",
    CMD_SET_ROLE: "Role saved",
    CMD_SET_PROFILE: "Security profile saved",
    CMD_SET_BAND: "Data band saved; reboot to apply",
    CMD_NEW_DEV_ID: "New device ID saved; pairing record cleared",
    CMD_FACTORY_RESET: "Factory settings restored",
    CMD_CONFIRM: "Running firmware confirmed",
    CMD_EXIT: "Management mode exited; radio resumed",
}


STATE_NAMES = dict(enumerate(("unconfigured", "unpaired", "pairing", "searching", "connected")))
REGION_NAMES = dict(enumerate(("FCC915", "EU868", "CN470", "CN433")))

def decode_status(values) -> Status:
    return values if isinstance(values, Status) else Status(**values)


def read_status(client) -> Status:
    return decode_status(client.request(CMD_STATUS)["status"])


def status_text(payload) -> str:
    status = decode_status(payload)
    flags = status.flags
    modulation = {0: "FLRC 2.6 Mbps", 1: "FLRC 650 kbps", 2: "FLRC 325 kbps",
                  3: "LoRa SF6 / BW 1 MHz", 4: "LoRa SF7 / BW 1 MHz",
                  5: "FLRC 1.3 Mbps"}.get(status.data_phy)
    if modulation is None:
        modulation = f"unknown profile ({status.data_phy})"
    sections = [
        ("Link", [("Role", "Leader" if status.role == 0 else "Follower"),
                  ("State", STATE_NAMES.get(status.state, str(status.state))),
                  ("Paired", "yes" if flags & 0x02 else "no"),
                  ("Session", "active" if flags & 0x04 else "inactive")]),
        ("Radio", [
            ("Band mode", "auto" if flags & 0x10 else "fixed"),
            ("Active band", "sub-GHz" if flags & 0x08 else "2.4 GHz"),
            ("Modulation", modulation),
            ("Frequency", f"{status.frequency_hz / 1000000:.3f} MHz"),
            ("Hop index", str(status.hop_index)),
            ("Frame period", f"{status.frame_period_us / 1000:.3f} ms"),
            ("Capture", "searching" if flags & 0x20 else "ready"),
            ("Transition", "scheduled" if flags & 0x40 else "none"),
        ]),
        ("Signal / Power", [
            ("Local RSSI", f"{status.rssi_dbm} dBm" if status.rssi_dbm else "unavailable"),
            ("Local LQ", f"{status.lq_percent}%" if status.lq_percent <= 100 else "pending"),
            ("Peer RSSI", f"{status.peer_rssi_dbm} dBm" if status.peer_rssi_dbm else "unavailable"),
            ("Peer LQ", (f"{status.peer_lq_percent}%" if status.peer_lq_percent <= 100 else "pending")
             if status.peer_rssi_dbm else "unavailable"),
            ("TX power", f"{status.active_power_dbm} dBm"),
            ("TX range", f"{status.power_floor_dbm}..{status.tx_power_dbm} dBm"),
            ("Power control", "debug fixed" if flags & 0x80 else "automatic"),
        ]),
        ("Traffic / Errors", [
            ("RF frames", f"TX {status.tx_frames}, RX {status.rx_frames}"),
            ("ARQ retries", str(status.tx_retries)),
            ("CRC errors", str(status.rx_crc_errors)),
            ("Auth errors", str(status.rx_bad_tag)),
            ("Radio failures", str(status.radio_fail)),
            ("Missed slots", str(status.missed_slots)),
            ("Dropped data", f"{status.data_drop_bytes} bytes"),
        ]),
        ("Device / Configuration", [
            ("Hardware", "HF (868/915 MHz)" if status.band else "LF (470/433 MHz)"),
            ("Region", REGION_NAMES.get(status.region, str(status.region))),
            ("Security profile", f"P{status.profile}"),
            ("Device ID", status.dev_id.hex()),
            ("Peer ID", status.peer_id.hex()),
            ("Data UART", f"{status.data_baud or 115200} baud"),
            ("Root key", "configured" if flags & 0x01 else "missing"),
        ]),
    ]
    valid = status.radio_valid
    radio_temp = status.radio_temperature_centi_c
    xosc_temp = status.radio_xosc_temperature_centi_c
    vbat = status.radio_vbat_mv
    mode, command, reset = (status.radio_chip_mode, status.radio_command_status, status.radio_reset_status)
    mode_names = {0: "sleep", 1: "standby RC", 2: "standby XOSC",
                  3: "frequency synthesis", 4: "RX", 5: "TX"}
    command_names = {0: "failed", 1: "invalid command", 2: "OK", 3: "read data"}
    reset_names = {0: "cleared", 1: "power-on/brown-out", 2: "reset pin",
                   5: "NSS wakeup", 6: "RTC wakeup"}
    errors = status.radio_errors
    error_names = ("HF XOSC start", "LF XOSC start", "PLL lock", "LF RC calibration",
                   "HF RC calibration", "PLL calibration", "AAF calibration",
                   "image calibration", "chip busy", "missing front-end calibration",
                   "measurement ADC calibration", "PA offset calibration",
                   "PPF calibration", "SRC calibration", "SRC saturation",
                   "SRC tolerance")
    error_text = ", ".join(name for bit, name in enumerate(error_names)
                           if errors & (1 << bit)) or "none"
    irq = status.radio_irq
    sections.append(("Sensors", [
        ("MCU chip temp", "unavailable" if status.temperature_centi_c == -32768
         else f"{status.temperature_centi_c / 100:.2f} C"),
        ("LR2021 chip temp", f"{radio_temp / 100:.2f} C" if valid & 0x01 else "unavailable"),
        ("LR2021 XOSC temp", f"{xosc_temp / 100:.2f} C" if valid & 0x02 else "unavailable"),
        ("LR2021 supply", f"{vbat / 1000:.3f} V ({vbat} mV)" if valid & 0x04 else "unavailable"),
    ]))
    sections.append(("LR2021 snapshot", [
        ("Chip version", f"major 0x{status.radio_version_major:02x}, minor 0x{status.radio_version_minor:02x}"
         if valid & 0x08 else "unavailable"),
        ("Chip mode", mode_names.get(mode, f"unknown ({mode})") if valid & 0x10 else "unavailable"),
        ("Last command", command_names.get(command, f"unknown ({command})")
         if valid & 0x10 else "unavailable"),
        ("Reset source", reset_names.get(reset, f"unknown ({reset})")
         if valid & 0x10 else "unavailable"),
        ("IRQ status", f"0x{irq:08x}" if valid & 0x10 else "unavailable"),
        ("Chip errors", f"0x{errors:04x} ({error_text})" if valid & 0x20 else "unavailable"),
    ]))
    return "\n\n".join(title + "\n" + fields(rows) for title, rows in sections)



class ManagementError(ValueError):
    def __init__(self, command, result):
        super().__init__(f"operation {command}: {os.strerror(-result) if result < 0 else result}")
        self.command, self.status = command, result


class ManagementClient:
    """One standard SMP serial connection, with no application framing or retries."""
    def __init__(self, port, baud=115200, timeout=10):
        self.port, self.baud, self.timeout = port, baud, timeout
        self.entered = False

    def __enter__(self):
        from smpclient import SMPClient
        from smpclient.transport.serial import SMPSerialTransport
        self.runner = asyncio.Runner()
        self.client = SMPClient(SMPSerialTransport(baudrate=self.baud), self.port,
                                timeout_s=self.timeout)
        try:
            self.runner.run(self.client.__aenter__())
        except BaseException:
            self.runner.close()
            raise
        return self

    def __exit__(self, *exc):
        try:
            self.runner.run(self.client.__aexit__(*exc))
        finally:
            self.runner.close()

    def _request(self, request):
        from smpclient.generics import success
        response = self.runner.run(self.client.request(request))
        if not success(response):
            raise ValueError(f"SMP rejected request: {response}")
        return response

    def image_states(self):
        from smpclient.requests.image_management import ImageStatesRead
        return self._request(ImageStatesRead()).images

    def request(self, command, payload=b""):
        from smpclient.requests.image_management import ImageStatesWrite
        from smpclient.requests.os_management import OSApplicationInfoRead, ResetWrite
        if command == CMD_BUILD:
            # Zephyr's native OS info reports application metadata via its format fields.
            return self._request(OSApplicationInfoRead(format="a")).output
        if command == CMD_IMAGE_HASH:
            images = self.image_states()
            active = next((image for image in images if image.active), None)
            if active is None or active.hash is None or len(active.hash) != 32:
                raise ValueError("native IMG response has no active image hash")
            return bytes(active.hash)
        if command == CMD_CONFIRM:
            self._request(ImageStatesWrite(confirm=True))
            return {}
        if command == CMD_RECOVERY:
            self._request(ResetWrite(boot_mode=1))
            return {}
        response = self._request(request_type(66, {"op": command, "value": payload}))
        if response.result != 0:
            raise ManagementError(command, response.result)
        if command == CMD_ENTER:
            self.entered = True
        elif command == CMD_EXIT:
            self.entered = False
        return response.model_extra or {}

    def download(self, path):
        return self.runner.run(self.client.download_file(path))


@contextmanager
def management_session(port, baud, timeout, enter=True, keep=False, observe=False):
    with ManagementClient(port, baud, timeout) as client:
        if enter and not observe:
            client.request(CMD_ENTER)
        elif not observe:
            client.entered = True  # --no-enter uses an existing maintenance session.
        try:
            yield client
        finally:
            if not keep and client.entered:
                client.request(CMD_EXIT)


def read_debug_report(client, report_id: int) -> bytes:
    return client.request(CMD_DEBUG_REPORT, bytes([report_id]))["report"].encode()


def device_heading(port: str) -> None:
    """Separate each target block in multi-device output."""
    print("-" * 72)
    heading(f"Device: {port}")


def session_options(command, enter=True, keep=False, refresh=False):
    """Share the maintenance decision between execution and dry-run output."""
    online = command in ONLINE_COMMANDS and enter and not refresh and not keep
    return {
        "enter": enter and command not in (CMD_ENTER, CMD_EXIT, CMD_RECOVERY) and not online,
        "keep": keep or command in (CMD_ENTER, CMD_RECOVERY),
        "observe": online,
    }


def talk(port, baud, operation, timeout, enter=True, quiet=False, keep=False, refresh=False):
    command, value = operation
    try:
        with management_session(port, baud, timeout,
                                **session_options(command, enter, keep, refresh)) as client:
            if command == CMD_DEBUG_REPORT:
                print(read_debug_report(client, value[0]).decode(), end="")
                return 0
            response = client.request(command, value)
            if command == CMD_STATUS:
                heading("ZLRS Status")
                age = (response["uptime_ms"] - response["hardware_ms"]) & 0xffffffff
                print(fields([("Device uptime", f"{response['uptime_ms']} ms"),
                              ("Chip observations age", f"{age} ms" if response["hardware_ms"] else "unavailable")]))
                print(status_text(response["status"]))
            elif command == CMD_BUILD:
                print(response)
            elif command == CMD_IMAGE_HASH:
                print(response.hex())
            elif command == CMD_RECOVERY:
                notice("info", "Recovery requested; waiting for USB re-enumeration")
            elif not quiet:
                notice("ok", COMMAND_MESSAGES.get(command, "Command accepted"))
        return 0
    except ImportError:
        notice("error", "smpclient serial support required; install smpmgr", file=sys.stderr)
        return 2
    except (OSError, ValueError, TimeoutError) as exc:
        notice("error", str(exc), file=sys.stderr)
        return 1


def dump_logs(port, baud, timeout, out_dir, keep=False, no_enter=False):
    os.makedirs(out_dir, exist_ok=True)
    try:
        with management_session(port, baud, timeout, enter=not no_enter, keep=keep) as client:
            inventory = client.request(CMD_LOG_INFO)["files"]
            for item in inventory:
                data = client.download(item["path"])
                if len(data) != item["size"]:
                    raise ValueError("log size changed during export")
                path = os.path.join(out_dir, os.path.basename(item["path"]) + ".bin")
                with open(path, "wb") as output:
                    output.write(data)
                print(f"{len(data):>10,} bytes  {path}")
        return 0
    except (OSError, ValueError, TimeoutError) as exc:
        notice("error", str(exc), file=sys.stderr)
        return 1


def find_by_id(hint: str, exclude: str | None = None, serial: str | None = None) -> list[str]:
    """Find serial ports by their /dev/serial/by-id name (case-insensitive)."""
    found = []
    for path in sorted(glob.glob("/dev/serial/by-id/*")):
        name = os.path.basename(path).upper()
        if hint.upper() not in name:
            continue
        if exclude is not None and exclude.upper() in name:
            continue
        if serial is not None and serial.upper() not in name:
            continue
        found.append(path)
    return found


def app_ports(serial: str | None = None) -> list[str]:
    """Application management CDCs, excluding bootloader and DATA interfaces."""
    return [path for path in find_by_id("ZLRS", exclude="MCUBOOT", serial=serial)
            if path.lower().endswith("-if02")]


def pick_targets(args, parser) -> list[str]:
    """Pick the target modules: explicit --port, then --serial, --all, or the only one present."""
    if args.port:
        return [args.port]

    ports = app_ports()

    if args.serial:
        hit = [p for p in ports if args.serial.upper() in p.upper()]

        if not hit:
            parser.error(f"no module with serial {args.serial!r}; present: {ports}")
        if len(hit) > 1:
            parser.error("serial matches several modules; use a longer serial or --port")
        return hit

    if args.all:
        if not ports:
            parser.error("no ZLRS module found")
        return ports

    if not ports:
        parser.error("no ZLRS module found (nothing matching ZLRS under /dev/serial/by-id)")
    if len(ports) > 1:
        parser.error(
            "several modules are plugged in: pick one with --serial / --port, or use --all:\n  "
            + "\n  ".join(ports))
    return [ports[0]]


def operation(command, value=b""):
    return command, value


def add_options(parser, include_list=True):
    target = parser.add_mutually_exclusive_group()

    target.add_argument("--port", help="management port device (second CDC in application mode, e.g. /dev/ttyACM1)")
    target.add_argument("--serial", help="pick a module by USB serial substring (with several plugged in)")
    target.add_argument("--all", action="store_true",
                        help="run this command on every ZLRS module (with several plugged in)")
    if include_list:
        parser.add_argument("--list", action="store_true", help="list the modules present, then exit")
    else:
        parser.set_defaults(list=False)
    parser.add_argument("--baud", type=bounded_int(1, 4000000), default=115200, help="management connection baud rate")
    parser.add_argument("--timeout", type=positive_seconds, default=10.0,
                        help="response timeout per SMP request in seconds (management entry may take several seconds)")
    parser.add_argument("--dry-run", action="store_true", help="print the SMP operation instead of talking to the device")
    parser.add_argument("--no-enter", action="store_true",
                        help="skip the enter-management-mode step (when already in it)")
    parser.add_argument("--keep", action="store_true",
                        help="stay in management mode after the command (the default leaves it, which restores the radio)")
    parser.add_argument("--refresh", action="store_true",
                        help="pause RF to refresh chip measurements for a read-only query")


def add_commands(sub, parents=()):
    commands = {}

    def command(name, **kwargs):
        child = sub.add_parser(name, parents=list(parents), formatter_class=HelpFormatter, **kwargs)
        commands[name] = child
        return child

    for name, (_, description) in SIMPLE_COMMANDS.items():
        command(name, help=description, description=description)
    for name in DEBUG_REPORTS:
        description = ("compare per-link TX power limits and setpoints" if name == "power"
                       else f"read detailed {name} diagnostics")
        command(name, help=f"{description} (debug firmware only)")
    phrase = command("set-phrase", help="derive the root key from a binding phrase", description="derive the root key from a binding phrase")
    phrase.add_argument("phrase", help="1..32 UTF-8 bytes; use the same phrase on both peers")
    key = command("set-key-hex", help="set a 32-byte root key", description="set a 32-byte root key")
    key.add_argument("hex_key", help="64 hex characters")
    baud = command("set-baud", help="set the data UART baud rate", description="set the data UART baud rate")
    baud.add_argument("data_baud", type=bounded_int(9600, 4000000), help="data UART baud rate")
    region = command("set-region", help="select a region compatible with the HF/LF board", description="select a region compatible with the HF/LF board")
    region.add_argument("region", type=int, choices=[0, 1, 2, 3],
                        help="0=FCC915 1=EU868 2=CN470 3=CN433")
    role = command("set-role", help="select the Leader or Follower role",
                   description="Select the timing role; both endpoints send and receive DATA.")
    role.add_argument("role", choices=["leader", "follower"],
                      help="leader provides the clock; follower follows it")
    profile = command("set-profile", help="select the authentication profile", description="select the authentication profile")
    profile.add_argument("profile", choices=["p0", "p1", "p2"],
                         help="P0 has no tag (debug images only), P1 is the production profile, "
                              "P2 is not implemented yet")
    band = command("set-band", help="select automatic switching or a fixed band (reboot required)",
                   description="Save the data band; reboot both peers to apply.")
    band.add_argument("band", choices=["auto", "2.4", "subg"],
                      help="select the data frequency band")
    logs = command("logs", help="dump the flash log store to a directory")
    logs.add_argument("out_dir", nargs="?", default="zlrs-logs",
                      help="where the log files are written (default: ./zlrs-logs)")
    return commands


def main(args=None, parser=None) -> int:
    if args is None:
        parser = argparse.ArgumentParser(
            description="Configure and inspect ZLRS through its management USB port.",
            formatter_class=HelpFormatter)
        add_options(parser)
        sub = parser.add_subparsers(dest="command", title="commands", metavar="COMMAND")
        add_commands(sub)
        args = parser.parse_args()

    if args.list:
        ports = app_ports()

        if not ports:
            notice("warn", "No ZLRS module plugged in")
            return 1
        heading("ZLRS Devices")
        print(f"  {'#':<4}Management port")
        for i, path in enumerate(ports, 1):
            print(f"  {i:<4}{path}")
        print(f"\n  {len(ports)} device(s) connected")
        return 0

    if args.command is None:
        parser.print_help()
        return 2

    # Native FS transfers share one explicit maintenance session.
    if args.command == "logs":
        if args.dry_run:
            print(f"[dry-run] export logs to {args.out_dir}; no files or ports opened")
            if not args.no_enter:
                print(f"enter: {operation(CMD_ENTER)!r}")
            print(f"info : {operation(CMD_LOG_INFO)!r}")
            print("read : standard MCUmgr FS download")
            if not args.keep:
                print(f"exit : {operation(CMD_EXIT)!r}")
            return 0
        targets = pick_targets(args, parser)
        rc = 0
        for port in targets:
            if len(targets) > 1:
                device_heading(port)
            this = dump_logs(port, args.baud, args.timeout,
                             args.out_dir if len(targets) == 1 else os.path.join(args.out_dir,
                                                                                os.path.basename(port)),
                             keep=args.keep, no_enter=args.no_enter)
            if this != 0:
                rc = this
        return rc

    if args.command in SIMPLE_COMMANDS:
        selected = operation(SIMPLE_COMMANDS[args.command][0])
    elif args.command in DEBUG_REPORTS:
        selected = operation(CMD_DEBUG_REPORT, bytes([DEBUG_REPORTS[args.command]]))
    elif args.command == "set-phrase":
        payload = args.phrase.encode()
        if not 1 <= len(payload) <= 32:
            parser.error("binding phrase must contain 1..32 UTF-8 bytes")
        selected = operation(CMD_SET_PHRASE, payload)
    elif args.command == "set-key-hex":
        if len(args.hex_key) != 64 or any(c not in "0123456789abcdefABCDEF" for c in args.hex_key):
            parser.error("the root key must be exactly 64 hex characters")
        selected = operation(CMD_SET_KEY_HEX, args.hex_key.encode())
    elif args.command == "set-baud":
        selected = operation(CMD_SET_BAUD, args.data_baud.to_bytes(4, "little"))
    elif args.command == "set-region":
        selected = operation(CMD_SET_REGION, bytes([args.region]))
    elif args.command == "set-role":
        selected = operation(CMD_SET_ROLE, bytes([0 if args.role == "leader" else 1]))
    elif args.command == "set-profile":
        selected = operation(CMD_SET_PROFILE, bytes([int(args.profile[1])]))
    elif args.command == "set-band":
        selected = operation(CMD_SET_BAND, bytes([{"2.4": 0, "subg": 1, "auto": 2}[args.band]]))
    else:
        parser.error(f"unknown command: {args.command}")

    if args.dry_run:
        session = session_options(selected[0], not args.no_enter, args.keep, args.refresh)
        if session["enter"]:
            print(f"enter: {operation(CMD_ENTER)!r}")
        print(f"cmd  : {selected!r}")
        if args.command != "exit" and not session["keep"] and not session["observe"]:
            print(f"exit : {operation(CMD_EXIT)!r}")
        return 0

    # With several modules plugged in, decide the targets first (--serial / --port / --all).
    targets = pick_targets(args, parser)

    rc = 0
    for port in targets:
        if len(targets) > 1:
            device_heading(port)
        try:
            this = talk(port, args.baud, selected, args.timeout, enter=not args.no_enter, keep=args.keep,
                        refresh=args.refresh)
        except (OSError, ValueError, TimeoutError) as exc:
            notice("error", f"{port}: {exc}", file=sys.stderr)
            this = 1
        if this != 0:
            rc = this

    return rc
