/**
 * @file    gc9a01_port.h
 * @brief   GC9A01 移植层接口（板级接线声明与配置注入接口）
 * @details 支持通过配置结构体指定 SPI 总线实例与控制引脚；
 *          提供默认 SPI1 兼容封装供现有应用直接调用。
 */
#ifndef __GC9A01_PORT_H__
#define __GC9A01_PORT_H__

#include "gc9a01.h"
#include "spi_bus.h"
#include "main.h"

/* ==================== 移植与配置注入接口 ==================== */

/** @brief GC9A01 板级端口配置参数（支持配置注入） */
typedef struct {
    spi_bus_t       *bus;          /**< 所属 SPI 抽象总线（NULL 则默认使用 SPI1） */
    uint32_t         spi_clock_hz; /**< SPI 时钟频率（Hz，如 18000000） */
    spi_bus_mode_t   spi_mode;     /**< SPI 时钟模式（如 SPI_BUS_MODE_0） */
    uint8_t          use_dma;      /**< 是否启用 DMA（1: 启用，0: 轮询） */
    GPIO_TypeDef    *cs_port;      /**< 片选引脚端口 */
    uint32_t         cs_pin;       /**< 片选引脚 */
    GPIO_TypeDef    *rst_port;     /**< 复位引脚端口 */
    uint32_t         rst_pin;      /**< 复位引脚 */
    GPIO_TypeDef    *dc_port;      /**< 数据/命令选择引脚端口 */
    uint32_t         dc_pin;       /**< 数据/命令选择引脚 */
} GC9A01_PortConfig;

/** @brief 全局默认 GC9A01 设备句柄 */
extern LCD_Device g_lcd;

/**
 * @brief 通过外部配置注入初始化 GC9A01 设备实例
 * @param dev    要初始化的 LCD 设备对象（不能为 NULL）
 * @param config 硬件与总线配置参数（内部持久化保存，入参可为临时局部变量）
 * @return LCD_OK 成功；其他为失败错误码
 */
LCD_Result GC9A01_PortInitWithConfig(LCD_Device *dev, const GC9A01_PortConfig *config);

/**
 * @brief 默认初始化入口：绑定 SPI1 并使用板级默认引脚配置初始化 g_lcd（向后兼容封装）
 * @return LCD_OK 成功；其他为初始化失败错误码
 * @note  需在 Timebase_Init() 之后调用
 */
LCD_Result GC9A01_PortInit(void);

/**
 * @brief 检查指定 GPIO 引脚是否已被 GC9A01 占用为控制线 (CS, RST, DC)
 * @param port GPIO 端口
 * @param pin  GPIO 引脚掩码
 * @return 1: 已被占用; 0: 未占用
 */
uint8_t GC9A01_Port_IsPinUsed(const GPIO_TypeDef *port, uint32_t pin);

/**
 * @brief 重置 GC9A01 端口层所有静态槽位（主要供单元测试复位环境使用）
 */
void GC9A01_Port_ResetSlots(void);

#endif /* __GC9A01_PORT_H__ */
