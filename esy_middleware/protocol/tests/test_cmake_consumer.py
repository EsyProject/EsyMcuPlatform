#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Build independently selected protocol families, channels and OS workers."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TARGET_CALLS = {
    "esy_common_protocol_codec": ("esy_common/protocol/esy_proto_bin.h",
                                  "esy_proto_bin_parser_init(NULL)"),
    "esy_proto_bin_session": ("esy_middleware/protocol/esy_proto_bin_session.h",
                              "esy_proto_bin_session_init(NULL, NULL)"),
    "esy_proto_modbus_manager": ("esy_middleware/protocol/esy_proto_modbus_manager.h",
                                 "esy_proto_modbus_master_init(NULL, NULL)"),
    "esy_proto_bin_uart_channel": ("esy_middleware/protocol/esy_proto_bin_uart_channel.h",
                                   "esy_proto_bin_uart_channel_init(NULL, NULL)"),
    "esy_proto_bin_usb_channel": ("esy_middleware/protocol/esy_proto_bin_usb_channel.h",
                                  "esy_proto_bin_usb_channel_init(NULL, NULL)"),
    "esy_proto_bin_tcp_channel": ("esy_middleware/protocol/esy_proto_bin_tcp_channel.h",
                                  "esy_proto_bin_tcp_channel_init(NULL, NULL)"),
    "esy_proto_modbus_uart_channel": ("esy_middleware/protocol/esy_proto_modbus_uart_channel.h",
                                     "esy_proto_modbus_uart_master_channel_create(NULL, NULL, NULL)"),
    "esy_proto_modbus_uart_worker": ("esy_middleware/protocol/esy_proto_modbus_uart_worker.h",
                                    "esy_proto_modbus_uart_master_worker_create(NULL, NULL, NULL)"),
    "esy_proto_modbus_tcp_channel": ("esy_middleware/protocol/esy_proto_modbus_tcp_channel.h",
                                     "esy_proto_modbus_tcp_master_channel_create(NULL, NULL, NULL)"),
}
TARGET_SOURCES = {
    "esy_proto_bin_session": "bin/esy_proto_bin_session.c",
    "esy_proto_bin_uart_channel": "bin/esy_proto_bin_uart_channel.c",
    "esy_proto_bin_usb_channel": "bin/esy_proto_bin_usb_channel.c",
    "esy_proto_bin_tcp_channel": "bin/esy_proto_bin_tcp_channel.c",
    "esy_proto_modbus_manager": "modbus/esy_proto_modbus_manager.c",
    "esy_proto_modbus_uart_channel": "modbus/esy_proto_modbus_uart_channel.c",
    "esy_proto_modbus_uart_worker": "modbus/esy_proto_modbus_uart_worker.c",
    "esy_proto_modbus_tcp_channel": "modbus/esy_proto_modbus_tcp_channel.c",
}
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(protocol_consumer C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${ROOT}/esy_common" common)
if(PROVIDE_HAL)
    add_library(esy_hal STATIC "${ROOT}/esy_test/fake/hal/fake_esy_hal_system.c")
    target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include"
        PRIVATE "${ROOT}/esy_test/fake/hal")
    target_link_libraries(esy_hal PUBLIC esy_common_defs esy_common_device)
    if(ENABLE_ESY_UART)
        target_sources(esy_hal PRIVATE "${ROOT}/esy_test/fake/hal/fake_esy_hal_uart.c")
    endif()
    if(ENABLE_ESY_DMA)
        target_sources(esy_hal PRIVATE "${ROOT}/esy_test/fake/hal/fake_esy_hal_dma.c")
    endif()
endif()
if(PROVIDE_OS)
    add_library(esy_os STATIC "${ROOT}/esy_test/fake/os/fake_esy_os.c"
        "${ROOT}/esy_test/fake/os/fake_esy_os_task.c")
    target_include_directories(esy_os PUBLIC "${ROOT}/esy_middleware/os/include")
    target_link_libraries(esy_os PUBLIC esy_common_defs)
