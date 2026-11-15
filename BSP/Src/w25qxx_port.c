/**
 * @file    w25qxx_port.c
 * @brief   W25QXX 移植层实现（STM32F1 + LL 库，支持配置注入与多实例）
 * @details 负责片选引脚控制、SPI 总线绑定与读写回调，填充 W25QXX_Bus 回调表。
 *          支持通过 W25QXX_PortInitWithConfig 注入任意 SPI 总线与片选引脚；
 *          W25QXX_PortInit 提供默认 SPI1 兼容封装。
 */
#include "w25qxx_port.h"
#include "bsp_spi.h"
#include "main.h"
#include "delay.h"
#include <string.h>

/* ================================================================================== */
/* 默认板级硬件配置（SPI1 接线）                                                      */
/* ================================================================================== */

#define W25QXX_DEFAULT_CS_PORT      GPIOA
#define W25QXX_DEFAULT_CS_PIN       LL_GPIO_PIN_8

#define W25QXX_DEFAULT_SPI_CLOCK_HZ 9000000U
#define W25QXX_DEFAULT_SPI_MODE     SPI_BUS_MODE_0
#define W25QXX_DEFAULT_SPI_USE_DMA  1

#define W25QXX_BULK_CHUNK           64U
#define W25QXX_MAX_INSTANCES        2U

#if defined(CONFIG_DUAL_SPI) && (CONFIG_DUAL_SPI != 0)
#define W25QXX_DEFAULT_BUS_ID       BSP_SPI_BUS_2
#else
#define W25QXX_DEFAULT_BUS_ID       BSP_SPI_BUS_1
#endif

/* ================================================================================== */
/* 内部上下文结构定义                                                                 */
/* ================================================================================== */

typedef struct {
    W25QXX_Device    *owner_dev;
    spi_bus_t        *bus;
    spi_bus_config_t  cfg;
    GPIO_TypeDef     *cs_port;
    uint32_t          cs_pin;
    W25QXX_Bus        bus_ops;
    uint8_t           s_ff[W25QXX_BULK_CHUNK];
    uint8_t           in_use;
} W25QXX_PortContext;

static W25QXX_PortContext s_flash_contexts[W25QXX_MAX_INSTANCES];

/* 全局默认设备句柄 */
W25QXX_Device g_flash;

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
/* W25QXX_Bus 回调实现（通过 ctx 上下文动态访问绑定的硬件与总线）                      */
/* ================================================================================== */

static int W25QXX_Port_CS_Low(void *ctx)
{
    W25QXX_PortContext *p = (W25QXX_PortContext *)ctx;
    if (p == NULL || p->bus == NULL) {
        return -1;
    }

    if (spi_bus_acquire(p->bus, p, &p->cfg) != 0) {
        return -1;
    }
    LL_GPIO_ResetOutputPin(p->cs_port, p->cs_pin);
    return 0;
}

static int W25QXX_Port_CS_High(void *ctx)
{
    W25QXX_PortContext *p = (W25QXX_PortContext *)ctx;
    if (p == NULL || p->bus == NULL) {
        return -1;
    }

    int rel = spi_bus_release(p->bus, p);
    if (rel > 0) {
        LL_GPIO_SetOutputPin(p->cs_port, p->cs_pin);
        Delay_us(10); /* 保证 Flash tSHSL 片选释放恢复时间与写操作锁存就绪 */
        return 0;
    }
    return (rel == 0) ? 0 : -1;
}

static int W25QXX_Port_Swap(void *ctx, uint8_t tx, uint8_t *rx)
{
    W25QXX_PortContext *p = (W25QXX_PortContext *)ctx;
    uint8_t dummy = 0xFFU;
    uint8_t *rx_buf = (rx != NULL) ? rx : &dummy;

    if (p == NULL || p->bus == NULL || spi_bus_owner(p->bus) != p) {
        return -1;
    }
    if (spi_bus_transfer(p->bus, p, &tx, rx_buf, 1U) != 0) {
        return -1;
    }
    return 0;
}

static uint32_t W25QXX_Port_Millis(void *ctx)
{
    (void)ctx;
    return GetTick_ms();
}

static int W25QXX_Port_ReadBulk(void *ctx, uint8_t *rx, uint32_t len)
{
    W25QXX_PortContext *p = (W25QXX_PortContext *)ctx;
    uint32_t done = 0;

    if (rx == NULL || p == NULL || p->bus == NULL || spi_bus_owner(p->bus) != p) {
        return -1;
    }

    while (done < len) {
        uint32_t n = len - done;
        if (n > W25QXX_BULK_CHUNK) {
            n = W25QXX_BULK_CHUNK;
        }
        if (spi_bus_transfer(p->bus, p, p->s_ff, rx + done, (uint16_t)n) != 0) {
            return -1;
        }
        done += n;
    }
    return 0;
}

static int W25QXX_Port_WriteBulk(void *ctx, const uint8_t *tx, uint32_t len)
{
    W25QXX_PortContext *p = (W25QXX_PortContext *)ctx;

    if (tx == NULL || p == NULL || p->bus == NULL || spi_bus_owner(p->bus) != p) {
        return -1;
    }
    return spi_bus_transfer(p->bus, p, tx, NULL, len);
}

