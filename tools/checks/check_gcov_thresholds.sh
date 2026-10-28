#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
POLICY_FILE="${ROOT_DIR}/tools/policy/gcov_thresholds.env"

if [ -f "${POLICY_FILE}" ]; then
  # shellcheck disable=SC1090
  source "${POLICY_FILE}"
fi

LINE_MIN="${ESY_GCOV_LINE_MIN:-85}"

if ! [[ "${LINE_MIN}" =~ ^[0-9]+$ ]] || [ "${LINE_MIN}" -lt 0 ] || [ "${LINE_MIN}" -gt 100 ]; then
  echo "error: ESY_GCOV_LINE_MIN must be an integer from 0 to 100" >&2
  exit 2
fi

# Enumerate expected projects rather than accepting an incomplete report set.
mapfile -t PROJECTS < <(
  find "${ROOT_DIR}/esy_common" "${ROOT_DIR}/esy_hal" \
    "${ROOT_DIR}/esy_middleware" \
    -type d -name build -prune -o \( -path "*/tests/project.yml" -o \
    -path "*/tests/json/project.yml" \) -type f -print | sort
)
if [ "${#PROJECTS[@]}" -eq 0 ]; then
  echo "error: no module test projects found" >&2
  exit 1
fi
REPORTS=()
REPORT_PROJECTS=()
failed=0
for project in "${PROJECTS[@]}"; do
  if [ -n "${ESY_GCOV_REPORT_ROOT:-}" ]; then
    project_rel="${project#"${ROOT_DIR}/"}"
    project_key="${project_rel%/project.yml}"
    if [[ "${project_key}" = */tests/json ]]; then
      project_key="${project_key%/tests/json}/json"
    else
      project_key="${project_key%/tests}"
    fi
    report="${ESY_GCOV_REPORT_ROOT}/${project_key}/artifacts/gcov/gcovr/coverage.txt"
  else
    report="${project%/project.yml}/build/artifacts/gcov/gcovr/coverage.txt"
  fi
  if [ ! -f "${report}" ]; then
    echo "error: ${project#"${ROOT_DIR}/"}: missing coverage report; run gcov:all" >&2
    failed=1
  else
    REPORTS+=("${report}")
    REPORT_PROJECTS+=("${project#"${ROOT_DIR}/"}")
  fi
done

for index in "${!REPORTS[@]}"; do
  report="${REPORTS[${index}]}"
  rel_report="${report#"${ROOT_DIR}/"}"
  project_dir="${REPORT_PROJECTS[${index}]%/project.yml}"
  project_key="$(
    printf "%s" "${project_dir}" \
      | tr '[:lower:]/.-' '[:upper:]___' \
      | sed 's/[^A-Z0-9_]/_/g'
  )"
  project_min_var="ESY_GCOV_LINE_MIN__${project_key}"
  project_min="${!project_min_var:-${LINE_MIN}}"

  if ! [[ "${project_min}" =~ ^[0-9]+$ ]] || [ "${project_min}" -lt 0 ] || [ "${project_min}" -gt 100 ]; then
    echo "error: ${project_min_var} must be an integer from 0 to 100" >&2
    exit 2
  fi

  total_line="$(awk '$1 == "TOTAL" { line = $0 } END { print line }' "${report}")"

  if [ -z "${total_line}" ]; then
    echo "error: ${rel_report}: missing TOTAL line" >&2
    failed=1
    continue
  fi

  if ! awk '$1 == "TOTAL" && $2 + 0 > 0 { found = 1 } END { exit !found }' "${report}"; then
    echo "error: ${rel_report}: empty coverage report" >&2
    failed=1
    continue
  fi

  coverage="$(awk '$1 == "TOTAL" { gsub(/%/, "", $4); print $4 }' "${report}")"

  if ! [[ "${coverage}" =~ ^[0-9]+$ ]]; then
    echo "error: ${rel_report}: cannot parse line coverage from TOTAL line" >&2
    failed=1
    continue
  fi

  if [ "${coverage}" -lt "${project_min}" ]; then
    echo "error: ${rel_report}: line coverage ${coverage}% is below ${project_min}%" >&2
    failed=1
  else
    echo "[gcov-threshold] ${rel_report}: line coverage ${coverage}% >= ${project_min}%"
  fi
done

exit "${failed}"
