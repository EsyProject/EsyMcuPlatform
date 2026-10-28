#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REF="${1:-HEAD}"

if ! git -C "${ROOT_DIR}" rev-parse -q --verify "${REF}^{commit}" >/dev/null; then
  echo "error: '${REF}' is not a valid commit or tag" >&2
  exit 2
fi

PREVIOUS_TAG="$(
  git -C "${ROOT_DIR}" describe --tags --abbrev=0 "${REF}^" 2>/dev/null || true
)"

if [ -n "${PREVIOUS_TAG}" ]; then
  RANGE="${PREVIOUS_TAG}..${REF}"
else
  RANGE="${REF}"
fi

DATE="$(date -u +%Y-%m-%d)"

echo "# Release Notes"
echo
echo "Reference: ${REF}"
echo "Date: ${DATE}"
if [ -n "${PREVIOUS_TAG}" ]; then
  echo "Previous tag: ${PREVIOUS_TAG}"
else
  echo "Previous tag: none"
fi
echo
echo "## Changes"
echo

if ! git -C "${ROOT_DIR}" log --format='- %s (%h)' "${RANGE}" --no-merges; then
  echo "- No commits found."
fi

echo
echo "## Verification"
echo
echo "- tools/quality/ci_quality_gate.sh test:all"
echo "- tools/quality/ci_quality_gate.sh gcov:all"
echo "- tools/checks/check_gcov_thresholds.sh"
