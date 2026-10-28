# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

add_library(esy_common_smf STATIC src/esy_smf.c)
add_library(Esy::common_smf ALIAS esy_common_smf)
set_target_properties(esy_common_smf PROPERTIES EXPORT_NAME common_smf)
target_compile_features(esy_common_smf PUBLIC c_std_11)
target_include_directories(esy_common_smf PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(esy_common_smf PUBLIC esy_common_defs)

if(TARGET esy_opt_default)
    target_link_libraries(esy_common_smf PRIVATE esy_opt_default)
endif()
