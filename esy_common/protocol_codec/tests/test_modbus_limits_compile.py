#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Compile real Modbus codec boundaries with supported configuration overrides."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
INCLUDES = (
    ROOT / "esy_common/defs/include",
    ROOT / "esy_common/protocol_codec/include",
    ROOT / "esy_common/ds/include",
    ROOT / "esy_middleware/protocol/include",
)
PROBE = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "esy_common/protocol/esy_proto_modbus.h"

int main(void) {
  uint8_t pdu[ESY_PROTO_MODBUS_MAX_PDU_LEN + 1u] = {3u};
  uint8_t buf[sizeof(pdu) + ESY_PROTO_MODBUS_MBAP_LEN];
  esy_proto_modbus_frame_t frame;
  esy_proto_modbus_frame_view_t view;
  assert(ESY_PROTO_MODBUS_MAX_PDU_LEN == ESY_TEST_EXPECTED_LIMIT);
  assert(esy_proto_modbus_frame_init(&frame, buf, sizeof(buf)) == ESY_OK);
  assert(esy_proto_modbus_build_rtu_frame(&frame, 1u, pdu,
      ESY_PROTO_MODBUS_MAX_PDU_LEN) == ESY_OK);
  assert(esy_proto_modbus_verify_rtu_frame(buf, frame.len));
  assert(esy_proto_modbus_parse_frame(ESY_PROTO_MODBUS_FRAME_RTU, buf,
      frame.len, &view) == ESY_OK);
  assert(view.pdu_len == ESY_PROTO_MODBUS_MAX_PDU_LEN);
  assert(esy_proto_modbus_build_tcp_frame(&frame, 7u, 1u, pdu,
      ESY_PROTO_MODBUS_MAX_PDU_LEN) == ESY_OK);
  assert(esy_proto_modbus_verify_tcp_frame(buf, frame.len));
  assert(esy_proto_modbus_parse_frame(ESY_PROTO_MODBUS_FRAME_TCP, buf,
      frame.len, &view) == ESY_OK);
  assert(view.pdu_len == ESY_PROTO_MODBUS_MAX_PDU_LEN);
  assert(esy_proto_modbus_build_rtu_frame(&frame, 1u, pdu,
      sizeof(pdu)) == ESY_ERR_INVALID_ARG);
  assert(esy_proto_modbus_build_tcp_frame(&frame, 7u, 1u, pdu,
      sizeof(pdu)) == ESY_ERR_INVALID_ARG);

  /* Supply valid envelopes directly; builders must not create oversized PDUs. */
  memset(buf, 3, sizeof(buf));
  uint16_t crc = esy_proto_modbus_crc16(buf, sizeof(pdu) + 1u);
  buf[sizeof(pdu) + 1u] = (uint8_t)crc;
  buf[sizeof(pdu) + 2u] = (uint8_t)(crc >> 8);
  assert(!esy_proto_modbus_verify_rtu_frame(buf, sizeof(pdu) + 3u));
  assert(esy_proto_modbus_parse_frame(ESY_PROTO_MODBUS_FRAME_RTU, buf,
      sizeof(pdu) + 3u, &view) == ESY_ERR_FAIL);
  buf[2] = 0u;
  buf[3] = 0u;
  buf[4] = (uint8_t)((sizeof(pdu) + 1u) >> 8);
  buf[5] = (uint8_t)(sizeof(pdu) + 1u);
  assert(!esy_proto_modbus_verify_tcp_frame(buf, sizeof(buf)));
  assert(esy_proto_modbus_parse_frame(ESY_PROTO_MODBUS_FRAME_TCP, buf,
      sizeof(buf), &view) == ESY_ERR_FAIL);
  return 0;
}
"""


class ModbusLimitsCompileTests(unittest.TestCase):
    def compile(self, source, definitions, *, link=False):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory(prefix="esy-modbus-limits-") as tmp:
            probe = Path(tmp) / "probe.c"
            probe.write_text(source, encoding="utf-8")
            command = compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror"]
            command += [f"-I{path}" for path in INCLUDES]
            command += [f"-D{value}" for value in definitions]
            command += [str(probe)]
            if link:
                executable = Path(tmp) / "probe"
                command += [str(ROOT / "esy_common/protocol_codec/src/esy_proto_modbus.c"),
                            "-o", str(executable)]
            else:
                command += ["-fsyntax-only"]
            result = subprocess.run(command, capture_output=True, text=True,
                                    check=False)
            if link:
                self.assertEqual(0, result.returncode, result.stderr)
                result = subprocess.run([str(executable)], capture_output=True,
                                        text=True, check=False)
                self.assertEqual(0, result.returncode, result.stderr)
            return result

    def test_default_limit_boundaries(self):
        self.compile(PROBE, ["ESY_TEST_EXPECTED_LIMIT=252"], link=True)

    def test_supported_override_boundaries(self):
        for maximum in (1, 16, 253):
            with self.subTest(maximum=maximum):
                self.compile(PROBE, [f"ESY_TEST_EXPECTED_LIMIT={maximum}",
                                    f"ESY_PROTO_MODBUS_MAX_PDU_LEN={maximum}"],
                             link=True)

    def test_invalid_codec_overrides_fail_compilation(self):
        source = '#include "esy_common/protocol/esy_proto_modbus.h"\n'
        for maximum in (0, -1, 254, 65535):
            with self.subTest(maximum=maximum):
                result = self.compile(source,
                                      [f"ESY_PROTO_MODBUS_MAX_PDU_LEN={maximum}"])
                self.assertNotEqual(0, result.returncode)
                self.assertIn("must be in 1..253", result.stderr)

    def test_manager_limit_may_restrict_codec_limit(self):
        source = '#include "esy_middleware/protocol/esy_proto_modbus_manager.h"\n'
        for codec, manager in ((1, 1), (16, 8), (253, 253)):
            with self.subTest(codec=codec, manager=manager):
                result = self.compile(source,
                                      [f"ESY_PROTO_MODBUS_MAX_PDU_LEN={codec}",
                                       f"ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN={manager}"])
                self.assertEqual(0, result.returncode, result.stderr)

    def test_manager_limit_must_fit_codec_limit(self):
        source = '#include "esy_middleware/protocol/esy_proto_modbus_manager.h"\n'
        for codec, manager in ((16, 0), (16, 17), (252, 253)):
            with self.subTest(codec=codec, manager=manager):
                result = self.compile(source,
                                      [f"ESY_PROTO_MODBUS_MAX_PDU_LEN={codec}",
                                       f"ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN={manager}"])
                self.assertNotEqual(0, result.returncode)
                self.assertIn("must be in 1..ESY_PROTO_MODBUS_MAX_PDU_LEN",
                              result.stderr)


if __name__ == "__main__":
    unittest.main()
