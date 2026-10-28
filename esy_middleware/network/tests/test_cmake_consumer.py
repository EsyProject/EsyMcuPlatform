# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Compile the generic network contract and independently gated providers."""

from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = r"""
cmake_minimum_required(VERSION 3.20)
project(network_consumer LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" defs)
if(ENABLE_ESY_NETWORK_LWIP)
  add_subdirectory("${PLATFORM_ROOT}/esy_common/utils" utils)
  add_subdirectory("${PLATFORM_ROOT}/esy_common/ds" ds)
  if(NOT MISSING_PROVIDER)
    add_library(esy_network_lwip_provider STATIC
      "${PLATFORM_ROOT}/esy_test/fake/third_party/lwip/fake_lwip.c"
      provider_config.c)
    target_include_directories(esy_network_lwip_provider PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/native_config"
      "${PLATFORM_ROOT}/esy_test/fake/third_party/lwip")
  endif()
endif()
add_subdirectory("${PLATFORM_ROOT}/esy_middleware/network" network)
foreach(lang c cpp)
  add_library(header_${lang} OBJECT consumer.${lang})
  target_link_libraries(header_${lang} PRIVATE esy_network)
  add_library(connection_header_${lang} OBJECT connection.${lang})
  target_link_libraries(connection_header_${lang} PRIVATE esy_network)
endforeach()
if(NOT ENABLE_ESY_NETWORK_LWIP AND TARGET esy_network_lwip)
  message(FATAL_ERROR "Disabled lwIP provider assembled")
endif()
if(ENABLE_ESY_NETWORK_LWIP)
  add_executable(raw_consumer raw_consumer.c)
  target_link_libraries(raw_consumer PRIVATE esy_network_lwip)
endif()
"""
CONSUMER = r"""
#include "esy_middleware/network/esy_network_stream.h"
#if defined(ESY_NETWORK_CONNECTION_H) || defined(ESY_NETWORK_LWIP_H)
#error base stream leaked another capability header
#endif
int main(void) {
  esy_network_stream_t stream = {0};
  return stream.send != 0;
}
"""

CONNECTION_CONSUMER = r"""
#include "esy_middleware/network/esy_network_connection.h"
#ifdef ESY_NETWORK_LWIP_H
#error connection contract leaked endpoint lifecycle
#endif
int main(void) {
  esy_network_connection_t connection = {0};
  esy_network_connection_status_t status = {ESY_NETWORK_CONNECTION_INACTIVE, 0u, 0u, 0u, 0u};
  status.state = ESY_NETWORK_CONNECTION_INACTIVE;
  return connection.stream.send != 0 || status.state != ESY_NETWORK_CONNECTION_INACTIVE;
}
"""

RAW_CONSUMER = r"""
#include "esy_middleware/network/esy_network_lwip.h"
#include <assert.h>
static unsigned progress_calls;
static void progress(void *context) {
  assert(context == &progress_calls);
  ++progress_calls;
}
int main(void) {
  esy_network_lwip_t endpoint = {0};
  esy_network_stream_t stream = {0};
  esy_network_connection_t connection = {0};
  esy_network_connection_status_t status = {ESY_NETWORK_CONNECTION_INACTIVE, 0u, 0u, 0u, 0u};
  uint8_t buffer[8];
  const esy_network_lwip_static_resources_t resources = {buffer, sizeof(buffer)};
  const esy_network_lwip_config_t config = {.static_resources = &resources,
      .progress = progress, .progress_context = &progress_calls};
  assert(esy_network_lwip_init(&endpoint, &config) == ESY_OK);
  assert(esy_network_lwip_get_stream(&endpoint, &stream) == ESY_OK);
  assert(esy_network_lwip_get_connection(&endpoint, &connection) == ESY_OK);
  assert(connection.stream.context == stream.context);
  assert(connection.get_status(connection.stream.context, &status) == ESY_OK);
  assert(status.state == ESY_NETWORK_CONNECTION_INACTIVE);
  assert(stream.progress(stream.context) == ESY_OK);
  assert(progress_calls == 1u);
  assert(esy_network_lwip_deinit(&endpoint) == ESY_OK);
  return 0;
}
"""


