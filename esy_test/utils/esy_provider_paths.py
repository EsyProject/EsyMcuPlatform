# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Share offline provider lookup with directly executed host test scripts."""

from pathlib import Path
import runpy


PLATFORM = Path(__file__).resolve().parents[2]
PROVIDER_CACHE = runpy.run_path(str(PLATFORM / "tools/providers/provider_cache.py"))
cached_provider = PROVIDER_CACHE["cached_provider"]
load_cached_environment = PROVIDER_CACHE["load_cached_environment"]
