cmake_minimum_required(VERSION 3.10)
project(lvgl-printer C)

set(CMAKE_C_STANDARD 99)
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

add_subdirectory(lvgl)
target_include_directories(lvgl PUBLIC ${PROJECT_SOURCE_DIR})

add_executable(lvgl-demo
    main.c
    evdev_touch.c
    lv_font_cn_22.c
    lv_font_cn_32.c
)
target_link_libraries(lvgl-demo lvgl m pthread)

# Fully static: the target rootfs has damaged shared libraries.
target_link_options(lvgl-demo PRIVATE -static)