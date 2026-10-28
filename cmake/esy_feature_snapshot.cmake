# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

function(esy_invalidate_feature_snapshot)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        file(REMOVE "${CMAKE_BINARY_DIR}/esy_features.json")
    endif()
endfunction()

function(esy_feature_snapshot_json_string value output)
    string(REPLACE "\\" "\\\\" encoded "${value}")
    string(REPLACE "\"" "\\\"" encoded "${encoded}")
    foreach(code RANGE 1 31)
        string(ASCII ${code} control)
        string(HEX "${control}" control_hex)
        string(REPLACE "${control}" "\\u00${control_hex}" encoded "${encoded}")
    endforeach()
    set(${output} "\"${encoded}\"" PARENT_SCOPE)
endfunction()

# This artifact reports the values already consumed by the current configure;
# it never selects features or changes normal/cache configuration variables.
function(esy_generate_feature_snapshot)
    set(features "{}")
    get_cmake_property(feature_variables VARIABLES)
    list(SORT feature_variables)
    foreach(feature_variable IN LISTS feature_variables)
        if(feature_variable MATCHES "^ENABLE_ESY_[A-Z0-9_]+$")
            set(enabled false)
            if(${feature_variable})
                set(enabled true)
            endif()
            string(JSON features SET "${features}" "${feature_variable}" ${enabled})
        endif()
    endforeach()

    get_filename_component(project_root "${CMAKE_SOURCE_DIR}" REALPATH)
    set(feature_import_file "")
    if(ESY_FEATURE_IMPORT_FILE)
        get_filename_component(feature_import_file "${ESY_FEATURE_IMPORT_FILE}" REALPATH
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    set(snapshot "{}")
    string(JSON snapshot SET "${snapshot}" schema_version 1)
    foreach(field IN ITEMS project_root feature_import_file)
        esy_feature_snapshot_json_string("${${field}}" json_value)
        string(JSON snapshot SET "${snapshot}" "${field}" "${json_value}")
    endforeach()
    esy_feature_snapshot_json_string("${ESY_TARGET_DEVICE}" json_value)
    string(JSON snapshot SET "${snapshot}" target_device "${json_value}")
    string(JSON snapshot SET "${snapshot}" features "${features}")
    # A later generator step can still fail after writing this report. Consumers
    # must successfully configure/build before treating the snapshot as usable.
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/esy_features.json" CONTENT "${snapshot}\n")
endfunction()
