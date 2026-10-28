#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

usage() {
  echo "usage: $0 [--last | --message <subject> | <commit-msg-file>]"
}

subject=""

if [[ $# -eq 0 ]] || [[ "${1:-}" == "--last" ]]; then
  if ! git rev-parse --git-dir >/dev/null 2>&1; then
    echo "[commit-message] error: not inside a git repository" >&2
    exit 2
  fi
  subject="$(git log -1 --pretty=%s)"
elif [[ "${1:-}" == "--message" ]]; then
  if [[ $# -ne 2 ]]; then
    usage >&2
    exit 2
  fi
  subject="$2"
elif [[ $# -eq 1 ]] && [[ -f "$1" ]]; then
  subject="$(head -n 1 "$1")"
else
  usage >&2
  exit 2
fi

if [[ -z "${subject}" ]]; then
  echo "[commit-message] violation: subject is empty" >&2
  exit 1
fi

if [[ "${subject}" == *$'\r'* ]] || [[ "${subject}" == *$'\n'* ]]; then
  echo "[commit-message] violation: subject must be a single line" >&2
  exit 1
fi

if ! printf '%s' "${subject}" | LC_ALL=C grep -Eq '^(Initial commit|(feat|fix|docs|style|refactor|test|chore|ci)(\([a-z0-9][a-z0-9-]*\))?: [ -~]+)$'; then
  echo "[commit-message] violation: subject must match '<type>: <summary>' or '<type>(<scope>): <summary>' with an allowed type, or be exactly 'Initial commit'" >&2
  echo "[commit-message] allowed types: feat, fix, docs, style, refactor, test, chore, ci" >&2
  echo "[commit-message] optional scope: start with a lowercase letter or digit; use only lowercase letters, digits, and hyphens" >&2
  echo "[commit-message] actual: ${subject}" >&2
  exit 1
fi

echo "[commit-message] passed"
