#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Audit the RP2350 timed-launch call closure in the linked ARM ELF."""

import argparse
import json
from pathlib import Path
import re
import subprocess

ROOTS = ("launch_timer_isr", "launch_command_isr", "zlrs_slot_timer_now_us")
RAM_SECTIONS = {".ramfunc", ".ram_text_reloc"}
# Alarm 3 invokes this prepared-command callback. Other indirect calls fail closed.
CALLBACKS = {"launch_timer_isr": "launch_command_isr"}
BRANCH = re.compile(r"b(?:lx|l|x)?(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?(?:\.[nw])?$")


def analyze(symbol_table, disassembly, section_table, max_ram_text=32768):
    functions = {}
    for line in symbol_table.splitlines():
        match = re.match(r"^([0-9a-f]+)\s+.*\bF\s+(\S+)\s+([0-9a-f]+)\s+(\S+)\s*$", line)
        if match:
            address, section, size, name = match.groups()
            functions[int(address, 16)] = dict(name=name, section=section, size=int(size, 16))
    by_name = {}
    for address, function in functions.items():
        by_name.setdefault(function["name"], []).append(address)
    errors = []
    roots = []
    for name in ROOTS:
        addresses = by_name.get(name, [])
        if len(addresses) != 1:
            errors.append(f"expected one definition of {name}, got {len(addresses)}")
        else:
            roots.extend(addresses)
    edges = {address: set() for address in functions}
    indirect = {}
    current = None
    for line in disassembly.splitlines():
        label = re.match(r"^([0-9a-f]+) <[^>]+>:$", line)
        if label:
            current = int(label[1], 16)
            continue
        instruction = re.match(r"^\s*[0-9a-f]+:\s+(\S+)\s+(.*)$", line)
        if current not in functions or not instruction:
            continue
        opcode, operands = instruction.groups()
        if BRANCH.fullmatch(opcode) or opcode in {"cbz", "cbnz"}:
            target = re.search(r"\b([0-9a-f]+) <([^>]+)>", operands)
            if target:
                address = int(target[1], 16)
                function = functions[current]
                if not current <= address < current + function["size"]:
                    edges[current].add(address)
            elif not (opcode.startswith("bx") and operands.strip() == "lr"):
                indirect.setdefault(current, []).append(f"{opcode} {operands}")
        elif ((opcode.startswith("ldr") or opcode.startswith("mov")) and
              re.match(r"pc\b", operands)) or opcode in {"tbb", "tbh"}:
            # A veneer or unrecognized computed transfer requires explicit review.
            indirect.setdefault(current, []).append(f"{opcode} {operands}")
    visited = set()
    pending = list(roots)
    declared_callbacks = []
    while pending:
        address = pending.pop()
        if address in visited:
            continue
        visited.add(address)
        function = functions.get(address)
        if function is None:
            errors.append(f"unknown branch target 0x{address:x}")
            continue
        if function["section"] not in RAM_SECTIONS:
            errors.append(f"{function['name']} executes from {function['section']}")
        transfers = indirect.get(address, [])
        if transfers:
            callback = CALLBACKS.get(function["name"])
            # Only the one register call used by the alarm callback is permitted.
            if (callback and len(transfers) == 1 and
                    re.fullmatch(r"blx r(?:[0-9]|1[0-2])", transfers[0]) and
                    len(by_name.get(callback, [])) == 1):
                pending.extend(by_name[callback])
                declared_callbacks.append({"caller": function["name"], "target": callback})
            else:
                errors.append(f"{function['name']}: unreviewed transfer {transfers}")
        pending.extend(edges[address])
    ram_bytes = 0
    for line in section_table.splitlines():
        match = re.match(r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+", line)
        if match and match[1] in RAM_SECTIONS:
            ram_bytes += int(match[2], 16)
    if ram_bytes == 0:
        errors.append("RAM code sections are missing or empty")
    if ram_bytes > max_ram_text:
        errors.append(f"RAM code uses {ram_bytes} bytes, budget is {max_ram_text}")
    flash_edges = []
    for source, targets in edges.items():
        if functions[source]["section"] in RAM_SECTIONS:
            for target in sorted(targets):
                if target in functions and functions[target]["section"] not in RAM_SECTIONS:
                    flash_edges.append({"caller": functions[source]["name"],
                                        "callee": functions[target]["name"]})
    return {"errors": sorted(errors), "ram_code_bytes": ram_bytes,
            "ram_code_budget": max_ram_text, "declared_callbacks": declared_callbacks,
            "timed_launch_closure": [dict(address=f"0x{address:x}", **functions[address])
                                     for address in sorted(visited) if address in functions],
            "other_ram_indirect_transfers": [
                {"function": functions[address]["name"], "transfers": transfers}
                for address, transfers in sorted(indirect.items())
                if address not in visited and functions[address]["section"] in RAM_SECTIONS],
            "other_ram_to_flash_edges": sorted(flash_edges, key=lambda row: (row["caller"], row["callee"]))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--max-ram-text", type=int, default=32768)
    args = parser.parse_args()
    if args.max_ram_text <= 0:
        parser.error("--max-ram-text must be positive")
    def dump(*options):
        return subprocess.check_output([args.objdump, *options, str(args.elf)], text=True)
    report = analyze(dump("-t"), dump("-d", "--no-show-raw-insn"), dump("-h"), args.max_ram_text)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    for error in report["errors"]:
        print(f"RAM audit: {error}")
    print(f"RAM audit: {len(report['timed_launch_closure'])} timed-launch functions; "
          f"{report['ram_code_bytes']}/{args.max_ram_text} RAM code bytes")
    return bool(report["errors"])


if __name__ == "__main__":
    raise SystemExit(main())
