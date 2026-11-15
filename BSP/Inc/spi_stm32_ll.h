/**
 * @file    spi_stm32_ll.h
 * @brief   SPI 总线的 STM32F1 LL 实现（MCU 相关）
 * @details 仅由板级文件（bsp_spi.c）包含；上层驱动只依赖 spi_bus.h。
 */
#ifndef __SPI_STM32_LL_H__
#define __SPI_STM32_LL_H__

#include "main.h"
#include "stm32f1xx_ll_spi.h"
#include "spi_bus.h"

/**
 * @brief LL 层统一状态码
 * @note  全部为非正值，可直接作为 spi_bus_transfer 的返回值透传
 */
typedef enum {
    SPI_LL_OK             = 0,   /**< 成功 */
    SPI_LL_ERROR_PARAM    = -1,  /**< 参数非法 */
    SPI_LL_ERROR_TIMEOUT  = -2,  /**< 等待超时 */
    SPI_LL_ERROR_DMA      = -3,  /**< DMA 传输错误（TEIF） */
    SPI_LL_ERROR_BUSY     = -4,  /**< 外设正忙，无法配置 */
    SPI_LL_ERROR_RECOVERY = -5,  /**< 异常恢复失败 */
} spi_ll_status_t;

/** @brief STM32 SPI 硬件描述（板级填写） */
typedef struct {
    SPI_TypeDef  *periph;      /* SPI1 / SPI2 */
    uint32_t      pclk_hz;     /* 该 SPI 外设的输入时钟频率（Hz） */

    GPIO_TypeDef *sck_port;
    uint32_t      sck_pin;
    GPIO_TypeDef *miso_port;   /* 可为 NULL（只发不收） */
    uint32_t      miso_pin;
    GPIO_TypeDef *mosi_port;
    uint32_t      mosi_pin;

    DMA_TypeDef  *dma;         /* DMA 控制器，NULL 表示不启用 DMA */
    uint32_t      dma_tx_ch;   /* 发送通道（SPI1_TX = DMA1_Channel3） */
    uint32_t      dma_rx_ch;   /* 接收通道（SPI1_RX = DMA1_Channel2） */
} spi_stm32_ll_hw_t;

/** @brief STM32 SPI 总线实例 */
typedef struct {
    spi_bus_t            bus;           /* 对外的抽象总线 */
    const spi_stm32_ll_hw_t *hw;        /* 硬件描述（生命周期需覆盖实例） */
    spi_bus_config_t     current;       /* 当前已生效参数（用于避免重复配置） */
    uint8_t              configured;    /* 是否已完成首次配置 */
    uint32_t             recover_count; /* 本实例异常恢复次数（与全局统计隔离） */
    int32_t              last_error;    /* 本实例最后一次错误码（与全局统计隔离） */
} spi_stm32_ll_t;

/**
 * @brief 初始化 GPIO 与 SPI 外设（完成引脚复用与时钟使能）
 * @note  初始化后总线默认 1MHz / 模式 0，实际速率由设备传输前 configure 切换
 */
void spi_stm32_ll_init(spi_stm32_ll_t *ll, const spi_stm32_ll_hw_t *hw);

/** @brief 获取抽象总线句柄 */
spi_bus_t *spi_stm32_ll_bus(spi_stm32_ll_t *ll);

/**
 * @brief  测试/恢复用：把 SPI/DMA 拉回可用态（与错误路径同一清理逻辑）
 * @return spi_ll_status_t（0 成功，负值失败）
 * @note   不操作任何 CS 引脚；供异常恢复测试或故障后手动恢复调用。
 */
int spi_stm32_ll_recover(spi_stm32_ll_t *ll);

/** @brief 查询实例恢复调用次数 */
uint32_t spi_stm32_ll_get_recover_count(const spi_stm32_ll_t *ll);

/** @brief 查询实例最后一次错误码 */
int32_t spi_stm32_ll_get_last_error(const spi_stm32_ll_t *ll);

/** @brief 清除实例错误状态 */
void spi_stm32_ll_clear_error(spi_stm32_ll_t *ll);

#endif /* __SPI_STM32_LL_H__ */
