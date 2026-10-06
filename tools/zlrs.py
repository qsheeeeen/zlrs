#!/usr/bin/env python3
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
"""The public entry point for ZLRS device tools."""

import argparse

from zlrs_cli import HelpFormatter, run
import zlrs_mgmt as mgmt
import zlrs_update as update


def build_parser():
    parser = argparse.ArgumentParser(
        description="ZLRS device management and firmware updates.",
        formatter_class=HelpFormatter,
        epilog="""Examples:
  %(prog)s list
  %(prog)s status --serial <serial>
  %(prog)s set-role rx --serial <serial>
  %(prog)s set-phrase 'my binding phrase' --serial <serial>
  %(prog)s update --serial <serial> --image <signed.bin>
  %(prog)s recovery --serial <serial>

Use COMMAND --help for options. A single connected module is selected automatically
for management commands. With several modules, use --serial or --port.
""")
    sub = parser.add_subparsers(dest="command", title="commands", metavar="COMMAND")

    options = argparse.ArgumentParser(add_help=False)
    mgmt.add_options(options, include_list=False)
    commands = mgmt.add_commands(sub, parents=[options])
    for child in commands.values():
        child.set_defaults(handler=mgmt.main, command_parser=child)
    listing = sub.add_parser("list", help="list connected ZLRS management ports",
                             formatter_class=HelpFormatter)
    listing.set_defaults(handler=mgmt.main, list=True, command_parser=listing)
    upgrade = sub.add_parser("update", help="upload, verify and confirm a signed firmware image",
                             description="Upgrade ZLRS through MCUboot recovery.",
                             formatter_class=HelpFormatter)
    update.configure_parser(upgrade)
    upgrade.set_defaults(handler=update.main, command_parser=upgrade)
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    if args.command is None:
        parser.print_help()
        return 2
    # Each module consumes the namespace parsed by its subcommand; no second parse or subprocess.
    return args.handler(args=args, parser=args.command_parser)


if __name__ == "__main__":
    raise SystemExit(run(main))
