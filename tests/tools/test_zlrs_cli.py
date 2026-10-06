# SPDX-License-Identifier: Apache-2.0
import support  # Add the production tools directory to the import path.
import os
import unittest
from unittest.mock import Mock, patch

from zlrs_cli import fields, styled


class OutputTests(unittest.TestCase):
    def test_fields_align_label_separator(self):
        output = fields([("Device", "1234"), ("Size", "1,024 bytes")])
        lines = output.splitlines()
        self.assertEqual(lines[0].index(":"), lines[1].index(":"))

    def test_color_respects_redirects_and_no_color(self):
        stream = Mock()
        with patch.dict(os.environ, {"TERM": "xterm"}, clear=True):
            stream.isatty.return_value = False
            self.assertEqual(styled("ok", "32", stream), "ok")
            stream.isatty.return_value = True
            self.assertEqual(styled("ok", "32", stream), "\033[32mok\033[0m")
            with patch.dict(os.environ, {"NO_COLOR": ""}):
                self.assertEqual(styled("ok", "32", stream), "ok")
