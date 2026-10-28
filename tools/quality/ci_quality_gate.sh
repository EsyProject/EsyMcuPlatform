#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CHECKS_DIR="${ROOT_DIR}/tools/checks"
CACHE_READY=false
if [[ "${1:-}" = --_esy-provider-cache-ready ]]; then
  CACHE_READY=true
  shift
fi
TASK="${1:-test:all}"

case "${TASK}" in
  test:all|gcov:all)
    ;;
  *)
    echo "usage: $0 [test:all|gcov:all]" >&2
    exit 2
    ;;
esac

if [[ "${CACHE_READY}" = false ]]; then
  exec python3 -B "${ROOT_DIR}/tools/providers/provider_cache.py" \
    --require-default --exec bash "${BASH_SOURCE[0]}" --_esy-provider-cache-ready "$@"
fi

if ! command -v ceedling >/dev/null 2>&1; then
  echo "error: ceedling is required but was not found in PATH" >&2
  exit 127
fi

"${CHECKS_DIR}/check_esy_common_contracts.sh"
python3 -B "${ROOT_DIR}/tools/providers/test_prepare_providers.py"
python3 -B "${CHECKS_DIR}/test_copyright_header.py"
python3 -B "${CHECKS_DIR}/test_public_header_compatibility.py"
python3 -B "${CHECKS_DIR}/check_hardware_boundaries.py"
python3 -B "${CHECKS_DIR}/test_hardware_boundaries.py"
"${CHECKS_DIR}/check_source_contracts.sh"
"${CHECKS_DIR}/check_esy_hal_backend_contracts.sh"
"${CHECKS_DIR}/check_commit_message.sh"
python3 -B "${ROOT_DIR}/esy_hal/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_hal/tests/test_atomic_backend.py"
python3 -B "${ROOT_DIR}/esy_hal/tests/test_xio_storage_contract.py"
python3 -B "${ROOT_DIR}/esy_hal/tests/test_hal_storage_aliasing.py"
python3 -B "${ROOT_DIR}/esy_hal/tests/test_stm32_handoff_native.py"
python3 -B "${ROOT_DIR}/esy_hal/tests/test_esp32_fault_contract.py"
python3 -B "${ROOT_DIR}/esy_hal/tests/test_pico_wifi_runtime_contract.py"
python3 -B "${ROOT_DIR}/esy_hal/tests/test_coverage_gate.py"
python3 -B "${ROOT_DIR}/esy_middleware/bootloader/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/watchdog_monitor/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/watchdog_monitor/tests/test_coverage_gate.py"
python3 -B "${ROOT_DIR}/esy_middleware/core_lockout/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/dual_flash_store/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/file/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/fs/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/settings/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/diagnostics/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/remote_manager/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/time/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/credentials/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/crypto/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/tls/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/tls/tests/test_tls_smoke.py"
python3 -B "${ROOT_DIR}/esy_middleware/coredump/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/tools/coredump/test_esy_coredump_decode.py"
python3 -B "${ROOT_DIR}/esy_middleware/freq_synth/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/io_scheduler/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/lcd/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/input/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/event_bus/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/sensor/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/touch/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/ble/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/ble/tests/test_btstack_runtime.py"
python3 -B "${ROOT_DIR}/esy_middleware/lvgl/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/mqtt/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/mqtt/tests/test_coverage_gate.py"
python3 -B "${ROOT_DIR}/esy_middleware/http/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/http/tests/test_http_smoke.py"
python3 -B "${ROOT_DIR}/esy_middleware/http/tests/test_coverage_gate.py"
python3 -B "${ROOT_DIR}/esy_middleware/network/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/network/tests/test_coverage_gate.py"
python3 -B "${ROOT_DIR}/esy_middleware/io_scheduler/tests/test_coverage_gate.py"
python3 -B "${ROOT_DIR}/esy_middleware/logger/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/os/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/os/tests/test_coverage_sources.py"
python3 -B "${ROOT_DIR}/esy_middleware/power_manager/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/power_manager/tests/test_coverage_gate.py"
python3 -B "${ROOT_DIR}/esy_middleware/ota/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/ota/tests/test_coverage_gate.py"
python3 -B "${ROOT_DIR}/esy_middleware/protocol/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/protocol/tests/test_json_tcp_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/protocol/tests/test_json_usb_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/protocol/tests/test_coverage_gate.py"
python3 -B "${ROOT_DIR}/esy_middleware/usb/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/wifi/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/wifi/tests/test_coverage_gate.py"
python3 -B "${CHECKS_DIR}/test_gcov_thresholds.py"
python3 -B "${CHECKS_DIR}/test_maintenance_checks.py"
python3 -B "${ROOT_DIR}/esy_test/utils/tests/test_fixture_io.py"
python3 -B "${ROOT_DIR}/esy_test/utils/tests/test_fixture_task.py"
python3 -B "${ROOT_DIR}/esy_test/utils/tests/test_provider_paths.py"
python3 -B "${ROOT_DIR}/esy_common/protocol_codec/tests/test_modbus_limits_compile.py"
python3 -B "${ROOT_DIR}/esy_common/protocol_codec/tests/test_json_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_common/ds/tests/test_prio_fifo_turnover.py"
python3 -B "${ROOT_DIR}/esy_common/smf/tests/test_cmake_consumer.py"
python3 -B "${CHECKS_DIR}/test_cmake_integration.py"
python3 -B "${CHECKS_DIR}/test_pico_host_tools.py"
python3 -B "${CHECKS_DIR}/test_port_stages.py"
python3 -B "${CHECKS_DIR}/test_source_language.py"
python3 -B "${ROOT_DIR}/esy_middleware/numerics/tests/test_cmake_consumer.py"
python3 -B "${ROOT_DIR}/esy_middleware/numerics/tests/test_coverage_gate.py"

