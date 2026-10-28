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
  echo "[c-empty-params] error: ripgrep (rg) is required" >&2
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

echo "[c-empty-params] checking C empty parameter definitions"
set +e
empty_param_matches="$(
  rg -n -P '^[[:space:]]*[A-Za-z_][A-Za-z0-9_[:space:]\*]*[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(\)[[:space:]]*\{' \
    --glob '*.[ch]' \
    --glob '!**/build/**' \
    "${resolved_paths[@]}" 2>&1
)"
empty_param_status=$?
set -e

if ((empty_param_status == 0)); then
  printf '%s\n' "${empty_param_matches}" >&2
  echo "[c-empty-params] checks failed" >&2
  exit 1
elif ((empty_param_status > 1)); then
  printf '%s\n' "${empty_param_matches}" >&2
  exit "${empty_param_status}"
fi

echo "[c-empty-params] checks passed"
