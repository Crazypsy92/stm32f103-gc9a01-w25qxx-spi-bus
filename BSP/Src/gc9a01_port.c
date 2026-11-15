/**
 * @file    gc9a01_port.c
 * @brief   GC9A01 移植层实现（STM32F1 + LL 库）
 * @details 本文件只负责板级资源：CS/RST/DC 引脚、SPI1 总线绑定与延时实现，
 *          并填充 gc9a01.h 中的 LCD_Port 回调表。
 *          SPI1 外设本身由 bsp_spi.c 统一管理，与 W25QXX 共享，不再重复初始化。
 *          Stage 2.1: 规范化 Port_Cs 与 Port_Write 错误返回值，向协议层正确上报总线与片选状态。
 */
#include "gc9a01_port.h"
#include "bsp_spi.h"
#include "main.h"
#include "delay.h"

/* ================================================================================== */
/* 板级配置（已核实硬件接线）                                                         */
/* ================================================================================== */

/* 接线：SCK=PA5  MOSI=PA7  CS=PA9  RES=PA10  DC=PB13  背光未接控制引脚（常亮） */
#define GC9A01_CS_PORT      GPIOA
#define GC9A01_CS_PIN       LL_GPIO_PIN_9

#define GC9A01_RST_PORT     GPIOA
#define GC9A01_RST_PIN      LL_GPIO_PIN_10

#define GC9A01_DC_PORT      GPIOB
#define GC9A01_DC_PIN       LL_GPIO_PIN_13

/* 设备级总线参数：18MHz（72MHz/4），模式 0；大块刷屏走 DMA */
#define GC9A01_SPI_CLOCK_HZ 18000000U
#define GC9A01_SPI_MODE     SPI_BUS_MODE_0
#define GC9A01_SPI_USE_DMA  1

/* ================================================================================== */
/* 内部变量                                                                           */
/* ================================================================================== */

/** @brief 移植层上下文：共享总线 + 本设备的速率/模式 */
typedef struct {
    spi_bus_t        *bus;
    spi_bus_config_t  cfg;
} GC9A01_HW;

static GC9A01_HW s_hw;

/* 全局设备句柄，供应用层直接使用 */
LCD_Device g_lcd;

/* ================================================================================== */
/* LCD_Port 回调实现（STM32 相关代码全部集中在这里）                                  */
/* ================================================================================== */

static LCD_Result Port_Cs(void *ctx, uint8_t level)
{
    GC9A01_HW *hw = (GC9A01_HW *)ctx;
    if (hw == NULL || hw->bus == NULL) {
        return LCD_ERROR_PARAM;
    }

    if (level == 0U) {
        /* 占用总线并切到本设备参数；被其它设备占用时拒绝抢占，保持 CS 高避免误伤 */
        if (spi_bus_acquire(hw->bus, hw, &hw->cfg) != 0) {
            return LCD_ERROR_BUS;
        }
        LL_GPIO_ResetOutputPin(GC9A01_CS_PORT, GC9A01_CS_PIN);
        return LCD_OK;
    } else {
        /* 仅当本次 release 真正交还总线（depth 归零）才拉高 CS；
           嵌套事务未结束时 CS 必须保持有效，避免提前结束当前命令 */
        int rel = spi_bus_release(hw->bus, hw);
        if (rel > 0) {
            LL_GPIO_SetOutputPin(GC9A01_CS_PORT, GC9A01_CS_PIN);
            return LCD_OK;
        } else if (rel == 0) {
            return LCD_OK; /* 嵌套事务保持 CS 有效 */
        } else {
            return LCD_ERROR_BUS; /* 释放失败（状态损坏或非法调用） */
        }
    }
}

static void Port_Rst(void *ctx, uint8_t level)
{
    (void)ctx;
    if (level) {
        LL_GPIO_SetOutputPin(GC9A01_RST_PORT, GC9A01_RST_PIN);
    } else {
        LL_GPIO_ResetOutputPin(GC9A01_RST_PORT, GC9A01_RST_PIN);
    }
}

static void Port_Dc(void *ctx, uint8_t level)
{
    (void)ctx;
    if (level) {
        LL_GPIO_SetOutputPin(GC9A01_DC_PORT, GC9A01_DC_PIN);
    } else {
        LL_GPIO_ResetOutputPin(GC9A01_DC_PORT, GC9A01_DC_PIN);
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

    /* 只有本设备仍持有总线时才发送，防止写进别人的事务 */
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
/* 硬件初始化                                                                         */
/* ================================================================================== */

static void GC9A01_GPIO_Init(void)
{
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (GC9A01_CS_PORT == GPIOA) {
        LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOA);
    } else if (GC9A01_CS_PORT == GPIOB) {
        LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOB);
    } else if (GC9A01_CS_PORT == GPIOC) {
        LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOC);
    }
    if (GC9A01_DC_PORT == GPIOA) {
        LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOA);
    } else if (GC9A01_DC_PORT == GPIOB) {
        LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOB);
    } else if (GC9A01_DC_PORT == GPIOC) {
        LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOC);
    }

    GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
    GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;

    GPIO_InitStruct.Pin = GC9A01_CS_PIN;
    LL_GPIO_Init(GC9A01_CS_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GC9A01_RST_PIN;
    LL_GPIO_Init(GC9A01_RST_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GC9A01_DC_PIN;
    LL_GPIO_Init(GC9A01_DC_PORT, &GPIO_InitStruct);

    /* 默认电平：片选/复位/DC 拉高 */
    LL_GPIO_SetOutputPin(GC9A01_CS_PORT, GC9A01_CS_PIN);
    LL_GPIO_SetOutputPin(GC9A01_RST_PORT, GC9A01_RST_PIN);
    LL_GPIO_SetOutputPin(GC9A01_DC_PORT, GC9A01_DC_PIN);
}

/* ================================================================================== */
/* 移植接口实现                                                                       */
/* ================================================================================== */

/* 抽象端口：驱动只通过这张表访问硬件，背光未接控制引脚故 bl 为 NULL */
static const LCD_Port s_lcd_port = {
    .cs            = Port_Cs,
    .rst           = Port_Rst,
    .dc            = Port_Dc,
    .bl            = NULL,
    .delay_ms      = Port_DelayMs,
    .write         = Port_Write,
    .max_chunk_len = 65535U,
    .ctx           = &s_hw,
};

void GC9A01_PortInit(void)
{
    /* 1. 绑定共享的 SPI1 总线与本设备参数 */
    s_hw.bus = BSP_SPI1_GetBus();
    s_hw.cfg.clock_hz = GC9A01_SPI_CLOCK_HZ;
    s_hw.cfg.mode     = GC9A01_SPI_MODE;
    s_hw.cfg.use_dma  = GC9A01_SPI_USE_DMA;

    /* 2. 控制引脚 */
    GC9A01_GPIO_Init();

    /* 3. 绑定端口并完成面板初始化 */
    (void)GC9A01_Init(&g_lcd, &s_lcd_port);
}