mapfile -t PROJECTS < <(
  find "${ROOT_DIR}/esy_common" "${ROOT_DIR}/esy_hal" \
    "${ROOT_DIR}/esy_middleware" \
    "${ROOT_DIR}/esy_test" \
    -type d -name build -prune -o \( -path "*/tests/project.yml" -o \
    -path "*/tests/json/project.yml" \) -type f -print | sort
)

if [ "${#PROJECTS[@]}" -eq 0 ]; then
  echo "error: no module test projects found" >&2
  exit 1
fi

for project in "${PROJECTS[@]}"; do
  test_dir="$(dirname "${project}")"
  rel_dir="${test_dir#"${ROOT_DIR}/"}"
  echo "[quality-gate] ${TASK} ${rel_dir}"
  if [ "${TASK}" = "gcov:all" ]; then
    rm -rf "${test_dir}/build"
  fi
  if [[ "${rel_dir}" = "esy_middleware/fs/tests" ]]; then
    python3 -B "${test_dir}/run_backend_tests.py" "${TASK}"
  elif [[ "${rel_dir}" = "esy_middleware/mqtt/tests" ||
        "${rel_dir}" = "esy_middleware/http/tests" ||
        "${rel_dir}" = "esy_middleware/crypto/tests" ||
        "${rel_dir}" = "esy_middleware/tls/tests" ||
        "${rel_dir}" = "esy_common/protocol_codec/tests/json" ||
        "${rel_dir}" = "esy_middleware/protocol/tests/json" ]]; then
    ruby "${ROOT_DIR}/tools/quality/run_module_tests.rb" "${test_dir}" \
      "${test_dir}/build" "${TASK}"
  else
    (cd "${test_dir}" && ceedling "${TASK}")
  fi
done

python3 -B "${ROOT_DIR}/esy_middleware/fs/tests/run_backend_tests.py" --sanitize

ruby "${ROOT_DIR}/tools/quality/run_module_tests.rb" \
  "${ROOT_DIR}/esy_common/protocol_codec/tests/json" \
  "${ROOT_DIR}/esy_common/protocol_codec/tests/json/build/sanitizer" test:all --sanitizer
ruby "${ROOT_DIR}/tools/quality/run_module_tests.rb" \
  "${ROOT_DIR}/esy_middleware/protocol/tests/json" \
  "${ROOT_DIR}/esy_middleware/protocol/tests/json/build/sanitizer" test:all --sanitizer

# Protocol arithmetic, byte alignment and lifecycle regressions also run under
# sanitizers, with artifacts isolated from the default coverage configuration.
for module in esy_hal esy_common/protocol_codec esy_common/smf esy_middleware/protocol esy_middleware/io_scheduler esy_middleware/input esy_middleware/event_bus esy_middleware/sensor esy_middleware/touch esy_middleware/ble esy_middleware/lvgl esy_middleware/mqtt esy_middleware/http esy_middleware/network/lwip esy_middleware/numerics esy_middleware/dual_flash_store esy_middleware/file esy_middleware/settings esy_middleware/diagnostics esy_middleware/ota esy_middleware/remote_manager esy_middleware/time esy_middleware/coredump esy_middleware/credentials esy_middleware/crypto esy_middleware/tls; do
  ruby "${ROOT_DIR}/tools/quality/run_module_tests.rb" "${ROOT_DIR}/${module}/tests" \
    "${ROOT_DIR}/${module}/tests/build/sanitizer" test:all --sanitizer
done

