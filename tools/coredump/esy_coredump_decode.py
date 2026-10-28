#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Recover a validated Esy coredump from raw bytes or best-effort CD1 log lines."""

import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib


DEFAULT_MAX_BYTES = 16 * 1024 * 1024
CHUNK_BYTES = 16
HEADER_BYTES = 384
MAX_STACK_BYTES = 1024
MAGIC = 0x31444345
REGISTER_COUNT = 32
RECORD_ID_OFFSET = 268
RECORD_ID_BYTES = 16
ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
CHUNK_LINE = re.compile(
    r"(?:^|\s)CD1 ([0-9a-fA-F]{32}) ([0-9]+) ([0-9]+) ([0-9a-fA-F]+)\s*$"
)


class DecodeError(ValueError):
    """The input cannot identify and reconstruct one complete valid record."""


def decode_record(data, expected_id=None):
    """Validate the committed little-endian version-one wire format."""
    if len(data) < HEADER_BYTES:
        raise DecodeError("record is shorter than the fixed header")
    words = struct.unpack_from("<12I", data)
    magic, version, total, checksum, architecture, reason, core, flags = words[:8]
    image_base, firmware_version, stack_address, stack_length = words[8:]
    if magic != MAGIC:
        raise DecodeError("record has no valid ECD1 commit word")
    if version != 1:
        raise DecodeError(f"unsupported record version {version}")
    if total != len(data) or stack_length != total - HEADER_BYTES or stack_length > MAX_STACK_BYTES:
        raise DecodeError("record total/stack lengths do not match the input")
    calculated = zlib.crc32(data[4:12])
    calculated = zlib.crc32(data[16:], calculated) & 0xffffffff
    if checksum != calculated:
        raise DecodeError("record CRC32 does not match its contents")
    record_id = data[RECORD_ID_OFFSET:RECORD_ID_OFFSET + RECORD_ID_BYTES].hex()
    if not any(data[RECORD_ID_OFFSET:RECORD_ID_OFFSET + RECORD_ID_BYTES]):
        raise DecodeError("record identity must be nonzero")
    if expected_id is not None and record_id != expected_id.lower():
        raise DecodeError("CD1 identifier does not match the validated record identity")
    if architecture not in (1, 2):
        raise DecodeError(f"unsupported record architecture {architecture}")
    values = list(struct.unpack_from("<32I", data, 96))
    if architecture == 1:
        registers = {f"r{index}": values[index] for index in range(13)}
        registers.update(sp=values[13], lr=values[14], pc=values[15], xpsr=values[16])
    else:
        registers = {f"a{index}": values[index] for index in range(16)}
        registers.update(pc=values[16], ps=values[17], exccause=values[18], excvaddr=values[19])
    controls = dict(zip(("exc_return", "msp", "psp", "control", "primask", "basepri", "faultmask"),
                        struct.unpack_from("<7I", data, 68)))
    status = dict(zip(("cfsr", "hfsr", "dfsr", "afsr", "mmfar", "bfar", "shcsr",
                       "detail0", "detail1", "msplim", "psplim"),
                      struct.unpack_from("<11I", data, 224)))
    return {"format_version": version, "record_size": total, "record_crc": f"{checksum:08x}",
            "record_id": record_id,
            "architecture": architecture, "architecture_name": "arm_m" if architecture == 1 else "xtensa",
            "reason": reason, "core_id": core, "flags": flags,
            "core_registers_valid": bool(flags & 1), "image_base": image_base,
            "firmware_version": firmware_version, "build_id": data[48:68].hex(),
            "registers": registers, "register_values": values, "controls": controls,
            "fault_status": status, "stack_address": stack_address,
            "stack_length": stack_length, "stack_hex": data[HEADER_BYTES:].hex()}


def parse_record_id(value):
    """Parse the opaque 128-bit occurrence identity used by CD1 lines."""
    if not re.fullmatch(r"[0-9a-fA-F]{32}", value) or not any(bytes.fromhex(value)):
        raise argparse.ArgumentTypeError("record must contain 32 hexadecimal digits and be nonzero")
    return value.lower()


