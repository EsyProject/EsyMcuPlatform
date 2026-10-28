# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Check native GCC assembly for the STM32F4 stack-free application handoff."""

import argparse
from pathlib import Path
import re


def function_body(assembly, name):
    match = re.search(
        rf"^{name}:\s*$(.*?)^\s*\.size\s+{name}\s*,",
        assembly, re.MULTILINE | re.DOTALL)
    if match is None:
        raise ValueError(f"Missing native function: {name}")
    return match.group(1)


def check(path):
    assembly = path.read_text()
    body = function_body(assembly, "enter_application")
    instructions = []
    for line in body.splitlines():
        line = line.split("@", 1)[0].strip()
        line = re.sub(r"^\w+:\s*", "", line)
        if not line or line.startswith("."):
            continue
        instructions.append(re.sub(r"\s+", " ", line.lower()))
    expected = ["msr msp, r0", "dsb 0xf", "isb 0xf", "blx r1", "b 1b"]
    if instructions[:len(expected)] != expected:
        raise ValueError(f"Unexpected handoff instructions: {instructions}")
    if any(instruction not in ("nop", "udf #255")
           for instruction in instructions[len(expected):]):
        raise ValueError(f"Unexpected code after nonreturning trap: {instructions}")
    caller = function_body(assembly, "esy_hb_boot_jump_to_app").lower()
    if re.search(r"\bmsr\s+msp\b", caller):
        raise ValueError("C wrapper still changes MSP")
    if not re.search(r"\bbl\s+enter_application\b", caller):
        raise ValueError("C wrapper does not enter the native trampoline")
    print(f"{path}: stack-free MSP switch, reset entry and return trap verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assembly", nargs="+", type=Path)
    args = parser.parse_args()
    for assembly_path in args.assembly:
        check(assembly_path)
