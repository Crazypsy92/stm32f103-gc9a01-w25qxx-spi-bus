/**
 * @file    bsp_spi.h
 * @brief   板级 SPI 总线接口（与 MCU 无关）
 * @details 同一物理 SPI 外设只在这里创建一次，多个设备通过
 *          BSP_SPI1_GetBus() 取得同一个 spi_bus_t 共享使用。
 */
#ifndef __BSP_SPI_H__
#define __BSP_SPI_H__

#include "spi_bus.h"

/** @brief 板级 SPI 总线标识 */
typedef enum {
    BSP_SPI_BUS_1 = 0,
    BSP_SPI_BUS_2 = 1,
    BSP_SPI_BUS_MAX
} bsp_spi_id_t;

/**
 * @brief 获取指定 ID 的 SPI 抽象总线（按需惰性初始化对应硬件）
 * @param id BSP_SPI_BUS_1 或 BSP_SPI_BUS_2
 * @return spi_bus_t* 成功；NULL 参数非法
 */
spi_bus_t *BSP_SPI_GetBus(bsp_spi_id_t id);

/**
 * @brief 获取 SPI1 抽象总线（专供 GC9A01 TFT 屏幕使用）
 */
spi_bus_t *BSP_SPI1_GetBus(void);

/**
 * @brief 获取 SPI2 抽象总线（专供 W25Q128 Flash 存储使用）
 */
spi_bus_t *BSP_SPI2_GetBus(void);

/**
 * @brief 查询指定 SPI 总线是否已完成硬件初始化并处于就绪态
 * @param id BSP_SPI_BUS_1 或 BSP_SPI_BUS_2
 * @return 1: 已就绪; 0: 未初始化或无效
 */
uint8_t BSP_SPI_IsBusActive(bsp_spi_id_t id);

/**
 * @brief 重置板级 SPI 总线单例状态（主要供单元测试复位环境使用）
 */
void BSP_SPI_ResetBuses(void);

/**
 * @brief  对指定总线执行硬件级异常恢复
 * @param  bus 目标抽象总线指针
 * @return 0 成功，负值失败
 */
int BSP_SPI_RecoverBus(spi_bus_t *bus);

/**
 * @brief  板级 SPI1 恢复入口（测试/故障用，兼容原 API）
 * @return 0 成功，负值失败
 * @note   调用 spi_stm32_ll_recover，不触碰任何 CS；供恢复测试使用。
 */
int BSP_SPI1_Recover(void);

#endif /* __BSP_SPI_H__ */
