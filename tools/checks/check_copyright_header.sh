#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

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
  resolved_path="$(resolve_path "${scan_path}")"
  if [[ ! -e "${resolved_path}" ]]; then
    echo "[copyright] error: scan path does not exist: ${scan_path}" >&2
    exit 1
  fi
  resolved_paths+=("${resolved_path}")
done

COPYRIGHT_PATTERN='^ \* SPDX-FileCopyrightText: ([1-9][0-9]{3})(-([1-9][0-9]{3}))? ([^[:space:]].*)$'

status=0
echo "[copyright] checking copyright headers"

while IFS= read -r -d '' source_file; do
  mapfile -t header_lines < <(awk '{ print; if (index($0, "*/")) exit }' "${source_file}")

  if [[ ${#header_lines[@]} -lt 5 ]]; then
    echo "[copyright] error: file is too short or missing the copyright header: ${source_file#${ROOT_DIR}/}" >&2
    status=1
    continue
  fi

  for i in "${!header_lines[@]}"; do
    header_lines[$i]="${header_lines[$i]%$'\r'}"
  done

  blank_index=$((${#header_lines[@]} - 3))
  license_index=$((blank_index + 1))
  closing_index=$((blank_index + 2))
  if [[ "${header_lines[0]}" != '/*' ||
        "${header_lines[$blank_index]}" != ' *' ||
        "${header_lines[$license_index]}" != ' * SPDX-License-Identifier: Apache-2.0' ||
        "${header_lines[$closing_index]}" != ' */' ]]; then
    echo "[copyright] error: invalid EsyProject Apache-2.0 header: ${source_file#${ROOT_DIR}/}" >&2
    status=1
    continue
  fi

  for ((i = 1; i < blank_index; i++)); do
    if [[ ! "${header_lines[$i]}" =~ ${COPYRIGHT_PATTERN} ]] ||
        [[ ${i} -eq 1 && "${BASH_REMATCH[4]}" != 'EsyProject contributors' ]]; then
      echo "[copyright] error: invalid copyright author notice: ${source_file#${ROOT_DIR}/}" >&2
      status=1
      break
    fi

    if [[ -n "${BASH_REMATCH[3]}" ]] &&
        ((10#${BASH_REMATCH[3]} < 10#${BASH_REMATCH[1]})); then
      echo "[copyright] error: reversed copyright year range: ${source_file#${ROOT_DIR}/}" >&2
      status=1
      break
    fi
  done
done < <(
  find "${resolved_paths[@]}" \
    -path '*/build/*' -prune -o \
    \( -name '*.c' -o -name '*.h' \) -type f -print0 | sort -z
)

if ((status != 0)); then
  echo "[copyright] checks failed" >&2
  exit "${status}"
fi

echo "[copyright] checks passed"
