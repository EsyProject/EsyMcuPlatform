# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Check capture-log recovery and exact-ELF matching independently of firmware."""

import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from unittest import mock


SCRIPT = Path(__file__).with_name("esy_coredump_decode.py")
SPEC = importlib.util.spec_from_file_location("esy_coredump_decode", SCRIPT)
DECODER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DECODER)


DEFAULT_RECORD_ID = "12345678" * 4


def chunks(data, identifier=None):
    if identifier is None:
        identifier = data[268:284].hex() if len(data) >= 384 else DEFAULT_RECORD_ID
    return [f"[I:COREDUMP] CD1 {identifier} {len(data)} {offset} {data[offset:offset + 16].hex()}"
            for offset in range(0, len(data), 16)]


def record(architecture=1, stack=b"stack\x00\xff"):
    """Construct a documented wire fixture without importing firmware layout."""
    result = bytearray(384 + len(stack))
    struct.pack_into("<12I", result, 0, 0x31444345, 1, len(result), 0,
                     architecture, 1, 0, 3, 0x10030000, 10001, 0x20001000, len(stack))
    result[48:68] = bytes.fromhex("12" * 20)
    struct.pack_into("<7I", result, 68, 0xfffffff9, 0x20001000, 0, 0, 1, 0, 0)
    struct.pack_into("<32I", result, 96, *range(32))
    if architecture == 1:
        struct.pack_into("<3I", result, 148, 0x20001000, 0x10031001, 0x10032003)
    else:
        struct.pack_into("<I", result, 160, 0x40001003)
    struct.pack_into("<11I", result, 224, *range(101, 112))
    result[268:284] = bytes.fromhex(DEFAULT_RECORD_ID)
    result[384:] = stack
    refresh_crc(result)
    return result


def refresh_crc(data):
    checksum = zlib.crc32(data[4:12])
    struct.pack_into("<I", data, 12, zlib.crc32(data[16:], checksum) & 0xffffffff)


