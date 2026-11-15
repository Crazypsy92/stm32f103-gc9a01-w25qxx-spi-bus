cmake_minimum_required(VERSION 3.22)

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Define the default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME stm32f103_gc9a01_w25qxx)

# Enable compile commands database for clangd / LSP indexing
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME} LANGUAGES C ASM)
message("Build type: " ${CMAKE_BUILD_TYPE})

# Create executable target
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Base BSP sources
set(PROJECT_SOURCES
    BSP/Src/delay.c
    BSP/Src/multi_button.c
    BSP/Src/key_port.c
    BSP/Src/spi_stm32_ll.c
    BSP/Src/bsp_spi.c
    BSP/Src/gc9a01.c
    BSP/Src/gc9a01_port.c
    BSP/Src/w25qxx.c
    BSP/Src/w25qxx_port.c
)

# Project options
option(CONFIG_DUAL_SPI "Enable Dual SPI mode: TFT on SPI1 (DC=PA4), Flash on SPI2 (PB13/14/15)" ON)
option(CONFIG_HIL_TESTS "Enable hardware-in-the-loop automated test suite" OFF)
option(CONFIG_TFT_ONLY "Enable minimal TFT diagnostic mode" OFF)

if(CONFIG_HIL_TESTS OR CONFIG_TFT_ONLY)
    list(APPEND PROJECT_SOURCES tests/target/app_hil_test.c)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE tests/target)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CONFIG_HIL_TESTS=1)
    message(STATUS "Application Mode: HIL / Diagnostic Tests")
else()
    list(APPEND PROJECT_SOURCES BSP/Src/app_demo.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CONFIG_HIL_TESTS=0)
    message(STATUS "Application Mode: Interactive Demo")
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${PROJECT_SOURCES})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE BSP/Inc)

if(CONFIG_DUAL_SPI)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CONFIG_DUAL_SPI=1)
    message(STATUS "SPI Mode: DUAL_SPI (TFT on SPI1 DC=PA4, Flash on SPI2 PB13/14/15)")
else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CONFIG_DUAL_SPI=0)
    message(STATUS "SPI Mode: SHARED_SPI1 (TFT and Flash on SPI1 DC=PB13)")
endif()

if(CONFIG_TFT_ONLY)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CONFIG_TFT_ONLY=1)
    message(STATUS "Diagnostic Mode: TFT_ONLY enabled")
else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CONFIG_TFT_ONLY=0)
endif()

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
)

# Post-build: print firmware size, generate .bin and .hex
if(CMAKE_OBJCOPY AND CMAKE_SIZE)
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
        COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.hex
        COMMENT "Generated binary/hex and firmware size summary"
    )
endif()
