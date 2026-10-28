#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TAG="${1:-}"

if [ -z "${TAG}" ]; then
  TAG="$(git -C "${ROOT_DIR}" describe --tags --exact-match 2>/dev/null || true)"
fi

if [ -z "${TAG}" ]; then
  echo "error: release tag is required" >&2
  exit 2
fi

if ! [[ "${TAG}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]]; then
  echo "error: release tag '${TAG}' must match vMAJOR.MINOR.PATCH[-PRERELEASE]" >&2
  exit 1
fi

if ! git -C "${ROOT_DIR}" rev-parse -q --verify "refs/tags/${TAG}" >/dev/null; then
  echo "error: tag '${TAG}' does not exist in this repository" >&2
  exit 1
fi

if [ ! -f "${ROOT_DIR}/CHANGELOG.md" ]; then
  echo "error: CHANGELOG.md is required for releases" >&2
  exit 1
fi

VERSION="${TAG#v}"
if ! awk -v tag="${TAG}" -v version="${VERSION}" '
  /^##[ \t]+/ && $1 == "##" {
    heading = $2
    if (heading ~ /^\[.*\]$/) heading = substr(heading, 2, length(heading) - 2)
    if (heading == tag || heading == version) found = 1
  }
  END { exit !found }
' "${ROOT_DIR}/CHANGELOG.md"; then
  echo "error: CHANGELOG.md must contain a release section for ${TAG} or ${VERSION}" >&2
  exit 1
fi

if [ ! -f "${ROOT_DIR}/docs/release/release_tag_checklist.md" ]; then
  echo "error: docs/release/release_tag_checklist.md is required" >&2
  exit 1
fi

if [ ! -x "${ROOT_DIR}/tools/quality/generate_changelog.sh" ]; then
  echo "error: tools/quality/generate_changelog.sh must be executable" >&2
  exit 1
fi

if [ ! -x "${ROOT_DIR}/tools/checks/check_gcov_thresholds.sh" ]; then
  echo "error: tools/checks/check_gcov_thresholds.sh must be executable" >&2
  exit 1
fi

echo "[release-readiness] ${TAG} passed"