class RecordValidationTests(unittest.TestCase):
    def test_arm_record_keeps_faulting_image_identity_and_stack(self):
        data = record()
        info = DECODER.decode_record(data)
        self.assertEqual("arm_m", info["architecture_name"])
        self.assertEqual(0x10030000, info["image_base"])
        self.assertEqual(10001, info["firmware_version"])
        self.assertEqual("12" * 20, info["build_id"])
        self.assertEqual(DEFAULT_RECORD_ID, info["record_id"])
        self.assertEqual(0x10032003, info["registers"]["pc"])
        self.assertEqual(0x10031001, info["registers"]["lr"])
        self.assertEqual(111, info["fault_status"]["psplim"])
        self.assertEqual(b"stack\x00\xff", bytes.fromhex(info["stack_hex"]))
        self.assertTrue(info["core_registers_valid"])

    def test_xtensa_registers_use_documented_indices(self):
        info = DECODER.decode_record(record(architecture=2))
        self.assertEqual("xtensa", info["architecture_name"])
        self.assertEqual(0x40001003, info["registers"]["pc"])
        self.assertEqual(15, info["registers"]["a15"])
        self.assertEqual(19, info["registers"]["excvaddr"])

    def test_complete_binary_and_log_reconstruction_have_identical_metadata(self):
        data = record()
        identifier = data[268:284].hex()
        binary, found = DECODER.recover_log("\n".join(reversed(chunks(data, identifier))))
        self.assertEqual(DECODER.decode_record(data), DECODER.decode_record(binary, found))

    def test_truncated_uncommitted_unknown_version_and_bad_lengths_are_rejected(self):
        cases = []
        cases.append((record()[:383], "shorter"))
        data = record()
        struct.pack_into("<I", data, 0, 0)
        cases.append((data, "commit"))
        data = record()
        struct.pack_into("<I", data, 4, 2)
        refresh_crc(data)
        cases.append((data, "version 2"))
        data = record()
        struct.pack_into("<I", data, 8, 0xffffffff)
        refresh_crc(data)
        cases.append((data, "lengths"))
        data = record()
        struct.pack_into("<I", data, 44, 0)
        refresh_crc(data)
        cases.append((data, "lengths"))
        for data, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(DECODER.DecodeError, message):
                DECODER.decode_record(data)

    def test_checksum_covers_version_length_header_registers_and_stack(self):
        for offset in (12, 48, 95, 148, 267, 268, 283, 383, 384):
            data = record()
            data[offset] ^= 0x80
            with self.subTest(offset=offset), self.assertRaisesRegex(DECODER.DecodeError, "CRC32"):
                DECODER.decode_record(data)
        data = record()
        with self.assertRaisesRegex(DECODER.DecodeError, "identifier"):
            DECODER.decode_record(data, expected_id="00" * 16)

    def test_unknown_architecture_is_rejected_even_with_valid_checksum(self):
        with self.assertRaisesRegex(DECODER.DecodeError, "architecture 3"):
            DECODER.decode_record(record(architecture=3))

    def test_nonzero_occurrence_identity_is_required_even_with_a_valid_crc(self):
        data = record()
        data[268:284] = bytes(16)
        refresh_crc(data)
        with self.assertRaisesRegex(DECODER.DecodeError, "identity must be nonzero"):
            DECODER.decode_record(data)

    def test_log_identity_matches_the_persisted_occurrence_not_the_record_checksum(self):
        data = record()
        self.assertEqual(DEFAULT_RECORD_ID,
                         DECODER.decode_record(data, DEFAULT_RECORD_ID)["record_id"])
        with self.assertRaisesRegex(DECODER.DecodeError, "identifier"):
            DECODER.decode_record(data, "ab" * 16)

    def test_cli_generates_validated_binary_and_json_and_preserves_input(self):
        data = record(stack=b" CD1 not a log line")
        with tempfile.TemporaryDirectory(prefix="coredump decode ") as directory:
            source = Path(directory) / "raw.bin"
            output = Path(directory) / "fault.bin"
            source.write_bytes(data)
            result = subprocess.run([sys.executable, "-B", str(SCRIPT), str(source),
                                     "--output", str(output)], capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(data, source.read_bytes())
            self.assertEqual(data, output.read_bytes())
            info = json.loads(output.with_suffix(".json").read_text())
            self.assertEqual(0x10030000, info["image_base"])
            result = subprocess.run([sys.executable, "-B", str(SCRIPT), str(source),
                                     "--output", str(source)], capture_output=True, text=True)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("must not overwrite", result.stderr)
            self.assertEqual(data, source.read_bytes())

    def test_cli_rejects_bad_record_without_emitting_artifacts(self):
        with tempfile.TemporaryDirectory(prefix="coredump invalid ") as directory:
            source = Path(directory) / "bad.bin"
            source.write_bytes(b"incomplete")
            result = subprocess.run([sys.executable, "-B", str(SCRIPT), str(source)],
                                    capture_output=True, text=True)
            self.assertNotEqual(0, result.returncode)
            self.assertEqual([source], list(Path(directory).iterdir()))


class LogRecoveryTests(unittest.TestCase):
    def test_unordered_duplicates_and_other_logs_recover_exact_binary(self):
        data = bytes(range(67))
        lines = chunks(data)
        log = "\n".join(["normal startup", *reversed(lines), lines[1], "", "other output"])
        self.assertEqual((data, DEFAULT_RECORD_ID), DECODER.recover_log(log))

    def test_ansi_color_is_ignored(self):
        log = "\x1b[31m" + chunks(b"123")[0] + "\x1b[0m"
        self.assertEqual(b"123", DECODER.recover_log(log)[0])

    def test_later_export_fills_lost_chunks_despite_truncated_lines(self):
        data = record()
        identifier = data[268:284].hex()
        lines = chunks(data, identifier)
        first_attempt = [line for index, line in enumerate(lines) if index % 3 != 1]
        first_attempt.insert(1, lines[1][:-5] + "[I:other] next log")
        later_attempt = [line for index, line in enumerate(lines) if index % 3 == 1]
        recovered, found = DECODER.recover_log("\n".join(first_attempt + later_attempt))
        self.assertEqual(data, recovered)
        self.assertEqual(DECODER.decode_record(data), DECODER.decode_record(recovered, found))

    def test_complete_reexport_recovers_after_line_ends_halfway_through_a_byte(self):
        data = record()
        identifier = data[268:284].hex()
        lines = chunks(data, identifier)
        for removed_digits in (1, 5, 31):
            with self.subTest(removed_digits=removed_digits):
                first_attempt = lines.copy()
                first_attempt[1] = lines[1][:-removed_digits]
                recovered, found = DECODER.recover_log("\n".join(first_attempt + lines))
                self.assertEqual(data, recovered)
                self.assertEqual(DECODER.decode_record(data),
                                 DECODER.decode_record(recovered, found))

    def test_odd_hex_truncation_without_reexport_leaves_record_incomplete(self):
        data = record()
        identifier = data[268:284].hex()
        lines = chunks(data, identifier)
        lines[1] = lines[1][:-1]
        with self.assertRaisesRegex(DECODER.DecodeError, "missing 16 bytes"):
            DECODER.recover_log("\n".join(lines))

    def test_odd_hex_truncation_does_not_hide_conflicting_reexport_bytes(self):
        data = record()
        identifier = data[268:284].hex()
        lines = chunks(data, identifier)
        conflicting = bytearray(data)
        conflicting[16] ^= 1
        log = "\n".join([lines[1][:-1], *lines, chunks(conflicting, identifier)[1]])
        with self.assertRaisesRegex(DECODER.DecodeError, "conflicting bytes"):
            DECODER.recover_log(log)

    def test_odd_hex_payload_still_rejects_out_of_bounds_chunks(self):
        data = record()
        identifier = data[268:284].hex()
        lines = chunks(data, identifier)
        for offset, hexadecimal in ((len(data), "0"), (len(data) - 1, "000"),
                                     (0, "0" * 33)):
            malformed = f"CD1 {identifier} {len(data)} {offset} {hexadecimal}"
            with self.subTest(offset=offset, hexadecimal=hexadecimal):
                with self.assertRaisesRegex(DECODER.DecodeError, "exceeds|chunk must contain"):
                    DECODER.recover_log("\n".join([malformed, *lines]))

    def test_missing_chunk_is_rejected(self):
        with self.assertRaisesRegex(DECODER.DecodeError, "missing 16 bytes"):
            DECODER.recover_log("\n".join(chunks(bytes(range(33)))[1:]))

    def test_different_records_require_explicit_selection(self):
        lines = chunks(b"first") + chunks(b"second", "abcdef01" * 4)
        with self.assertRaisesRegex(DECODER.DecodeError, "multiple records"):
            DECODER.recover_log("\n".join(lines))
        self.assertEqual((b"second", "abcdef01" * 4),
                         DECODER.recover_log("\n".join(lines), "abcdef01" * 4))
        with self.assertRaisesRegex(DECODER.DecodeError, "no matching"):
            DECODER.recover_log("\n".join(lines), "87654321" * 4)

    def test_identical_fault_snapshots_with_distinct_occurrences_do_not_merge(self):
        first = record()
        second = bytearray(first)
        second[268:284] = bytes.fromhex("ab" * 16)
        refresh_crc(second)
        log = "\n".join(chunks(first) + chunks(second))
        with self.assertRaisesRegex(DECODER.DecodeError, "multiple records"):
            DECODER.recover_log(log)
        restored, identifier = DECODER.recover_log(log, "ab" * 16)
        self.assertEqual(second, restored)
        self.assertEqual("ab" * 16, DECODER.decode_record(restored, identifier)["record_id"])

    def test_short_or_zero_chunk_identifiers_are_not_valid_occurrences(self):
        with self.assertRaisesRegex(DECODER.DecodeError, "no matching"):
            DECODER.recover_log("CD1 12345678 3 0 010203")
        with self.assertRaisesRegex(DECODER.DecodeError, "identity must be nonzero"):
            DECODER.recover_log("CD1 " + "00" * 16 + " 3 0 010203")

    def test_conflicting_duplicate_and_overlapping_chunks_are_rejected(self):
        for suffix in ("CD1 12345678123456781234567812345678 3 0 303233", "CD1 12345678123456781234567812345678 3 1 30"):
            with self.subTest(suffix=suffix), self.assertRaisesRegex(DECODER.DecodeError, "conflicting"):
                DECODER.recover_log(chunks(b"123")[0] + "\n" + suffix)

    def test_length_changes_are_rejected(self):
        with self.assertRaisesRegex(DECODER.DecodeError, "inconsistent lengths"):
            DECODER.recover_log(chunks(b"123")[0] + "\nCD1 12345678123456781234567812345678 4 3 34")

    def test_malformed_chunks_are_rejected(self):
        malformed = ["CD1 12345678123456781234567812345678 3 0 1", "CD1 12345678123456781234567812345678 3 2 0001",
                     "CD1 12345678123456781234567812345678 3 3 01", "CD1 12345678123456781234567812345678 0 0 01",
                     "CD1 12345678123456781234567812345678 3 0 xx", "CD1 12345678123456781234567812345678 3 0 01 tail",
                     "CD1 12345678123456781234567812345678 30 0 " + "ff" * 17, "CD1"]
        for line in malformed:
            with self.subTest(line=line), self.assertRaises(DECODER.DecodeError):
                DECODER.recover_log(line)
        with self.assertRaisesRegex(DECODER.DecodeError, "outside"):
            DECODER.recover_log("CD1 12345678123456781234567812345678 100 0 00", max_bytes=99)

    def test_complete_nonzero_128_bit_record_selector_is_required(self):
        self.assertEqual("1234abcd" * 4, DECODER.parse_record_id("1234AbCd" * 4))
        for invalid in ("123", "12345678", "0x" + "12" * 16, "00" * 16, "xx" * 16):
            with self.subTest(value=invalid), self.assertRaises(argparse.ArgumentTypeError):
                DECODER.parse_record_id(invalid)


class SymbolizationTests(unittest.TestCase):
    def setUp(self):
        self.info = {"build_id": "12" * 20, "registers": {"pc": 0x10010003, "lr": 0x10020005}}

    def test_exact_build_id_and_saved_pc_lr_only(self):
        notes = subprocess.CompletedProcess([], 0, "Build ID: " + "12" * 20, "")
        symbols = subprocess.CompletedProcess([], 0, "fault_fn\nfile.c:3\ncaller\nfile.c:8\n", "")
        with mock.patch.object(DECODER.subprocess, "run", side_effect=[notes, symbols]) as run:
            result = DECODER.symbolize(self.info, Path("exact image.elf"))
        self.assertIn("not an unwound backtrace", result["note"])
        command = run.call_args_list[1].args[0]
        self.assertEqual(["arm-none-eabi-addr2line", "-f", "-C", "-e", "exact image.elf",
                          "0x10010002", "0x10020004"], command)

    def test_missing_mismatched_or_zero_id_never_calls_addr2line(self):
        for note in ("", "Build ID: " + "34" * 20, "Build ID: " + "12" * 19):
            with self.subTest(note=note), mock.patch.object(DECODER.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, note, "")
                with self.assertRaisesRegex(DECODER.DecodeError, "does not match"):
                    DECODER.symbolize(self.info, Path("wrong.elf"))
                self.assertEqual(1, run.call_count)
        self.info["build_id"] = "00" * 20
        with mock.patch.object(DECODER.subprocess, "run") as run:
            run.return_value = subprocess.CompletedProcess([], 0, "Build ID: " + "00" * 20, "")
            with self.assertRaisesRegex(DECODER.DecodeError, "does not match"):
                DECODER.symbolize(self.info, Path("zero.elf"))

    def test_missing_tool_and_tool_failure_are_reported(self):
        for error in (FileNotFoundError("missing readelf"), subprocess.CalledProcessError(1, "readelf")):
            with self.subTest(error=error), mock.patch.object(DECODER.subprocess, "run", side_effect=error):
                with self.assertRaisesRegex(DECODER.DecodeError, "cannot read ELF"):
                    DECODER.symbolize(self.info, Path("image.elf"))

    def test_xtensa_symbols_only_pc_and_preserves_its_address(self):
        self.info.update(architecture=2, registers={"pc": 0x40001003, "a0": 0x80002003})
        notes = subprocess.CompletedProcess([], 0, "Build ID: " + "12" * 20, "")
        symbols = subprocess.CompletedProcess([], 0, "fault_fn\nfile.c:3\n", "")
        with mock.patch.object(DECODER.subprocess, "run", side_effect=[notes, symbols]) as run:
            DECODER.symbolize(self.info, Path("exact.elf"), "xtensa-readelf", "xtensa-addr2line")
        self.assertEqual(["xtensa-addr2line", "-f", "-C", "-e", "exact.elf", "0x40001003"],
                         run.call_args_list[1].args[0])

    def test_invalid_registers_are_not_symbolized(self):
        self.info["core_registers_valid"] = False
        with mock.patch.object(DECODER.subprocess, "run") as run:
            with self.assertRaisesRegex(DECODER.DecodeError, "not valid"):
                DECODER.symbolize(self.info, Path("exact.elf"))
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