class NetworkConsumerTests(unittest.TestCase):
    def run_project(self, *, lwip=False, missing=False):
        with tempfile.TemporaryDirectory(prefix="network consumer ") as directory:
            project = Path(directory)
            (project / "CMakeLists.txt").write_text(PROJECT)
            (project / "esy_feature_import.cmake").write_text(
                f"set(ENABLE_ESY_NETWORK ON)\n"
                f"set(ENABLE_ESY_NETWORK_LWIP {'ON' if lwip else 'OFF'})\n")
            (project / "native_config").mkdir()
            (project / "native_config/network_native_config.h").write_text(
                "#define NETWORK_NATIVE_CONFIG_READY 1\n")
            (project / "provider_config.c").write_text(
                '#include "network_native_config.h"\n'
                '#ifndef NETWORK_NATIVE_CONFIG_READY\n#error missing provider config\n#endif\n')
            (project / "raw_consumer.c").write_text(RAW_CONSUMER)
            for suffix in ("c", "cpp"):
                (project / f"consumer.{suffix}").write_text(CONSUMER)
                (project / f"connection.{suffix}").write_text(CONNECTION_CONSUMER)
            result = subprocess.run(
                ["cmake", "-S", str(project), "-B", str(project / "out"),
                 f"-DPLATFORM_ROOT={ROOT}", f"-DMISSING_PROVIDER={int(missing)}"], capture_output=True, text=True)
            if missing:
                self.assertNotEqual(0, result.returncode)
                self.assertIn("requires", result.stdout + result.stderr)
                return
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            result = subprocess.run(["cmake", "--build", str(project / "out")],
                                    capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            if lwip:
                result = subprocess.run([str(project / "out/raw_consumer")],
                                        capture_output=True, text=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                symbols = subprocess.run(["nm", "-g", str(project / "out/raw_consumer")],
                                         capture_output=True, text=True, check=True).stdout
                for private in ("cyw43_", "esy_hal_", "esy_os_"):
                    self.assertNotIn(private, symbols)
            commands = json.loads((project / "out/compile_commands.json").read_text())
            for entry in commands:
                if Path(entry["file"]).name.startswith(("consumer.", "connection.")):
                    command = entry.get("command", " ".join(entry.get("arguments", [])))
                    for private in ("esy_test", "esy_hal", "network/src", "esy_middleware/os"):
                        self.assertNotIn(private, command)

    def test_shared_headers_need_no_native_stack_or_os(self):
        self.run_project()

    def test_lwip_provider_compiles_without_pico_or_os(self):
        self.run_project(lwip=True)

    def test_basic_stream_excludes_connection_capabilities_and_endpoint_states(self):
        probes = (
            "esy_network_stream_t stream = {0}; (void)stream.send_all;",
            "esy_network_stream_t stream = {0}; (void)stream.get_status;",
            "esy_network_state_t state; (void)state;",
            "esy_network_stream_status_t status; (void)status;",
            "esy_network_connection_t connection; (void)connection;",
            "int state = ESY_NETWORK_CONNECTED; (void)state;",
        )
        with tempfile.TemporaryDirectory(prefix="network base isolation ") as directory:
            for compiler, standard, suffix in (("cc", "c11", "c"), ("c++", "c++17", "cpp")):
                for index, body in enumerate(probes):
                    with self.subTest(language=suffix, probe=body):
                        source = Path(directory) / f"probe_{index}.{suffix}"
                        source.write_text('#include "esy_middleware/network/esy_network_stream.h"\n'
                                          + 'int main(void) { ' + body + ' return 0; }\n')
                        result = subprocess.run([compiler, f"-std={standard}", "-fsyntax-only",
                            f"-I{ROOT / 'esy_middleware/network/include'}",
                            f"-I{ROOT / 'esy_common/defs/include'}", str(source)],
                            capture_output=True, text=True)
                        self.assertNotEqual(0, result.returncode, body)

    def test_missing_raw_provider_is_reported(self):
        self.run_project(lwip=True, missing=True)

if __name__ == "__main__":
    unittest.main()
