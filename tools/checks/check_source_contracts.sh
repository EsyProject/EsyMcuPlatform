#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

DEFAULT_SCAN_PATHS=(
  "esy_common"
  "esy_hal_backend"
  "esy_hal"
  "esy_middleware"
  "esy_test"
)

if (($# > 0)); then
  scan_paths=("$@")
else
  scan_paths=("${DEFAULT_SCAN_PATHS[@]}")
fi

echo "[source-contracts] checking dynamic allocation contract"
"${ROOT_DIR}/tools/checks/check_no_dynamic_alloc.sh" "${scan_paths[@]}"

echo "[source-contracts] checking copyright headers"
"${ROOT_DIR}/tools/checks/check_copyright_header.sh" "${scan_paths[@]}"

echo "[source-contracts] checking English-only comments and build metadata"
"${ROOT_DIR}/tools/checks/check_source_language.sh" "${scan_paths[@]}"

echo "[source-contracts] checking C empty parameter definitions"
"${ROOT_DIR}/tools/checks/check_c_empty_parameter_definitions.sh" "${scan_paths[@]}"

echo "[source-contracts] checking public header compatibility"
"${ROOT_DIR}/tools/checks/check_public_header_compatibility.sh" "${scan_paths[@]}"

echo "[source-contracts] checks passed"
