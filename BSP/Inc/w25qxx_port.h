/**
 * @file    w25qxx_port.h
 * @brief   W25QXX 移植层接口（板级接线声明）
 * @details 板级参数（引脚 / SPI 外设 / 速率）统一放在 w25qxx_port.c 顶部；
 *          换板/换 MCU 时只需重写 w25qxx_port.c，w25qxx.c/h 无需改动。
 */
#ifndef __W25QXX_PORT_H__
#define __W25QXX_PORT_H__

#include "w25qxx.h"

/* ==================== 移植接口 ==================== */

/** @brief 全局 W25QXX Flash 设备句柄（在 w25qxx_port.c 中定义） */
extern W25QXX_Device g_flash;

/**
 * @brief 初始化 SPI 总线并挂载 W25QXX 设备
 * @note  需在 Timebase_Init() 之后调用（内部有上电延时）；
 *        完成后 g_flash.id 为读到的芯片 ID（0x0000/0xFFFF 表示未识别）
 */
void W25QXX_PortInit(void);

#endif /* __W25QXX_PORT_H__ */