endif()
if(ENABLE_ESY_USB)
    add_library(tinyusb_device STATIC
        "${ROOT}/esy_test/fake/third_party/tinyusb/fake_tinyusb.c")
    target_include_directories(tinyusb_device PUBLIC
        "${ROOT}/esy_test/fake/third_party/tinyusb")
endif()
if(DIRECT_ASSEMBLY)
    if(ENABLE_ESY_USB)
        add_subdirectory("${ROOT}/esy_middleware/usb" usb)
    endif()
    if(ENABLE_ESY_NETWORK)
        add_subdirectory("${ROOT}/esy_middleware/network" network)
    endif()
    if(ENABLE_ESY_IO_SCHEDULER)
        add_subdirectory("${ROOT}/esy_middleware/io_scheduler" io_scheduler)
    endif()
    add_subdirectory("${ROOT}/esy_middleware/protocol" protocol)
else()
    add_subdirectory("${ROOT}/esy_middleware" middleware)
endif()
foreach(selected IN ITEMS esy_proto_bin_session esy_proto_modbus_manager
        esy_proto_bin_uart_channel esy_proto_bin_usb_channel esy_proto_bin_tcp_channel
        esy_proto_modbus_uart_channel esy_proto_modbus_uart_worker esy_proto_modbus_tcp_channel)
    if(selected IN_LIST EXPECTED_TARGETS)
        if(NOT TARGET ${selected})
            message(FATAL_ERROR "Selected protocol target is missing: ${selected}")
        endif()
    elseif(TARGET ${selected})
        message(FATAL_ERROR "Unselected protocol target was assembled: ${selected}")
    endif()
endforeach()
if(ENABLE_ESY_PROTO_BIN)
    if(NOT TARGET esy_proto_bin_contract)
        message(FATAL_ERROR "ProtoBin sender contract is missing")
    endif()
    get_target_property(contract_links esy_proto_bin_contract INTERFACE_LINK_LIBRARIES)
    if(NOT contract_links STREQUAL "esy_common_defs;esy_common_protocol_codec")
        message(FATAL_ERROR "ProtoBin contract acquired a runtime dependency")
    endif()
    foreach(language IN ITEMS c cpp)
        add_executable(contract_${language} contract.${language})
        target_link_libraries(contract_${language} PRIVATE esy_proto_bin_contract)
    endforeach()
elseif(TARGET esy_proto_bin_contract)
    message(FATAL_ERROR "Disabled ProtoBin family exposed a contract target")
endif()
if(NOT PROVIDE_OS AND TARGET esy_os)
    message(FATAL_ERROR "Caller-driven protocol consumer acquired an OS")
endif()
foreach(caller_driven IN ITEMS esy_proto_bin_session esy_proto_bin_uart_channel
        esy_proto_bin_usb_channel esy_proto_bin_tcp_channel
        esy_proto_modbus_manager esy_proto_modbus_uart_channel esy_proto_modbus_tcp_channel)
    if(TARGET ${caller_driven})
        get_target_property(dependencies ${caller_driven} INTERFACE_LINK_LIBRARIES)
        if("${dependencies}" MATCHES "esy_os")
            message(FATAL_ERROR "Caller-driven target acquired an OS dependency: ${caller_driven}")
        endif()
        if(caller_driven MATCHES "^esy_proto_modbus" AND "${dependencies}" MATCHES "esy_proto_bin")
            message(FATAL_ERROR "Modbus acquired a ProtoBin dependency")
        endif()
        if(caller_driven MATCHES "^esy_proto_bin" AND "${dependencies}" MATCHES "esy_proto_modbus")
            message(FATAL_ERROR "ProtoBin acquired a Modbus dependency")
        endif()
    endif()
