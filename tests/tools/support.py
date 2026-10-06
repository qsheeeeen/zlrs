# SPDX-License-Identifier: Apache-2.0
"""Shared fixtures; module tests live in their matching test_<module>.py."""

import contextlib
import hashlib
import io
from pathlib import Path
import struct
import sys
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools"
sys.path.insert(0, str(TOOLS))


def invoke(module, *args):
    output = io.StringIO()
    with patch.object(sys, "argv", [module.__file__, *args]), \
            contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
        try:
            rc = module.main()
        except SystemExit as exc:
            rc = exc.code
    return rc, output.getvalue()


def image_bytes(protected=False):
    body = b"test firmware"
    protected_data = struct.pack("<HH", 0x6908, 4) if protected else b""
    header = struct.pack("<IIHHI", 0x96F3B83D, 0, 32, len(protected_data), len(body))
    content = header.ljust(32, b"\0") + body + protected_data
    digest = hashlib.sha256(content).digest()
    return content + struct.pack("<HHHH", 0x6907, 40, 0x10, 32) + digest
