#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

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

if ! command -v rg >/dev/null 2>&1; then
  echo "[source-language] error: ripgrep (rg) is required" >&2
  exit 127
fi

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

resolved_paths=()
for scan_path in "${scan_paths[@]}"; do
  resolved_paths+=("$(resolve_path "${scan_path}")")
done

echo "[source-language] checking English-only comments and build metadata"
set +e
han_matches="$(
  rg -n -P '[\p{sc=Han}]' \
    --glob '*.[ch]' \
    --glob '*.cmake' \
    --glob 'CMakeLists.txt' \
    --glob '!**/build/**' \
    "${resolved_paths[@]}" 2>&1
)"
han_status=$?
set -e

if ((han_status == 0)); then
  printf '%s\n' "${han_matches}" >&2
  echo "[source-language] checks failed" >&2
  exit 1
elif ((han_status > 1)); then
  printf '%s\n' "${han_matches}" >&2
  exit "${han_status}"
fi

echo "[source-language] checks passed"
