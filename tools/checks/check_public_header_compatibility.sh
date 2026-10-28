#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "${1:-}" = --_esy-provider-cache-ready ]]; then
  shift
else
  exec python3 -B "${ROOT_DIR}/tools/providers/provider_cache.py" --exec \
    bash "${BASH_SOURCE[0]}" --_esy-provider-cache-ready "$@"
fi

DEFAULT_SCAN_PATHS=(
  "esy_common"
  "esy_hal_backend"
  "esy_hal"
  "esy_middleware"
)

if (($# > 0)); then
  scan_paths=("$@")
else
  scan_paths=("${DEFAULT_SCAN_PATHS[@]}")
fi

require_tool() {
  local tool="$1"

  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "[public-header-compat] error: ${tool} is required" >&2
    exit 127
  fi
}

resolve_path() {
  local path="$1"

  if [[ "${path}" = /* ]]; then
    printf '%s\n' "${path}"
  elif [[ -e "${PWD}/${path}" ]]; then
    printf '%s\n' "${PWD}/${path}"
  else
    printf '%s\n' "${ROOT_DIR}/${path}"
  fi
}

require_tool cc
require_tool c++

resolved_paths=()
for scan_path in "${scan_paths[@]}"; do
  resolved_paths+=("$(resolve_path "${scan_path}")")
done

include_dirs=()
for common_dir in "${ROOT_DIR}"/esy_common/{core,defs,utils,ds,protocol_codec,device}; do
  if [[ -d "${common_dir}/include" ]]; then
    include_dirs+=("-I${common_dir}/include")
  fi
done

for module_dir in "${ROOT_DIR}"/esy_common "${ROOT_DIR}"/esy_hal_backend \
                  "${ROOT_DIR}"/esy_hal "${ROOT_DIR}"/esy_middleware/*; do
  if [[ -d "${module_dir}/include" ]]; then
    include_dirs+=("-I${module_dir}/include")
  fi
done

status=0
headers=()
requires_mbedtls=false
requires_littlefs=false
requires_fatfs=false
while IFS= read -r -d '' header; do
  headers+=("${header}")
  if [[ "${header##*/}" == *_mbedtls.h ]]; then
    requires_mbedtls=true
  fi
  case "${header##*/}" in
    esy_fs_littlefs.h|esy_fs_flash.h) requires_littlefs=true ;;
    esy_fs_fatfs.h) requires_fatfs=true ;;
  esac
done < <(
  find "${resolved_paths[@]}" \
    -path '*/build/*' -prune -o \
    \( -path '*/include/*.h' -o -path '*/esy_test/*.h' \) \
    -type f -print0 | sort -z
)

if [[ "${requires_mbedtls}" == true ]]; then
  if [[ -z "${ESY_MBEDTLS_PATH:-}" ]]; then
    echo "[public-header-compat] error: ESY_MBEDTLS_PATH is required to check *_mbedtls.h adapter headers" >&2
    exit 2
  fi
  if [[ ! -f "${ESY_MBEDTLS_PATH}/include/mbedtls/version.h" ]]; then
    echo "[public-header-compat] error: ESY_MBEDTLS_PATH must contain include/mbedtls/version.h: ${ESY_MBEDTLS_PATH}" >&2
    exit 2
  fi
fi

if [[ "${requires_littlefs}" == true ]]; then
  if [[ -z "${ESY_LITTLEFS_PATH:-}" ]]; then
    echo "[public-header-compat] error: ESY_LITTLEFS_PATH is required to check esy_fs_littlefs.h and esy_fs_flash.h adapter headers" >&2
    exit 2
  fi
  for provider_header in lfs.h lfs_util.h; do
    if [[ ! -f "${ESY_LITTLEFS_PATH}/${provider_header}" ]]; then
      echo "[public-header-compat] error: ESY_LITTLEFS_PATH must contain ${provider_header}: ${ESY_LITTLEFS_PATH}" >&2
      exit 2
    fi
  done
fi

fatfs_include_dir=""
if [[ "${requires_fatfs}" == true ]]; then
  if [[ -z "${ESY_FATFS_PATH:-}" ]]; then
    echo "[public-header-compat] error: ESY_FATFS_PATH is required to check esy_fs_fatfs.h adapter headers" >&2
    exit 2
  fi
  if [[ ! -f "${ESY_FATFS_PATH}/ff.h" ]]; then
    echo "[public-header-compat] error: ESY_FATFS_PATH must contain ff.h: ${ESY_FATFS_PATH}" >&2
    exit 2
  fi
  fatfs_config="${ROOT_DIR}/esy_middleware/fs/config/fatfs/ffconf.h"
  if [[ ! -f "${fatfs_config}" ]]; then
    echo "[public-header-compat] error: filesystem adapter configuration is missing: ${fatfs_config}" >&2
    exit 2
  fi
  fatfs_include_dir="$(mktemp -d)"
  trap 'rm -rf -- "${fatfs_include_dir}"' EXIT
  # ff.h searches beside itself first; leave the supplied provider untouched.
  cp -- "${ESY_FATFS_PATH}/ff.h" "${fatfs_include_dir}/ff.h"
  cp -- "${fatfs_config}" "${fatfs_include_dir}/ffconf.h"
fi

select_provider_includes() {
  local header="$1"
  provider_includes=()
  if [[ "${header##*/}" == *_mbedtls.h ]]; then
    provider_includes+=("-I${ESY_MBEDTLS_PATH}/include")
  fi
  case "${header##*/}" in
    esy_fs_littlefs.h|esy_fs_flash.h)
      provider_includes+=("-I${ESY_LITTLEFS_PATH}" -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR)
      ;;
    esy_fs_fatfs.h) provider_includes+=("-I${fatfs_include_dir}") ;;
  esac
}

echo "[public-header-compat] checking C public header self-containment"
for header in "${headers[@]}"; do
  select_provider_includes "${header}"
  cc -std=c11 -Wall -Wextra -Werror \
    "${include_dirs[@]}" "${provider_includes[@]}" \
    -fsyntax-only -x c "${header}" || status=1
done

echo "[public-header-compat] checking C++ public header compatibility"
for header in "${headers[@]}"; do
  select_provider_includes "${header}"
  c++ -std=c++17 -Wall -Wextra -Werror \
    "${include_dirs[@]}" "${provider_includes[@]}" \
    -fsyntax-only -x c++ "${header}" || status=1
done

if ((status != 0)); then
  echo "[public-header-compat] checks failed" >&2
  exit "${status}"
fi

echo "[public-header-compat] checks passed"
