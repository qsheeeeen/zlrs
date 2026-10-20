# SPDX-License-Identifier: Apache-2.0
"""The firmware placement gate must reject broken closures, not silently pass them."""

import unittest
from check_ram import analyze

SYMBOLS = """20000000 l     F .ramfunc 00000010 launch_timer_isr
20000010 l     F .ramfunc 00000010 launch_command_isr
20000020 g     F .ramfunc 00000010 zlrs_slot_timer_now_us
10000000 g     F text 00000010 flash_helper
"""
SECTIONS = "  1 .ramfunc 00000030 20000000 10000000 00010000 2**2\n"
CODE = """20000000 <launch_timer_isr>:
20000000: blx r3
20000002: bx lr
20000010 <launch_command_isr>:
20000010: bl 20000020 <zlrs_slot_timer_now_us>
20000014: b.n 20000018 <launch_command_isr+0x8>
20000018: pop {r4, pc}
20000020 <zlrs_slot_timer_now_us>:
20000020: bx lr
10000000 <flash_helper>:
10000000: bx lr
"""


class RamAuditTests(unittest.TestCase):
    def test_complete_ram_closure_with_declared_callback(self):
        result = analyze(SYMBOLS, CODE, SECTIONS)
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(result["timed_launch_closure"]), 3)
        self.assertEqual(result["ram_code_bytes"], 48)
        self.assertEqual(result["declared_callbacks"], [
            {"caller": "launch_timer_isr", "target": "launch_command_isr"}])

    def test_direct_call_and_tail_call_to_flash_fail(self):
        for opcode in ("bl", "blne", "blx", "blxeq", "b.w", "bne.w", "bls.n"):
            with self.subTest(opcode=opcode):
                code = CODE.replace("bl 20000020 <zlrs_slot_timer_now_us>",
                                    f"{opcode} 10000000 <flash_helper>")
                result = analyze(SYMBOLS, code, SECTIONS)
                self.assertIn("flash_helper executes from text", result["errors"])

    def test_missing_root_cannot_pass(self):
        result = analyze(SYMBOLS.replace("launch_command_isr", "wrong_callback"), CODE, SECTIONS)
        self.assertTrue(result["errors"])

    def test_indirect_call_outside_declared_callback_fails(self):
        code = CODE.replace("bl 20000020 <zlrs_slot_timer_now_us>", "blx r4")
        self.assertTrue(analyze(SYMBOLS, code, SECTIONS)["errors"])

    def test_unknown_target_and_veneer_require_review(self):
        for instruction in ("bl 12345678 <missing>", "ldr.w pc, [pc, #0]"):
            with self.subTest(instruction=instruction):
                code = CODE.replace("bl 20000020 <zlrs_slot_timer_now_us>", instruction)
                self.assertTrue(analyze(SYMBOLS, code, SECTIONS)["errors"])

    def test_ram_budget_and_missing_sections_fail(self):
        self.assertTrue(analyze(SYMBOLS, CODE, SECTIONS, max_ram_text=47)["errors"])
        self.assertTrue(analyze(SYMBOLS, CODE, "")["errors"])

    def test_flash_root_is_rejected(self):
        symbols = SYMBOLS.replace("F .ramfunc 00000010 zlrs_slot_timer_now_us",
                                  "F text 00000010 zlrs_slot_timer_now_us")
        self.assertTrue(analyze(symbols, CODE, SECTIONS)["errors"])


if __name__ == "__main__":
    unittest.main()
