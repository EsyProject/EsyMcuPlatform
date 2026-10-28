# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

target_sources(esy_lcd PRIVATE esy_ili9488.c)
target_link_libraries(esy_lcd PRIVATE esy_hal)
