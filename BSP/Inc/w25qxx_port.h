/**
 * @file    w25qxx_port.h
 * @brief   W25QXX 移植层接口（板级接线声明与配置注入接口）
 * @details 支持通过配置结构体指定 SPI 总线实例与片选引脚；
 *          提供默认 SPI1 兼容封装供现有应用直接调用。
 */
#ifndef __W25QXX_PORT_H__
#define __W25QXX_PORT_H__

#include "w25qxx.h"
#include "spi_bus.h"
#include "main.h"

/* ==================== 移植与配置注入接口 ==================== */

/** @brief W25QXX 板级端口配置参数（支持配置注入） */
typedef struct {
    spi_bus_t       *bus;          /**< 所属 SPI 抽象总线（NULL 则默认使用 SPI1） */
    uint32_t         spi_clock_hz; /**< SPI 时钟频率（Hz，如 9000000） */
    spi_bus_mode_t   spi_mode;     /**< SPI 时钟模式（如 SPI_BUS_MODE_0） */
    uint8_t          use_dma;      /**< 是否启用 DMA（1: 启用，0: 轮询） */
    GPIO_TypeDef    *cs_port;      /**< 片选引脚端口 */
    uint32_t         cs_pin;       /**< 片选引脚 */
} W25QXX_PortConfig;

/** @brief 全局默认 W25QXX Flash 设备句柄 */
extern W25QXX_Device g_flash;

/**
 * @brief 通过外部配置注入初始化 W25QXX 设备实例
 * @param dev    要初始化的 Flash 设备对象（不能为 NULL）
 * @param config 硬件与总线配置参数（内部持久化保存，入参可为临时局部变量）
 * @return W25QXX_OK 成功；其他为失败错误码
 */
W25QXX_Result W25QXX_PortInitWithConfig(W25QXX_Device *dev, const W25QXX_PortConfig *config);

/**
 * @brief 默认初始化入口：绑定 SPI1 并使用板级默认引脚配置初始化 g_flash（向后兼容封装）
 * @note  需在 Timebase_Init() 之后调用
 */
void W25QXX_PortInit(void);

/**
 * @brief 重置 W25QXX 端口层所有静态槽位（主要供单元测试复位环境使用）
 */
void W25QXX_Port_ResetSlots(void);

#endif /* __W25QXX_PORT_H__ */
