cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(kaypro_capture C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(kaypro_capture
    kaypro_capture.c
)

pico_generate_pio_header(kaypro_capture
    ${CMAKE_CURRENT_LIST_DIR}/video_sampler.pio
)

target_link_libraries(kaypro_capture
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_uart
)

pico_enable_stdio_usb(kaypro_capture 1)
pico_enable_stdio_uart(kaypro_capture 0)
pico_add_extra_outputs(kaypro_capture)

target_compile_options(kaypro_capture PRIVATE -O2)
