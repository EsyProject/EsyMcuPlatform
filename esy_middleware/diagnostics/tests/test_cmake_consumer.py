# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0
"""Exercise shared diagnostics through real C11/C++17 frontend targets."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(diagnostics_consumer C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Werror -pedantic)
include(features.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE TEST_PRIVATE_OPTIONS)
add_subdirectory("${ROOT}/esy_common" common)
add_subdirectory("${ROOT}/esy_middleware" middleware)
foreach(forbidden IN ITEMS esy_hal esy_hal_backend esy_board esy_os esy_logger
        esy_file esy_settings esy_coredump_core)
    if(TARGET ${forbidden})
        message(FATAL_ERROR "Unexpected dependency: ${forbidden}")
    endif()
endforeach()
if(NOT ENABLE_ESY_DIAGNOSTICS)
    if(TARGET esy_diagnostics OR TARGET esy_remote_manager_diagnostics)
        message(FATAL_ERROR "Disabled diagnostics target was assembled")
    endif()
    return()
endif()
if(NOT TARGET esy_diagnostics)
    message(FATAL_ERROR "Enabled diagnostics target missing")
endif()
if(TEST_FRONTENDS)
    set(consumer_targets esy_remote_manager_diagnostics esy_remote_manager_proto_bin)
else()
    set(consumer_targets esy_diagnostics)
    if(TARGET esy_remote_manager)
        message(FATAL_ERROR "Pure diagnostics assembled a frontend")
    endif()
endif()
foreach(language IN ITEMS c cpp)
    add_executable(consumer_${language} consumer.${language})
    target_link_libraries(consumer_${language} PRIVATE ${consumer_targets})
    if(TEST_FRONTENDS)
        target_compile_definitions(consumer_${language} PRIVATE TEST_FRONTENDS)
    endif()
endforeach()
'''

SOURCE = r'''
#include "esy_middleware/diagnostics/esy_diagnostics.h"
#include <string.h>
#ifdef TEST_FRONTENDS
#include "esy_middleware/remote_manager/esy_remote_manager_diagnostics_adapter.h"
#include "esy_middleware/remote_manager/esy_remote_manager_proto_bin.h"
#endif
#ifdef TEST_PRIVATE_OPTIONS
#error "Private implementation options leaked into a public consumer"
#endif
#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

typedef struct {
    unsigned reads;
    unsigned runs;
    unsigned authorizations;
    int deny_run;
    esy_diagnostics_health_t health;
    uint32_t code;
} source_state_t;

static esy_err_t query(void *context, esy_diagnostics_result_t *result) {
    source_state_t *state = (source_state_t *)context;
    ++state->reads;
    result->health = state->health;
    result->code = state->code;
    memcpy(result->detail, "measured", sizeof("measured"));
    result->detail_length = sizeof("measured") - 1u;
    return ESY_OK;
}

static esy_err_t self_test(void *context, esy_diagnostics_result_t *result) {
    source_state_t *state = (source_state_t *)context;
    ++state->runs;
    state->health = ESY_DIAGNOSTICS_HEALTH_WARNING;
    state->code = 19u;
    result->health = state->health;
    result->code = state->code;
    memcpy(result->detail, "calibration required", sizeof("calibration required"));
    result->detail_length = sizeof("calibration required") - 1u;
    return ESY_OK;
}

#ifdef TEST_FRONTENDS
static esy_err_t authorize(void *context, const esy_remote_manager_request_t *request) {
    source_state_t *state = (source_state_t *)context;
    ++state->authorizations;
    if (state->deny_run && request->group == ESY_REMOTE_MANAGER_GROUP_DIAGNOSTICS &&
            request->command == ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN) return ESY_ERR_STATE;
    return ESY_OK;
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
        ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}
#endif

int main(void) {
    source_state_t state = INITIALIZER;
    state.health = ESY_DIAGNOSTICS_HEALTH_OK;
    esy_diagnostics_entry_t entry = INITIALIZER;
    entry.name = "sensor.temperature";
    entry.read = query;
    entry.run = self_test;
    entry.context = &state;
    esy_diagnostics_static_resources_t resources = INITIALIZER;
    resources.entries = &entry;
    resources.count = 1u;
    esy_diagnostics_config_t configuration = INITIALIZER;
    configuration.static_resources = &resources;
    esy_diagnostics_t diagnostics = INITIALIZER;
    CHECK(esy_diagnostics_init(&diagnostics, &configuration) == ESY_OK);
    esy_diagnostics_snapshot_t snapshot = INITIALIZER;
    CHECK(esy_diagnostics_enumerate(&diagnostics, 0u, &snapshot) == ESY_OK);
    CHECK(snapshot.count == 1u && snapshot.index == 0u && state.reads == 0u && state.runs == 0u);
    CHECK(snapshot.result.health == ESY_DIAGNOSTICS_HEALTH_UNKNOWN);
    CHECK(esy_diagnostics_read(&diagnostics, entry.name, strlen(entry.name), &snapshot) == ESY_OK);
    CHECK(snapshot.result.health == ESY_DIAGNOSTICS_HEALTH_OK && state.reads == 1u);
    CHECK(esy_diagnostics_run(&diagnostics, entry.name, strlen(entry.name), &snapshot) == ESY_OK);
    CHECK(snapshot.result.health == ESY_DIAGNOSTICS_HEALTH_WARNING && state.runs == 1u);

#ifdef TEST_FRONTENDS
    esy_remote_manager_diagnostics_adapter_t remote = INITIALIZER;
    esy_remote_manager_diagnostics_adapter_config_t remote_config = INITIALIZER;
    remote_config.diagnostics = &diagnostics;
    CHECK(esy_remote_manager_diagnostics_adapter_init(&remote, &remote_config) == ESY_OK);
    esy_remote_manager_config_t manager_config = INITIALIZER;
    manager_config.max_key_length = ESY_DIAGNOSTICS_NAME_MAX_BYTES;
    manager_config.max_request_value_length = 128u;
    manager_config.max_response_value_length = ESY_REMOTE_MANAGER_PROTO_BIN_DIAGNOSTICS_BYTES;
    manager_config.authorize = authorize;
    manager_config.authorize_context = &state;
    CHECK(esy_remote_manager_diagnostics_adapter_get_ops(&remote,
        &manager_config.diagnostics_ops) == ESY_OK);
    esy_remote_manager_t manager = INITIALIZER;
    CHECK(esy_remote_manager_init(&manager, &manager_config) == ESY_OK);
    esy_remote_manager_request_t request = INITIALIZER;
    request.group = ESY_REMOTE_MANAGER_GROUP_DIAGNOSTICS;
    request.command = ESY_REMOTE_MANAGER_DIAGNOSTICS_READ;
    request.key = entry.name;
    request.key_length = strlen(entry.name);
    esy_remote_manager_response_t response = INITIALIZER;
    CHECK(esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) == ESY_OK);
    CHECK(response.diagnostic.result.health == snapshot.result.health);
    CHECK(response.diagnostic.result.code == snapshot.result.code && state.reads == 2u);
    request.command = ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN;
    state.deny_run = 1;
    CHECK(esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) == ESY_ERR_STATE);
    CHECK(state.runs == 1u);
    state.deny_run = 0;
    CHECK(esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) == ESY_OK);
    CHECK(state.runs == 2u && response.diagnostic.result.code == 19u);

    uint8_t payload[ESY_REMOTE_MANAGER_PROTO_BIN_REQUEST_HEADER_BYTES + ESY_DIAGNOSTICS_NAME_MAX_BYTES] = INITIALIZER;
    payload[0] = ESY_PROTO_DOMAIN_REMOTE_MANAGER;
    payload[1] = ESY_REMOTE_MANAGER_PROTO_BIN_REQUEST;
    payload[2] = ESY_REMOTE_MANAGER_PROTO_BIN_VERSION;
    payload[4] = ESY_REMOTE_MANAGER_GROUP_DIAGNOSTICS;
    payload[6] = ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN;
    write_u32(payload + 8u, 47u);
    payload[12] = (uint8_t)strlen(entry.name);
    memcpy(payload + ESY_REMOTE_MANAGER_PROTO_BIN_REQUEST_HEADER_BYTES, entry.name, strlen(entry.name));
    const size_t payload_length = ESY_REMOTE_MANAGER_PROTO_BIN_REQUEST_HEADER_BYTES + strlen(entry.name);
    uint8_t wire[ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE_HEADER_BYTES + ESY_REMOTE_MANAGER_PROTO_BIN_DIAGNOSTICS_BYTES];
    size_t length = 0u;
    const unsigned authorizations_before = state.authorizations;
    CHECK(esy_remote_manager_proto_bin_process(&manager, payload, payload_length,
        wire, sizeof(wire) - 1u, &length) == ESY_OK);
    CHECK(length == ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE_HEADER_BYTES && state.runs == 2u);
    CHECK(state.authorizations == authorizations_before);
    CHECK(read_u32(wire + 12u) == (uint32_t)ESY_ERR_NO_MEM);
    CHECK(read_u32(wire + 21u) == ESY_REMOTE_MANAGER_PROTO_BIN_DIAGNOSTICS_BYTES);
    CHECK(esy_remote_manager_proto_bin_process(&manager, payload, payload_length,
        wire, sizeof(wire), &length) == ESY_OK);
    CHECK(length == sizeof(wire) && state.runs == 3u && read_u32(wire + 12u) == 0u);
    const uint8_t *metadata = wire + ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE_HEADER_BYTES;
    CHECK(read_u32(metadata) == 1u && read_u32(metadata + 12u) == ESY_DIAGNOSTICS_HEALTH_WARNING);
    CHECK(read_u32(metadata + 16u) == 19u && read_u32(metadata + 20u) == strlen(entry.name));
    CHECK(memcmp(metadata + 28u, entry.name, strlen(entry.name)) == 0);

    CHECK(esy_remote_manager_deinit(&manager) == ESY_OK);
    CHECK(esy_remote_manager_diagnostics_adapter_deinit(&remote) == ESY_OK);
#endif
    CHECK(esy_diagnostics_deinit(&diagnostics) == ESY_OK);
    return 0;
}
'''


class DiagnosticsConsumerTests(unittest.TestCase):
    def command(self, *arguments, error=None):
        result = subprocess.run(arguments, capture_output=True, text=True)
        output = result.stdout + result.stderr
        if error is None:
            self.assertEqual(0, result.returncode,
                             f"Command {arguments} returned {result.returncode}\n{output}")
        else:
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(error, output)

    def prepare(self, path, *, frontends=False, overrides=None):
        (path / "CMakeLists.txt").write_text(PROJECT)
        features = {
            "ENABLE_ESY_DIAGNOSTICS": True,
            "ENABLE_ESY_REMOTE_MANAGER": frontends,
            "ENABLE_ESY_REMOTE_MANAGER_DIAGNOSTICS": frontends,
            "ENABLE_ESY_REMOTE_MANAGER_PROTO_BIN": frontends,
        }
        features.update(overrides or {})
        (path / "features.cmake").write_text(
            "set(ESY_RTOS None)\n"
            + f"set(TEST_FRONTENDS {'ON' if frontends else 'OFF'})\n"
            + "".join(f"set({name} {'ON' if enabled else 'OFF'})\n"
                      for name, enabled in features.items())
        )
        for suffix, initializer in (("c", "{0}"), ("cpp", "{}")):
            (path / f"consumer.{suffix}").write_text(SOURCE.replace("INITIALIZER", initializer))

    def configure(self, path, error=None):
        self.command("cmake", "-S", str(path), "-B", str(path / "build"),
                     f"-DROOT={ROOT}", error=error)

    def check_consumers(self, frontends):
        with tempfile.TemporaryDirectory(prefix="diagnostics consumers ") as directory:
            path = Path(directory)
            self.prepare(path, frontends=frontends)
            self.configure(path)
            self.command("cmake", "--build", str(path / "build"))
            commands = (path / "build/compile_commands.json").read_text()
            for forbidden in ("esy_hal/", "esy_middleware/os/", "esy_middleware/logger/"):
                self.assertNotIn(forbidden, commands)
            for language in ("c", "cpp"):
                self.command(str(path / "build" / f"consumer_{language}"))

    def test_core_c11_and_cpp17_without_hardware_or_frontends(self):
        self.check_consumers(False)

    def test_remote_frontend_shares_sources_without_uart_or_os(self):
        self.check_consumers(True)

    def test_disabled_core_and_frontends_create_no_diagnostics_targets(self):
        with tempfile.TemporaryDirectory(prefix="diagnostics disabled ") as directory:
            path = Path(directory)
            self.prepare(path, overrides={"ENABLE_ESY_DIAGNOSTICS": False})
            self.configure(path)
            self.command("cmake", "--build", str(path / "build"))

    def test_remote_manager_core_does_not_require_diagnostics_engine(self):
        with tempfile.TemporaryDirectory(prefix="diagnostics normalized only ") as directory:
            path = Path(directory)
            self.prepare(path, overrides={"ENABLE_ESY_DIAGNOSTICS": False,
                                          "ENABLE_ESY_REMOTE_MANAGER": True})
            self.configure(path)
            self.command("cmake", "--build", str(path / "build"))

    def test_remote_adapter_requires_diagnostics_provider(self):
        with tempfile.TemporaryDirectory(prefix="diagnostics provider gate ") as directory:
            path = Path(directory)
            self.prepare(path, frontends=True, overrides={"ENABLE_ESY_DIAGNOSTICS": False})
            self.configure(path, error="ENABLE_ESY_REMOTE_MANAGER_DIAGNOSTICS requires")

    def test_remote_adapter_requires_manager_parent(self):
        with tempfile.TemporaryDirectory(prefix="diagnostics manager gate ") as directory:
            path = Path(directory)
            self.prepare(path, overrides={"ENABLE_ESY_REMOTE_MANAGER_DIAGNOSTICS": True})
            self.configure(path, error="ENABLE_ESY_REMOTE_MANAGER_DIAGNOSTICS requires ENABLE_ESY_REMOTE_MANAGER")


if __name__ == "__main__":
    unittest.main()
