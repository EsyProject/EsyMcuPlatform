# esy_middleware/wifi/CMakeLists.txt

add_library(esy_wifi_manager STATIC)

target_include_directories(esy_wifi_manager
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(esy_wifi_manager
    PUBLIC
        esy_common
    PRIVATE
        esy_opt_default
        esy_hal
)

add_subdirectory(src)
