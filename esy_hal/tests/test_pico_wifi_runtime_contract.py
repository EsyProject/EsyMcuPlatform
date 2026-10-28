# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Check Pico network-hook isolation without host emulation of the backend."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "esy_hal_backend/src/pico/esy_hb_wifi.c"


def function_body(source, name):
    """Extract a definition body while allowing nested C statement blocks."""
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source)
    if match is None:
        raise AssertionError(f"Missing function definition: {name}")
    start = match.end()
    depth = 1
    for index in range(start, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[start:index]
    raise AssertionError(f"Unterminated function definition: {name}")


def call_names(body):
    """Return function calls, excluding C control operators."""
    return set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", body)) - {
        "if", "for", "while", "switch", "sizeof", "_Alignof"
    }


class PicoWifiRuntimeContractTests(unittest.TestCase):
    def setUp(self):
        self.source = SOURCE.read_text()
        self.getter = function_body(self.source, "esy_hb_wifi_get_network_runtime")

    def test_query_rejects_null_before_output_and_has_no_native_side_effects(self):
        null_check = re.search(
            r"if\s*\(\s*out_runtime\s*==\s*NULL\s*\)\s*return\s+ESY_ERR_INVALID_ARG\s*;",
            self.getter,
        )
        self.assertIsNotNone(null_check)
        self.assertLess(null_check.end(), self.getter.index("*out_runtime ="))
        self.assertEqual(set(), call_names(self.getter))
        self.assertNotIn("s_ctx", self.getter)
        self.assertRegex(self.getter, r"\.context\s*=\s*NULL\b")
        self.assertRegex(self.getter, r"return\s+ESY_OK\s*;")

    def test_each_hook_guards_native_lifetime_and_only_dispatches_its_sdk_operation(self):
        expected = {
            "progress": "cyw43_arch_poll",
            "enter": "cyw43_arch_lwip_begin",
            "leave": "cyw43_arch_lwip_end",
        }
        for field, operation in expected.items():
            with self.subTest(hook=field):
                binding = re.search(r"\." + field + r"\s*=\s*(\w+)\b", self.getter)
                self.assertIsNotNone(binding)
                name = binding.group(1)
                self.assertRegex(self.source, r"static\s+void\s+" + name + r"\s*\(")
                body = function_body(self.source, name)
                guard = re.search(
                    r"if\s*\(\s*!s_ctx\.initialized\s*\)\s*return\s*;", body
                )
                self.assertIsNotNone(guard)
                self.assertLess(guard.end(), body.index(operation + "("))
                self.assertEqual({operation}, call_names(body))
                self.assertNotRegex(body, r"s_ctx\.\w+\s*(?:=(?!=)|[+*/-]=|\+\+|--)")


if __name__ == "__main__":
    unittest.main()
