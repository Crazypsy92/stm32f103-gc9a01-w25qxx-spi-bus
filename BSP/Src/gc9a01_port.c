/**
 * @file    gc9a01_port.c
 * @brief   GC9A01 移植层实现（STM32F1 + LL 库，支持配置注入与多实例）
 * @details 负责设备 GPIO 控制、SPI 总线绑定与延时实现，填充 LCD_Port 回调表。
 *          支持通过 GC9A01_PortInitWithConfig 注入任意 SPI 总线与 GPIO 引脚；
 *          GC9A01_PortInit 提供默认 SPI1 兼容封装。
 */
#include "gc9a01_port.h"
#include "bsp_spi.h"
#include "main.h"
#include "delay.h"
#include <string.h>

/* ================================================================================== */
/* 默认板级硬件配置（SPI1 接线）                                                      */
/* ================================================================================== */

#define GC9A01_DEFAULT_CS_PORT      GPIOA
#define GC9A01_DEFAULT_CS_PIN       LL_GPIO_PIN_9

#define GC9A01_DEFAULT_RST_PORT     GPIOA
#define GC9A01_DEFAULT_RST_PIN      LL_GPIO_PIN_10

#define GC9A01_DEFAULT_DC_PORT      GPIOB
#define GC9A01_DEFAULT_DC_PIN       LL_GPIO_PIN_13

#define GC9A01_DEFAULT_SPI_CLOCK_HZ 18000000U
#define GC9A01_DEFAULT_SPI_MODE     SPI_BUS_MODE_0
#define GC9A01_DEFAULT_SPI_USE_DMA  1

#define GC9A01_MAX_INSTANCES        2U

/* ================================================================================== */
/* 内部上下文结构定义                                                                 */
/* ================================================================================== */

typedef struct {
    spi_bus_t        *bus;
    spi_bus_config_t  cfg;
    GPIO_TypeDef     *cs_port;
    uint32_t          cs_pin;
    GPIO_TypeDef     *rst_port;
    uint32_t          rst_pin;
    GPIO_TypeDef     *dc_port;
    uint32_t          dc_pin;
    LCD_Port          port;
    uint8_t           in_use;
} GC9A01_HW;

static GC9A01_HW s_lcd_hw[GC9A01_MAX_INSTANCES];

/* 全局默认设备句柄，供应用层直接使用 */
LCD_Device g_lcd;

/* ================================================================================== */
/* 内部辅助：GPIO 时钟使能                                                            */
/* ================================================================================== */

static void gpio_clock_enable(GPIO_TypeDef *port)
{
    if (port == GPIOA) {
        LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOA);
    } else if (port == GPIOB) {
        LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOB);
    } else if (port == GPIOC) {
        LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOC);
    }
}

/* ================================================================================== */
/* LCD_Port 回调实现（通过 ctx 上下文动态访问绑定的硬件与总线）                        */
/* ================================================================================== */

static LCD_Result Port_Cs(void *ctx, uint8_t level)
{
    GC9A01_HW *hw = (GC9A01_HW *)ctx;
    if (hw == NULL || hw->bus == NULL) {
        return LCD_ERROR_PARAM;
    }

    if (level == 0U) {
        /* 占用本设备绑定的总线并切换至本设备参数 */
        if (spi_bus_acquire(hw->bus, hw, &hw->cfg) != 0) {
            return LCD_ERROR_BUS;
        }
        LL_GPIO_ResetOutputPin(hw->cs_port, hw->cs_pin);
        return LCD_OK;
    } else {
        /* 仅当完全释放总线时拉高片选；同 owner 嵌套期间片选保持低电平有效 */
        int rel = spi_bus_release(hw->bus, hw);
        if (rel > 0) {
            LL_GPIO_SetOutputPin(hw->cs_port, hw->cs_pin);
            return LCD_OK;
        } else if (rel == 0) {
            return LCD_OK;
        } else {
            return LCD_ERROR_BUS;
        }
    }
}

static void Port_Rst(void *ctx, uint8_t level)
{
    GC9A01_HW *hw = (GC9A01_HW *)ctx;
    if (hw == NULL || hw->rst_port == NULL) {
        return;
    }
    if (level) {
        LL_GPIO_SetOutputPin(hw->rst_port, hw->rst_pin);
    } else {
        LL_GPIO_ResetOutputPin(hw->rst_port, hw->rst_pin);
    }
}

static void Port_Dc(void *ctx, uint8_t level)
{
    GC9A01_HW *hw = (GC9A01_HW *)ctx;
    if (hw == NULL || hw->dc_port == NULL) {
        return;
    }
    if (level) {
        LL_GPIO_SetOutputPin(hw->dc_port, hw->dc_pin);
    } else {
        LL_GPIO_ResetOutputPin(hw->dc_port, hw->dc_pin);
    }
}

