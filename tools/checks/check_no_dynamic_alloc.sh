#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 -B "${repo_root}/tools/checks/check_no_dynamic_alloc.py" --root "${repo_root}" "$@"