endforeach()
if(TARGET esy_proto_modbus_manager)
    get_target_property(dependencies esy_proto_modbus_manager INTERFACE_LINK_LIBRARIES)
    if("${dependencies}" MATCHES "esy_proto_modbus_uart|esy_uart_tx_scheduler")
        message(FATAL_ERROR "Frame-oriented manager acquired a UART dependency")
    endif()
endif()
if(TARGET esy_uart_tx_scheduler)
    get_target_property(dependencies esy_uart_tx_scheduler INTERFACE_LINK_LIBRARIES)
    if("${dependencies}" MATCHES "esy_proto_")
        message(FATAL_ERROR "I/O scheduler acquired protocol policy")
    endif()
endif()
if(TARGET esy_proto_modbus_uart_worker)
    get_target_property(dependencies esy_proto_modbus_uart_worker INTERFACE_LINK_LIBRARIES)
    if(NOT "esy_proto_modbus_uart_channel" IN_LIST dependencies OR NOT "esy_os" IN_LIST dependencies)
        message(FATAL_ERROR "Worker must reuse the channel and OS public contracts")
    endif()
endif()
foreach(selected IN LISTS EXPECTED_TARGETS)
    foreach(language IN ITEMS c cpp)
        add_executable(${selected}_${language} ${selected}.${language})
        target_link_libraries(${selected}_${language} PRIVATE ${selected})
    endforeach()
endforeach()
'''
MODBUS_CHANNEL_PROBE = r'''
    if (esy_proto_modbus_uart_slave_channel_create(NULL, NULL, NULL) != ESY_ERR_INVALID_ARG) return 2;
    esy_uart_t uart = {0};
    esy_proto_modbus_uart_master_channel_storage_t storage = {0};
    ESY_PROTO_MODBUS_UART_MASTER_CHANNEL_STATIC_STORAGE(queues, 2, 2);
    esy_proto_modbus_uart_master_channel_config_t cfg = esy_proto_modbus_uart_master_channel_default_cfg();
    cfg.uart = &uart; cfg.queue_depth = 2;
    cfg.static_resources.slot_storage = queues.slot_storage;
    cfg.static_resources.slot_storage_count = 2;
    cfg.static_resources.payload_storage = queues.payload_storage;
    cfg.static_resources.payload_storage_count = 2;
    esy_proto_modbus_uart_master_channel_t *channel = NULL;
    if (esy_proto_modbus_uart_master_channel_create(&cfg, &storage, &channel) != ESY_OK) return 3;
    esy_proto_modbus_request_t request = {0};
    request.unit_id = 1; request.func_code = ESY_PROTO_MODBUS_FUNC_READ_COILS;
    request.pdu[0] = 1; request.pdu_len = 1;
    if (esy_proto_modbus_uart_master_channel_submit(channel, &request) != ESY_OK) return 4;
    if (esy_proto_modbus_uart_master_channel_abort_all(channel, true) != ESY_OK) return 5;
    if (esy_proto_modbus_uart_master_channel_poll(channel) != ESY_OK) return 6;
    if (esy_proto_modbus_uart_master_channel_destroy(channel) != ESY_OK) return 7;
'''


def consumer(target):
    header, call = TARGET_CALLS[target]
    declarations = ""
    checks = f"if ({call} != ESY_ERR_INVALID_ARG) return 1;\n"
    if target == "esy_proto_bin_tcp_channel":
        declarations = r'''
#ifdef ESY_NETWORK_LWIP_H
#error "Endpoint provider leaked into channel API"
#endif
static void check_connection_contract(void) {
    esy_proto_bin_tcp_channel_config_t config = {0};
    esy_network_connection_t *connection = &config.connection;
    esy_network_stream_t *stream = &connection->stream;
    esy_network_connection_status_t status = {ESY_NETWORK_CONNECTION_INACTIVE, 0u, 0u, 0u, 0u};
    status.state = ESY_NETWORK_CONNECTION_EOF;
    (void)stream; (void)status;
}
'''
        checks += "check_connection_contract();\n"
    if target == "esy_proto_modbus_uart_channel":
        checks += MODBUS_CHANNEL_PROBE
    if target == "esy_proto_modbus_uart_worker":
        declarations += '#include "esy_middleware/protocol/esy_proto_modbus_uart_channel.h"\n'
        checks += "if (esy_proto_modbus_uart_slave_worker_create(NULL, NULL, NULL) != ESY_ERR_INVALID_ARG) return 2;\n"
    if target == "esy_proto_modbus_tcp_channel":
        declarations += r'''