static LCD_Result Port_Write(void *ctx, const uint8_t *buf, uint16_t len)
{
    GC9A01_HW *hw = (GC9A01_HW *)ctx;
    if (hw == NULL || hw->bus == NULL) {
        return LCD_ERROR_PARAM;
    }
    if (len == 0U) {
        return LCD_OK;
    }
    if (buf == NULL) {
        return LCD_ERROR_PARAM;
    }

    /* 必须是当前总线持有者方可传输 */
    if (spi_bus_owner(hw->bus) != hw) {
        return LCD_ERROR_BUS;
    }

    int32_t res = spi_bus_transfer(hw->bus, hw, buf, NULL, len);
    if (res != 0) {
        return (res == -2) ? LCD_ERROR_TIMEOUT : LCD_ERROR_BUS;
    }
    return LCD_OK;
}

static void Port_DelayMs(void *ctx, uint32_t ms)
{
    (void)ctx;
    Delay_ms(ms);
}

/* ================================================================================== */
/* 硬件引脚初始化                                                                     */
/* ================================================================================== */

static void GC9A01_GPIO_Init(const GC9A01_HW *hw)
{
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    gpio_clock_enable(hw->cs_port);
    gpio_clock_enable(hw->rst_port);
    gpio_clock_enable(hw->dc_port);

    GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
    GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;

    GPIO_InitStruct.Pin = hw->cs_pin;
    LL_GPIO_Init(hw->cs_port, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = hw->rst_pin;
    LL_GPIO_Init(hw->rst_port, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = hw->dc_pin;
    LL_GPIO_Init(hw->dc_port, &GPIO_InitStruct);

    /* 默认空闲高电平 */
    LL_GPIO_SetOutputPin(hw->cs_port, hw->cs_pin);
    LL_GPIO_SetOutputPin(hw->rst_port, hw->rst_pin);
    LL_GPIO_SetOutputPin(hw->dc_port, hw->dc_pin);
}

/* ================================================================================== */
/* 对外移植与配置注入接口实现                                                         */
/* ================================================================================== */

LCD_Result GC9A01_PortInitWithConfig(LCD_Device *dev, const GC9A01_PortConfig *config)
{
    if (dev == NULL || config == NULL) {
        return LCD_ERROR_PARAM;
    }

    /* 分配并持久化上下文槽位（slot 0 保留给默认 g_lcd） */
    uint32_t slot = 0;
    if (dev != &g_lcd) {
        for (slot = 1; slot < GC9A01_MAX_INSTANCES; slot++) {
            if (!s_lcd_hw[slot].in_use) break;
        }
        if (slot >= GC9A01_MAX_INSTANCES) {
            slot = GC9A01_MAX_INSTANCES - 1U;
        }
    }

    GC9A01_HW *hw = &s_lcd_hw[slot];
    hw->in_use = 1U;

    /* 绑定总线（若未指定则使用 SPI1）与参数配置 */
    hw->bus = (config->bus != NULL) ? config->bus : BSP_SPI1_GetBus();
    hw->cfg.clock_hz = (config->spi_clock_hz > 0U) ? config->spi_clock_hz : GC9A01_DEFAULT_SPI_CLOCK_HZ;
    hw->cfg.mode     = config->spi_mode;
    hw->cfg.use_dma  = config->use_dma;

    hw->cs_port  = (config->cs_port != NULL) ? config->cs_port : GC9A01_DEFAULT_CS_PORT;
    hw->cs_pin   = (config->cs_pin != 0U) ? config->cs_pin : GC9A01_DEFAULT_CS_PIN;
    hw->rst_port = (config->rst_port != NULL) ? config->rst_port : GC9A01_DEFAULT_RST_PORT;
    hw->rst_pin  = (config->rst_pin != 0U) ? config->rst_pin : GC9A01_DEFAULT_RST_PIN;
    hw->dc_port  = (config->dc_port != NULL) ? config->dc_port : GC9A01_DEFAULT_DC_PORT;
    hw->dc_pin   = (config->dc_pin != 0U) ? config->dc_pin : GC9A01_DEFAULT_DC_PIN;

    hw->port.cs            = Port_Cs;
    hw->port.rst           = Port_Rst;
    hw->port.dc            = Port_Dc;
    hw->port.bl            = NULL;
    hw->port.delay_ms      = Port_DelayMs;
    hw->port.write         = Port_Write;
    hw->port.max_chunk_len = 65535U;
    hw->port.ctx           = hw;

    GC9A01_GPIO_Init(hw);

    return GC9A01_Init(dev, &hw->port);
}

LCD_Result GC9A01_PortInit(void)
{
    static const GC9A01_PortConfig def_cfg = {
        .bus          = NULL, /* 自动绑定 BSP_SPI1_GetBus() */
        .spi_clock_hz = GC9A01_DEFAULT_SPI_CLOCK_HZ,
        .spi_mode     = GC9A01_DEFAULT_SPI_MODE,
        .use_dma      = GC9A01_DEFAULT_SPI_USE_DMA,
        .cs_port      = GC9A01_DEFAULT_CS_PORT,
        .cs_pin       = GC9A01_DEFAULT_CS_PIN,
        .rst_port     = GC9A01_DEFAULT_RST_PORT,
        .rst_pin      = GC9A01_DEFAULT_RST_PIN,
        .dc_port      = GC9A01_DEFAULT_DC_PORT,
        .dc_pin       = GC9A01_DEFAULT_DC_PIN,
    };
    return GC9A01_PortInitWithConfig(&g_lcd, &def_cfg);
}
