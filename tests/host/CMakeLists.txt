cmake_minimum_required(VERSION 3.16)
project(stage1_host_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")

# 桩头优先于真实 STM32 头
set(STUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
set(BSP_INC  ${CMAKE_CURRENT_SOURCE_DIR}/../../BSP/Inc)

# ---- 总线契约测试（纯软件，mock ops） ----
add_executable(test_bus test_bus.c)
target_include_directories(test_bus PRIVATE ${BSP_INC})

# ---- LL 层测试（编译真实 spi_stm32_ll.c + 桩） ----
add_executable(test_ll test_ll.c mock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../BSP/Src/spi_stm32_ll.c)
target_include_directories(test_ll PRIVATE ${STUB_DIR} ${BSP_INC})

# ---- Flash 可靠性与安全测试 (Stage 1.2: 编译真实 w25qxx.c) ----
add_executable(test_flash test_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../BSP/Src/w25qxx.c)
target_include_directories(test_flash PRIVATE ${STUB_DIR} ${BSP_INC})

# ---- GC9A01 驱动规范化与可移植测试 (Stage 2.1: 编译真实 gc9a01.c) ----
add_executable(test_gc9a01 test_gc9a01.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../BSP/Src/gc9a01.c)
target_compile_definitions(test_gc9a01 PRIVATE HOST_TEST=1)
target_include_directories(test_gc9a01 PRIVATE ${STUB_DIR} ${BSP_INC})

# ---- SPI 多实例与设备配置注入测试 (Stage 2.2-A) ----
add_executable(test_spi_multi_instance test_spi_multi_instance.c)
target_include_directories(test_spi_multi_instance PRIVATE ${BSP_INC})

enable_testing()
add_test(NAME bus                COMMAND test_bus)
add_test(NAME ll                 COMMAND test_ll)
add_test(NAME flash              COMMAND test_flash)
add_test(NAME gc9a01             COMMAND test_gc9a01)
add_test(NAME spi_multi_instance COMMAND test_spi_multi_instance)