#ifdef ESY_NETWORK_LWIP_H
#error "Endpoint provider leaked into Modbus TCP API"
#endif
static void check_modbus_connection_contract(void) {
    esy_proto_modbus_tcp_master_channel_config_t config = {0};
    esy_network_connection_t *connection = &config.connection;
    esy_network_stream_t *stream = &connection->stream;
    (void)stream;
}
'''
        checks += "check_modbus_connection_contract();\n"
        checks += "if (esy_proto_modbus_tcp_slave_channel_create(NULL, NULL, NULL) != ESY_ERR_INVALID_ARG) return 2;\n"
    return (f'#include "{header}"\n'
            '#ifdef PRIVATE_OPTIONS_SENTINEL\n#error "Private options leaked"\n#endif\n'
            + declarations + "int main(void) {\n" + checks + "return 0;\n}\n")


class ProtocolConsumerTests(unittest.TestCase):
    def check(self, overrides=None, expected=(), error=None):
        features = dict(ENABLE_ESY_PROTO_BIN="OFF", ENABLE_ESY_PROTO_MODBUS="OFF",
            ENABLE_ESY_COMMON_PROTOCOL="ON", ENABLE_ESY_COMMON_DS="ON", ESY_RTOS="None",
            ENABLE_ESY_SYSTEM="ON", ENABLE_ESY_UART="OFF", ENABLE_ESY_DMA="OFF",
            ENABLE_ESY_PROTO_BIN_UART_CHANNEL="OFF", ENABLE_ESY_PROTO_BIN_USB_CHANNEL="OFF",
            ENABLE_ESY_PROTO_BIN_TCP_CHANNEL="OFF", ENABLE_ESY_USB="OFF", ENABLE_ESY_NETWORK="OFF",
            ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL="OFF", ENABLE_ESY_PROTO_MODBUS_UART_WORKER="OFF",
            ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL="OFF",
            ENABLE_ESY_IO_SCHEDULER="OFF", ENABLE_ESY_UART_TX_SCHEDULER="OFF",
            ENABLE_ESY_USB_CDC="ON", ENABLE_ESY_USB_BACKEND_TINYUSB="ON",
            PROVIDE_HAL="ON", PROVIDE_OS="OFF", DIRECT_ASSEMBLY="OFF")
        features.update(overrides or {})
        with tempfile.TemporaryDirectory(prefix="protocol consumer ") as directory:
            source = Path(directory)
            (source / "CMakeLists.txt").write_text(PROJECT)
            (source / "esy_feature_import.cmake").write_text(
                "".join(f"set({name} {value})\n" for name, value in features.items()) +
                'set(EXPECTED_TARGETS "' + ";".join(expected) + '")\n')
            for target in expected:
                for suffix in ("c", "cpp"):
                    (source / f"{target}.{suffix}").write_text(consumer(target))
            for suffix in ("c", "cpp"):
                (source / f"contract.{suffix}").write_text(
                    '#include "esy_middleware/protocol/esy_proto_bin_types.h"\n'
                    '#include "esy_middleware/protocol/esy_proto_bin_sender.h"\n'
                    'int main(void) { esy_proto_bin_sender_t sender = {0}; '
                    'return sizeof(sender) == 0; }\n')
            configured = subprocess.run(["cmake", "-S", str(source), "-B", str(source / "out"),
                f"-DROOT={ROOT}"], capture_output=True, text=True, timeout=60)
            output = configured.stdout + configured.stderr
            if error:
                self.assertNotEqual(0, configured.returncode, output)
                self.assertIn(error, " ".join(output.split()))
                return
            self.assertEqual(0, configured.returncode, output)
            built = subprocess.run(["cmake", "--build", str(source / "out"), "-j2"],
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            executables = list(expected)
            if features["ENABLE_ESY_PROTO_BIN"] == "ON":
                executables.append("contract")
            for target in executables:
                for suffix in ("c", "cpp"):
                    result = subprocess.run([str(source / "out" / f"{target}_{suffix}")],
                                            capture_output=True, text=True, timeout=5)
                    self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            commands = json.loads((source / "out/compile_commands.json").read_text())
            source_dir = ROOT / "esy_middleware/protocol/src"
            sources = {str(Path(entry["file"]).relative_to(source_dir)) for entry in commands
                       if Path(entry["file"]).is_relative_to(source_dir)}
            self.assertEqual({TARGET_SOURCES[target] for target in expected
                              if target in TARGET_SOURCES}, sources)
            self.assertEqual(features["ENABLE_ESY_IO_SCHEDULER"] == "ON" and
                             features["ENABLE_ESY_UART_TX_SCHEDULER"] == "ON",
                             any(Path(entry["file"]).name == "esy_uart_tx_scheduler.c"
                                 for entry in commands))
            for entry in commands:
                path = Path(entry["file"])
                if features["PROVIDE_OS"] == "OFF":
                    self.assertNotIn("esy_middleware/os", entry["command"])
                if path.stem == "contract":
                    self.assertNotIn("esy_hal/include", entry["command"])
                    self.assertNotIn("esy_middleware/os/include", entry["command"])
                if path.stem in {"esy_proto_modbus_uart_channel", "esy_proto_modbus_manager",
                                 "esy_proto_modbus_tcp_channel"}:
                    self.assertNotIn("esy_middleware/os/include", entry["command"])
                if features["PROVIDE_HAL"] == "OFF":
                    self.assertNotIn("esy_hal/include", entry["command"])

    def modbus_uart_flags(self, **overrides):
        flags = dict(ENABLE_ESY_PROTO_MODBUS="ON", ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL="ON",
                     ENABLE_ESY_UART="ON", ENABLE_ESY_DMA="ON", ENABLE_ESY_IO_SCHEDULER="ON",
                     ENABLE_ESY_UART_TX_SCHEDULER="ON")
        flags.update(overrides)
        return flags

    def test_bin_only_without_modbus_or_channels(self):
        self.check({"ENABLE_ESY_PROTO_BIN": "ON"}, ("esy_proto_bin_session",))

    def test_modbus_only_without_bin_or_channels(self):
        self.check({"ENABLE_ESY_PROTO_MODBUS": "ON"}, ("esy_proto_modbus_manager",))

    def test_families_can_be_selected_together(self):
        self.check(dict(ENABLE_ESY_PROTO_BIN="ON", ENABLE_ESY_PROTO_MODBUS="ON"),
                   ("esy_proto_bin_session", "esy_proto_modbus_manager"))

    def test_codec_only_has_no_hal_os_or_timed_protocol(self):
        self.check({"PROVIDE_HAL": "OFF", "ENABLE_ESY_SYSTEM": "OFF"},
                   ("esy_common_protocol_codec",))

    def test_channels_stay_off_when_hardware_and_providers_exist(self):
        self.check(dict(ENABLE_ESY_PROTO_BIN="ON", ENABLE_ESY_PROTO_MODBUS="ON",
                        ENABLE_ESY_UART="ON", ENABLE_ESY_DMA="ON", ENABLE_ESY_USB="ON",
                        ENABLE_ESY_NETWORK="ON", ENABLE_ESY_IO_SCHEDULER="ON",
                        ENABLE_ESY_UART_TX_SCHEDULER="ON"),
                   ("esy_proto_bin_session", "esy_proto_modbus_manager"))

    def test_explicit_bin_channels_link_in_c_and_cpp(self):
        for channel, dependencies in (
            ("UART", dict(ENABLE_ESY_UART="ON", ENABLE_ESY_DMA="ON", ENABLE_ESY_IO_SCHEDULER="ON",
                          ENABLE_ESY_UART_TX_SCHEDULER="ON")),
            ("USB", dict(ENABLE_ESY_USB="ON")),
            ("TCP", dict(ENABLE_ESY_NETWORK="ON")),
        ):
            with self.subTest(channel=channel):
                self.check(dict(dependencies, ENABLE_ESY_PROTO_BIN="ON",
                    **{f"ENABLE_ESY_PROTO_BIN_{channel}_CHANNEL": "ON"}),
                    ("esy_proto_bin_session", f"esy_proto_bin_{channel.lower()}_channel"))

    def test_bin_uart_requires_explicit_scheduler_and_stays_without_os(self):
        flags = dict(ENABLE_ESY_PROTO_BIN="ON", ENABLE_ESY_PROTO_BIN_UART_CHANNEL="ON",
                     ENABLE_ESY_UART="ON", ENABLE_ESY_DMA="ON")
        self.check(flags, error="requires target esy_uart_tx_scheduler")
        self.check(dict(flags, ENABLE_ESY_IO_SCHEDULER="ON"),
                   error="requires target esy_uart_tx_scheduler")
        self.check(dict(flags, ENABLE_ESY_IO_SCHEDULER="ON", ENABLE_ESY_UART_TX_SCHEDULER="ON",
                        DIRECT_ASSEMBLY="ON"),
                   ("esy_proto_bin_session", "esy_proto_bin_uart_channel"))

    def test_bin_channels_require_bin_family(self):
        for channel in ("UART", "USB", "TCP"):
            with self.subTest(channel=channel):
                self.check({f"ENABLE_ESY_PROTO_BIN_{channel}_CHANNEL": "ON"},
                           error="requires ENABLE_ESY_PROTO_BIN")

    def test_selected_families_require_system_and_common_providers(self):
        for family in ("ENABLE_ESY_PROTO_BIN", "ENABLE_ESY_PROTO_MODBUS"):
            for feature, required in (("ENABLE_ESY_SYSTEM", "ENABLE_ESY_SYSTEM"),
                                     ("PROVIDE_HAL", "target esy_hal"),
                                     ("ENABLE_ESY_COMMON_DS", "target esy_common_ds"),
                                     ("ENABLE_ESY_COMMON_PROTOCOL", "target esy_common_protocol_codec")):
                with self.subTest(family=family, feature=feature):
                    self.check({family: "ON", feature: "OFF"}, error=f"requires {required}")

    def test_selected_bin_channels_require_hardware_or_provider(self):
        for channel, dependency in (("UART", "ENABLE_ESY_UART"),
                                    ("USB", "target esy_usb"),
                                    ("TCP", "target esy_network")):
            with self.subTest(channel=channel):
                self.check({"ENABLE_ESY_PROTO_BIN": "ON", f"ENABLE_ESY_PROTO_BIN_{channel}_CHANNEL": "ON"},
                           error=f"requires {dependency}")
        self.check(dict(ENABLE_ESY_PROTO_BIN="ON", ENABLE_ESY_PROTO_BIN_UART_CHANNEL="ON",
                        ENABLE_ESY_UART="ON"), error="requires ENABLE_ESY_DMA")

    def test_modbus_uart_channel_is_caller_driven_without_os_or_worker(self):
        self.check(self.modbus_uart_flags(),
                   ("esy_proto_modbus_manager", "esy_proto_modbus_uart_channel"))

    def test_modbus_tcp_channel_is_caller_driven_without_uart_or_os(self):
        self.check(dict(ENABLE_ESY_PROTO_MODBUS="ON", ENABLE_ESY_NETWORK="ON",
                        ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL="ON"),
                   ("esy_proto_modbus_manager", "esy_proto_modbus_tcp_channel"))

    def test_modbus_tcp_requires_its_family_and_network_provider(self):
        self.check(dict(ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL="ON"),
                   error="requires ENABLE_ESY_PROTO_MODBUS")
        self.check(dict(ENABLE_ESY_PROTO_MODBUS="ON", ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL="ON"),
                   error="requires target esy_network")

    def test_modbus_tcp_is_independent_of_bin_tcp(self):
        self.check(dict(ENABLE_ESY_PROTO_BIN="ON", ENABLE_ESY_PROTO_MODBUS="ON",
                        ENABLE_ESY_NETWORK="ON", ENABLE_ESY_PROTO_BIN_TCP_CHANNEL="ON",
                        ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL="ON"),
                   ("esy_proto_bin_session", "esy_proto_bin_tcp_channel",
                    "esy_proto_modbus_manager", "esy_proto_modbus_tcp_channel"))

    def test_disabled_worker_adds_no_os_dependency_even_with_an_os_provider(self):
        self.check(self.modbus_uart_flags(DIRECT_ASSEMBLY="ON", PROVIDE_OS="ON"),
                   ("esy_proto_modbus_manager", "esy_proto_modbus_uart_channel"))

    def test_bin_and_modbus_channels_remain_independent_when_selected_together(self):
        self.check(self.modbus_uart_flags(ENABLE_ESY_PROTO_BIN="ON",
                                         ENABLE_ESY_PROTO_BIN_UART_CHANNEL="ON"),
                   ("esy_proto_bin_session", "esy_proto_bin_uart_channel",
                    "esy_proto_modbus_manager", "esy_proto_modbus_uart_channel"))

    def test_modbus_channel_requires_family_hardware_and_explicit_io_scheduler(self):
        for feature, required in (
            ("ENABLE_ESY_PROTO_MODBUS", "ENABLE_ESY_PROTO_MODBUS"),
            ("ENABLE_ESY_UART", "ENABLE_ESY_UART"),
            ("ENABLE_ESY_DMA", "ENABLE_ESY_DMA"),
            ("ENABLE_ESY_IO_SCHEDULER", "target esy_uart_tx_scheduler"),
            ("ENABLE_ESY_UART_TX_SCHEDULER", "target esy_uart_tx_scheduler"),
        ):
            with self.subTest(feature=feature):
                self.check(self.modbus_uart_flags(**{feature: "OFF"}), error=f"requires {required}")

    def test_modbus_worker_links_only_when_explicitly_enabled(self):
        self.check(self.modbus_uart_flags(DIRECT_ASSEMBLY="ON", PROVIDE_OS="ON",
                                         ESY_RTOS="FreeRTOS", ENABLE_ESY_PROTO_MODBUS_UART_WORKER="ON"),
                   ("esy_proto_modbus_manager", "esy_proto_modbus_uart_channel",
                    "esy_proto_modbus_uart_worker"))

    def test_modbus_worker_requires_channel_os_mode_and_os_target(self):
        flags = self.modbus_uart_flags(DIRECT_ASSEMBLY="ON", ESY_RTOS="FreeRTOS",
                                      ENABLE_ESY_PROTO_MODBUS_UART_WORKER="ON")
        for overrides, required in (
            (dict(ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL="OFF"), "requires ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL"),
            (dict(ESY_RTOS="None"), "requires ESY_RTOS=FreeRTOS"),
            (dict(ESY_RTOS="Unsupported"), "requires ESY_RTOS=FreeRTOS"),
            (dict(PROVIDE_OS="OFF"), "requires target esy_os"),
        ):
            with self.subTest(overrides=overrides):
                self.check(dict(flags, **overrides), error=required)


if __name__ == "__main__":
    unittest.main()
