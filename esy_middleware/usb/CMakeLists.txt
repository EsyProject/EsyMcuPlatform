# esy_middleware/usb/CMakeLists.txt

if(NOT ENABLE_ESY_USB_CDC AND NOT ENABLE_ESY_USB_BACKEND_TINYUSB)
    return()
endif()

add_library(esy_usb STATIC)

target_include_directories(esy_usb
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(esy_usb
    PUBLIC
        esy_common_defs
    PRIVATE
        esy_opt_default
)

add_subdirectory(src)

if(ENABLE_ESY_USB_BACKEND_TINYUSB)
    if(NOT TARGET tinyusb_device)
        message(FATAL_ERROR "USB CDC requires a consumer-provided tinyusb_device target.")
    endif()
    target_link_libraries(esy_usb PRIVATE tinyusb_device)
endif()
