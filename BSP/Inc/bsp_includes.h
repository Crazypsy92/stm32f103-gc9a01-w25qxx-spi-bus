/**
 * @file bsp_includes.h
 * @brief BSP 总头文件（原 MyApplication.h）
 * @details 裸机版统一头文件入口：包含系统基础头文件、标准库头文件及全部 BSP 模块头文件。
 *          已移除 FreeRTOS 相关头文件，工程不再依赖任何 RTOS。
 * @author 西伯利亚小菜狗
 * @version 2.0 (bare-metal)
 */
#ifndef __BSP_INCLUDES_H__
#define __BSP_INCLUDES_H__

/* Includes ------------------------------------------------------------------*/
#include "main.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

/* BSP 模块 */
#include "delay.h"          /* 统一时间基准 Timebase */
#include "multi_button.h"   /* MultiButton 状态机（裸机） */
#include "key_port.h"       /* 按键硬件适配层 */
#include "spi_bus.h"        /* SPI 总线抽象接口（MCU 无关） */
#include "bsp_spi.h"        /* 板级 SPI1 总线（设备共享） */
#include "gc9a01.h"         /* GC9A01 TFT 设备驱动 */
#include "gc9a01_port.h"    /* GC9A01 移植层（板级引脚/SPI 配置） */
#include "w25qxx.h"      /* W25Qxx 设备驱动 */
#include "w25qxx_port.h"    /* W25QXX 移植层（板级引脚/SPI 配置） */
#if (defined(CONFIG_HIL_TESTS) && (CONFIG_HIL_TESTS != 0)) || (defined(CONFIG_TFT_ONLY) && (CONFIG_TFT_ONLY != 0))
#include "app_hil_test.h"   /* 目标 HIL 自动化回归与诊断模块 */
#else
#include "app_demo.h"       /* 交互式应用 Demo 模块 */
#endif

#ifndef DEBUG
#define DEBUG 0
#endif
#if DEBUG
#define DEBUG_PRINTF(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINTF(fmt, ...) do {} while (0)
#endif

#ifndef App_Printf
#define App_Printf(...) printf(__VA_ARGS__)
#endif

#endif /* __BSP_INCLUDES_H__ */
