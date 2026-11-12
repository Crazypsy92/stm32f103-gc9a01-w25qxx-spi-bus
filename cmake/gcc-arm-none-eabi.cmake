set(CMAKE_SYSTEM_NAME               Generic)
set(CMAKE_SYSTEM_PROCESSOR          arm)

set(CMAKE_C_COMPILER_ID GNU)
set(CMAKE_CXX_COMPILER_ID GNU)

# Some default GCC settings
set(TOOLCHAIN_PREFIX                arm-none-eabi-)

# Keep explicit compiler locations when CMake configures its ABI test project.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES TOOLCHAIN_GCC TOOLCHAIN_GXX)

# 编译器若只写名字，CMake 只能在调用进程的 PATH 中查找；终端可用不代表 IDE
# 扩展进程也可用（常见 "not a full path and was not found in the PATH"）。
# 这里先解析成绝对路径，使配置不再依赖外部 PATH 环境。
# Generic 平台下 CMake 没有默认系统程序路径，显式给出常见安装位置作为兜底；
# 也可用环境变量 ARM_TOOLCHAIN_BIN 指定自定义工具链目录，方便换机器移植。
set(TOOLCHAIN_SEARCH_PATHS /usr/bin /usr/local/bin)
if(DEFINED ENV{ARM_TOOLCHAIN_BIN})
    list(PREPEND TOOLCHAIN_SEARCH_PATHS "$ENV{ARM_TOOLCHAIN_BIN}")
endif()

find_program(TOOLCHAIN_GCC NAMES ${TOOLCHAIN_PREFIX}gcc PATHS ${TOOLCHAIN_SEARCH_PATHS})
find_program(TOOLCHAIN_GXX NAMES ${TOOLCHAIN_PREFIX}g++ PATHS ${TOOLCHAIN_SEARCH_PATHS})
if(NOT TOOLCHAIN_GCC)
    message(FATAL_ERROR
        "ARM toolchain '${TOOLCHAIN_PREFIX}gcc' not found. "
        "Install gcc-arm-none-eabi or add its bin directory to PATH.")
endif()
get_filename_component(TOOLCHAIN_BIN_DIR "${TOOLCHAIN_GCC}" DIRECTORY)

set(CMAKE_C_COMPILER                "${TOOLCHAIN_GCC}")
set(CMAKE_ASM_COMPILER              "${CMAKE_C_COMPILER}")
# C++ is optional for this C/ASM firmware.
if(TOOLCHAIN_GXX)
    set(CMAKE_CXX_COMPILER          "${TOOLCHAIN_GXX}")
endif()
set(CMAKE_OBJCOPY                   "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}objcopy")
set(CMAKE_SIZE                      "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}size")

set(CMAKE_EXECUTABLE_SUFFIX_ASM     ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C       ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX     ".elf")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# MCU specific flags
set(TARGET_FLAGS "-mcpu=cortex-m3 ")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${TARGET_FLAGS}")
set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS} -x assembler-with-cpp -MMD -MP")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fdata-sections -ffunction-sections -fstack-usage")

# The cyclomatic-complexity parameter must be defined for the Cyclomatic complexity feature in STM32CubeIDE to work.
# However, most GCC toolchains do not support this option, which causes a compilation error; for this reason, the feature is disabled by default.
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fcyclomatic-complexity")

set(CMAKE_C_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_C_FLAGS_RELEASE "-Os -g0")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -g0")

set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fno-rtti -fno-exceptions -fno-threadsafe-statics")

set(CMAKE_EXE_LINKER_FLAGS "${TARGET_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T \"${CMAKE_SOURCE_DIR}/STM32F103xx_FLASH.ld\"")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --specs=nano.specs")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-Map=${CMAKE_PROJECT_NAME}.map -Wl,--gc-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--print-memory-usage")
set(TOOLCHAIN_LINK_LIBRARIES "m")