/* ================================================================================== */
/* 硬件引脚初始化                                                                     */
/* ================================================================================== */

static void W25QXX_CS_Init(const W25QXX_PortContext *ctx)
{
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    gpio_clock_enable(ctx->cs_port);

    GPIO_InitStruct.Pin        = ctx->cs_pin;
    GPIO_InitStruct.Mode       = LL_GPIO_MODE_OUTPUT;
    GPIO_InitStruct.Speed      = LL_GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    LL_GPIO_Init(ctx->cs_port, &GPIO_InitStruct);

    /* 默认不选中 */
    LL_GPIO_SetOutputPin(ctx->cs_port, ctx->cs_pin);
}

/* ================================================================================== */
/* 对外移植与配置注入接口实现                                                         */
/* ================================================================================== */

W25QXX_Result W25QXX_PortInitWithConfig(W25QXX_Device *dev, const W25QXX_PortConfig *config)
{
    if (dev == NULL || config == NULL) {
        return W25QXX_ERROR_PARAM;
    }

    /* 1. 查找是否已为该 dev 分配过槽位（重复初始化已有设备，复用已有槽位） */
    int assigned_slot = -1;
    for (uint32_t i = 0; i < W25QXX_MAX_INSTANCES; i++) {
        if (s_flash_contexts[i].in_use && s_flash_contexts[i].owner_dev == dev) {
            assigned_slot = (int)i;
            break;
        }
    }

    /* 2. 若未分配，寻找空闲槽位；g_flash 优先分配 slot 0 */
    if (assigned_slot < 0) {
        if (dev == &g_flash && !s_flash_contexts[0].in_use) {
            assigned_slot = 0;
        } else {
            for (uint32_t i = (dev == &g_flash ? 0U : 1U); i < W25QXX_MAX_INSTANCES; i++) {
                if (!s_flash_contexts[i].in_use) {
                    assigned_slot = (int)i;
                    break;
                }
            }
        }
    }

    /* 静态槽位用尽时直接返回错误，禁止覆盖已分配上下文 */
    if (assigned_slot < 0) {
        return W25QXX_ERROR_PARAM;
    }

    W25QXX_PortContext *ctx = &s_flash_contexts[assigned_slot];

    /* 若重复初始化且原先仍持有总线锁，先安全释放清理 */
    if (ctx->in_use && ctx->bus != NULL && ctx->bus->owner == ctx) {
        while (ctx->bus->depth > 0) {
            (void)spi_bus_release(ctx->bus, ctx);
        }
    }

    /* 绑定总线（若未指定则按编译宏绑定默认总线）与参数配置 */
    ctx->bus = (config->bus != NULL) ? config->bus : BSP_SPI_GetBus(W25QXX_DEFAULT_BUS_ID);
    if (ctx->bus == NULL) {
        return W25QXX_ERROR_BUS;
    }

    ctx->owner_dev = dev;
    ctx->in_use    = 1U;
    ctx->cfg.clock_hz = (config->spi_clock_hz > 0U) ? config->spi_clock_hz : W25QXX_DEFAULT_SPI_CLOCK_HZ;
    ctx->cfg.mode     = config->spi_mode;
    ctx->cfg.use_dma  = config->use_dma;
    ctx->cs_port      = (config->cs_port != NULL) ? config->cs_port : W25QXX_DEFAULT_CS_PORT;
    ctx->cs_pin       = (config->cs_pin != 0U) ? config->cs_pin : W25QXX_DEFAULT_CS_PIN;

    memset(ctx->s_ff, 0xFF, sizeof(ctx->s_ff));

    ctx->bus_ops.cs_low     = W25QXX_Port_CS_Low;
    ctx->bus_ops.cs_high    = W25QXX_Port_CS_High;
    ctx->bus_ops.swap       = W25QXX_Port_Swap;
    ctx->bus_ops.millis     = W25QXX_Port_Millis;
    ctx->bus_ops.ctx        = ctx;
    ctx->bus_ops.read_bulk  = W25QXX_Port_ReadBulk;
    ctx->bus_ops.write_bulk = W25QXX_Port_WriteBulk;

    W25QXX_CS_Init(ctx);

    return W25QXX_Init(dev, &ctx->bus_ops);
}

void W25QXX_PortInit(void)
{
    static const W25QXX_PortConfig def_cfg = {
        .bus          = NULL, /* 自动根据 CONFIG_DUAL_SPI 绑定 SPI1 或 SPI2 */
        .spi_clock_hz = W25QXX_DEFAULT_SPI_CLOCK_HZ,
        .spi_mode     = W25QXX_DEFAULT_SPI_MODE,
        .use_dma      = W25QXX_DEFAULT_SPI_USE_DMA,
        .cs_port      = W25QXX_DEFAULT_CS_PORT,
        .cs_pin       = W25QXX_DEFAULT_CS_PIN,
    };
    (void)W25QXX_PortInitWithConfig(&g_flash, &def_cfg);
}

void W25QXX_Port_ResetSlots(void)
{
    memset(s_flash_contexts, 0, sizeof(s_flash_contexts));
}