# OS port configurations have separate artifacts; never pollute default coverage.
for mixin in trace_disabled esp32_snapshot_unsupported; do
  ruby "${ROOT_DIR}/tools/quality/run_module_tests.rb" \
    "${ROOT_DIR}/esy_middleware/os/tests" \
    "${ROOT_DIR}/esy_middleware/os/tests/build/${mixin}" \
    test:test_esy_os_task_snapshot "--mixin=${mixin}.yml"
done
for mixin in tick16 tick64 esp32_critical; do
  ruby "${ROOT_DIR}/tools/quality/run_module_tests.rb" \
    "${ROOT_DIR}/esy_middleware/os/tests" \
    "${ROOT_DIR}/esy_middleware/os/tests/build/${mixin}" \
    test:test_os_contracts "--mixin=${mixin}.yml"
done
for mixin in tickless_smp tickless_tick16 tick64; do
  ruby "${ROOT_DIR}/tools/quality/run_module_tests.rb" \
    "${ROOT_DIR}/esy_middleware/os/tests" \
    "${ROOT_DIR}/esy_middleware/os/tests/build/${mixin}" \
    test:test_esy_os "--mixin=${mixin}.yml"
done
for mixin in fatal_esp32 tickless_smp; do
  ruby "${ROOT_DIR}/tools/quality/run_module_tests.rb" \
    "${ROOT_DIR}/esy_middleware/os/tests" \
    "${ROOT_DIR}/esy_middleware/os/tests/build/fatal_${mixin}" \
    test:test_esy_os_fatal "--mixin=${mixin}.yml"
done
if [ "${TASK}" = "gcov:all" ]; then
  python3 -B "${ROOT_DIR}/esy_middleware/mqtt/tests/check_coverage.py" \
    "${ROOT_DIR}/esy_middleware/mqtt/tests/build/artifacts/gcov/gcovr/coverage.json"
  python3 -B "${ROOT_DIR}/esy_middleware/http/tests/check_coverage.py" \
    "${ROOT_DIR}/esy_middleware/http/tests/build/artifacts/gcov/gcovr/coverage.json"
  python3 -B "${ROOT_DIR}/esy_middleware/network/tests/check_coverage.py" \
    "${ROOT_DIR}/esy_middleware/network/lwip/tests/build/artifacts/gcov/gcovr/coverage.json"
  python3 -B "${ROOT_DIR}/esy_hal/tests/check_coverage.py" \
    "${ROOT_DIR}/esy_hal/tests/build/artifacts/gcov/gcovr/coverage.json"
  python3 -B "${ROOT_DIR}/esy_middleware/numerics/tests/check_coverage.py" \
    "${ROOT_DIR}/esy_middleware/numerics/tests/build/artifacts/gcov/gcovr/coverage.json"
  python3 -B "${ROOT_DIR}/esy_middleware/io_scheduler/tests/check_coverage.py" \
    "${ROOT_DIR}/esy_middleware/io_scheduler/tests/build/artifacts/gcov/gcovr/coverage.txt"
  python3 -B "${ROOT_DIR}/esy_middleware/wifi/tests/check_coverage.py" \
    "${ROOT_DIR}/esy_middleware/wifi/tests/build/artifacts/gcov/gcovr/coverage.json"
  python3 -B "${ROOT_DIR}/esy_middleware/watchdog_monitor/tests/check_coverage.py" \
    "${ROOT_DIR}/esy_middleware/watchdog_monitor/tests/build/artifacts/gcov/gcovr/coverage.json"
  python3 -B "${ROOT_DIR}/esy_middleware/protocol/tests/check_coverage.py" \
    "${ROOT_DIR}/esy_common/protocol_codec/tests/build/artifacts/gcov/gcovr/coverage.txt" \
    "${ROOT_DIR}/esy_middleware/protocol/tests/build/artifacts/gcov/gcovr/coverage.txt" \
    "${ROOT_DIR}/esy_common/protocol_codec/tests/json/build/artifacts/gcov/gcovr/coverage.txt" \
    "${ROOT_DIR}/esy_middleware/protocol/tests/json/build/artifacts/gcov/gcovr/coverage.txt"
  python3 -B "${ROOT_DIR}/esy_middleware/os/tests/check_coverage_sources.py"
  python3 -B "${ROOT_DIR}/esy_middleware/power_manager/tests/check_coverage.py" \
    "${ROOT_DIR}/esy_middleware/power_manager/tests/build/artifacts/gcov/gcovr/coverage.json"
  python3 -B "${ROOT_DIR}/esy_middleware/ota/tests/check_coverage.py" \
    "${ROOT_DIR}/esy_middleware/ota/tests/build/artifacts/gcov/gcovr/coverage.json"
fi
