/**
 * @file    GC9A01_port.h
 * @brief   GC9A01 移植层接口（板级接线声明）
 * @details 板级参数（引脚 / SPI 外设 / 速率）统一放在 GC9A01_port.c 顶部；
 *          换板/换 MCU 时只需重写 GC9A01_port.c，GC9A01.c/h 无需改动。
 */
#ifndef __GC9A01_PORT_H__
#define __GC9A01_PORT_H__

#include "gc9a01.h"

/* ==================== 移植接口 ==================== */

/** @brief 全局 GC9A01 设备句柄（在 GC9A01_port.c 中定义） */
extern LCD_Device g_lcd;

/**
 * @brief 初始化 SPI1 总线、控制引脚并完成 GC9A01 初始化
 * @return LCD_OK 成功；其他为初始化失败错误码
 * @note  需在 Timebase_Init() 之后调用
 */
LCD_Result GC9A01_PortInit(void);

#endif /* __GC9A01_PORT_H__ */
