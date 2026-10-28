# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

target_sources(esy_touch PRIVATE esy_xpt2046.c)
target_link_libraries(esy_touch PRIVATE esy_hal)
