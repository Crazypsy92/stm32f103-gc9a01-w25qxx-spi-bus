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

enable_testing()
add_test(NAME bus COMMAND test_bus)
add_test(NAME ll  COMMAND test_ll)