def recover_log(log, record=None, max_bytes=DEFAULT_MAX_BYTES):
    """Reassemble one record, accepting reordered and identical repeated bytes.

    The opaque occurrence identity groups chunks; decode_record verifies it,
    the committed binary format and its CRC after reassembly. Incomplete syntax is
    ignored so a later export can fill gaps left by truncated Logger lines.
    """
    records = {}
    for number, original in enumerate(log.splitlines(), 1):
        line = ANSI_ESCAPE.sub("", original)
        if not re.search(r"(?:^|\s)CD1(?:\s|$)", line):
            continue
        match = CHUNK_LINE.search(line)
        if match is None:
            continue
        identifier, total, offset, hexadecimal = match.groups()
        if len(total) > 10 or len(offset) > 10:
            raise DecodeError(f"line {number}: length and offset must fit in 32 bits")
        identifier, total, offset = identifier.lower(), int(total), int(offset)
        if not any(bytes.fromhex(identifier)):
            raise DecodeError(f"line {number}: record identity must be nonzero")
        if total <= 0 or total > max_bytes or offset > 0xffffffff:
            raise DecodeError(f"line {number}: record length is outside 1..{max_bytes}")
        if len(hexadecimal) > CHUNK_BYTES * 2:
            raise DecodeError(f"line {number}: chunk must contain 1..{CHUNK_BYTES} bytes")
        if offset >= total or (len(hexadecimal) + 1) // 2 > total - offset:
            raise DecodeError(f"line {number}: chunk exceeds its record")
        if len(hexadecimal) % 2:
            # A Logger line may end halfway through a byte. Discard the whole
            # incomplete chunk so a later export can supply its missing bytes.
            continue
        chunk = bytes.fromhex(hexadecimal)
        if record is not None and identifier != record:
            continue
        if identifier not in records:
            records[identifier] = (total, {})
        declared, chunks = records[identifier]
        if declared != total:
            raise DecodeError(f"line {number}: inconsistent lengths for {identifier}")
        for index, byte in enumerate(chunk, offset):
            if index in chunks and chunks[index] != byte:
                raise DecodeError(f"line {number}: conflicting bytes for {identifier}")
            chunks[index] = byte
    if not records:
        raise DecodeError("no matching CD1 record found")
    if len(records) != 1:
        choices = ", ".join(sorted(records))
        raise DecodeError(f"multiple records found ({choices}); select one with --record")
    identifier, (total, chunks) = next(iter(records.items()))
    if len(chunks) != total:
        raise DecodeError(f"record {identifier}: missing {total - len(chunks)} bytes")
    return bytes(chunks[index] for index in range(total)), identifier


def symbolize(info, elf, readelf="arm-none-eabi-readelf", addr2line="arm-none-eabi-addr2line"):
    """Verify the captured GNU build ID before symbolizing saved valid registers."""
    if not info.get("core_registers_valid", True):
        raise DecodeError("saved core registers are not valid for symbolization")
    try:
        result = subprocess.run([readelf, "-n", str(elf)], check=True,
                                capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise DecodeError(f"cannot read ELF build ID: {error}") from error
    identifiers = re.findall(r"Build ID:\s*([0-9a-fA-F]+)", result.stdout)
    expected = info["build_id"].lower()
    if len(identifiers) != 1 or identifiers[0].lower() != expected or not any(bytes.fromhex(expected)):
        raise DecodeError("ELF GNU build ID does not match the captured firmware")
    registers = info["registers"]
    arm = info.get("architecture", 1) == 1
    addresses = {"pc": registers["pc"]}
    if arm:
        addresses["lr"] = registers["lr"]
    locations = [address & ~1 if arm else address for address in addresses.values()]
    try:
        result = subprocess.run([addr2line, "-f", "-C", "-e", str(elf),
                                 *(f"0x{address:08x}" for address in locations)],
                                check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise DecodeError(f"cannot symbolize saved locations: {error}") from error
    return {"elf": str(elf), "addresses": addresses,
            "addr2line": result.stdout.rstrip(),
            "note": "Saved locations only; this is not an unwound backtrace."}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="captured logger text or raw coredump bytes")
    parser.add_argument("--format", choices=("auto", "log", "binary"), default="auto")
    parser.add_argument("--record", type=parse_record_id, help="32-hex-digit CD1 occurrence identifier")
    parser.add_argument("--output", type=Path, help="validated binary output path")
    parser.add_argument("--json", type=Path, dest="json_output", help="decoded JSON output path")
    parser.add_argument("--max-bytes", type=int, default=DEFAULT_MAX_BYTES)
    parser.add_argument("--elf", type=Path, help="exact ELF matching the captured GNU build ID")
    parser.add_argument("--readelf", default="arm-none-eabi-readelf")
    parser.add_argument("--addr2line", default="arm-none-eabi-addr2line")
    args = parser.parse_args(argv)
    if not 0 < args.max_bytes <= 0xffffffff:
        parser.error("--max-bytes must be in range 1..4294967295")
    try:
        source = args.input.read_bytes()
        record = None
        committed_binary = len(source) >= 4 and source[:4] == b"ECD1"
        if args.format == "log" or (args.format == "auto" and not committed_binary and b"CD1 " in source):
            data, record = recover_log(source.decode("utf-8", errors="replace"),
                                       args.record, args.max_bytes)
        else:
            if args.record is not None:
                raise DecodeError("--record selects logger records; use --format log")
            if len(source) > args.max_bytes:
                raise DecodeError("binary input exceeds --max-bytes")
            data = source
        info = decode_record(data, record)
        if args.elf:
            info["symbols"] = symbolize(info, args.elf, args.readelf, args.addr2line)
        output = args.output or args.input.with_suffix(".coredump.bin")
        json_output = args.json_output or output.with_suffix(".json")
        if output.resolve() == args.input.resolve() or json_output.resolve() == args.input.resolve():
            raise DecodeError("output paths must not overwrite the input")
        if output.resolve() == json_output.resolve():
            raise DecodeError("binary and JSON output paths must differ")
        output.write_bytes(data)
        json_output.write_text(json.dumps(info, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"Validated {len(data)} bytes: {output}\nDecoded metadata: {json_output}")
        return 0
    except (DecodeError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
