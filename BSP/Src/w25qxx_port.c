/**
 * @file    w25qxx_port.c
 * @brief   W25QXX 移植层（STM32F1 + LL 库）
 * @details 唯一与 MCU 相关的文件：负责片选引脚初始化，并把共享 SPI1 总线
 *          以 W25QXX_Bus 回调表的形式交给驱动。
 *          SPI 外设本身由 bsp_spi.c 统一管理，与 GC9A01 共享。
 */
#include "w25qxx_port.h"
#include "bsp_spi.h"
#include "main.h"
#include "delay.h"
#include <string.h>

/* ================================================================================== */
/* 板级配置（按实际硬件修改）                                         */
/* ================================================================================== */

/* W25QXX 接线：CS=PA8  SCK=PA5  MISO=PA6  MOSI=PA7（SPI1 默认引脚） */
#define W25QXX_CS_PORT      GPIOA
#define W25QXX_CS_PIN       LL_GPIO_PIN_8

/* 设备级总线参数：9MHz（72MHz/8），模式 0（W25Qxx 支持模式 0/3）
   数据段交给批量回调，长度超过总线阈值（16B）时走 DMA1_Ch2/Ch3 */
#define W25QXX_SPI_CLOCK_HZ 9000000U
#define W25QXX_SPI_MODE     SPI_BUS_MODE_0
#define W25QXX_SPI_USE_DMA  1

/* 读方向需要同时发 0xFF 才能产生时钟，分块大小即该哑元缓冲长度 */
#define W25QXX_BULK_CHUNK   64U

/* ================================================================================== */
/* 内部变量                                                           */
/* ================================================================================== */

/** @brief 总线回调的上下文：共享总线 + 本设备参数 + 片选引脚 */
typedef struct {
    spi_bus_t        *bus;
    spi_bus_config_t  cfg;
    GPIO_TypeDef     *cs_port;
    uint32_t          cs_pin;
} W25QXX_PortCtx;

static W25QXX_PortCtx s_ctx = {
    .bus     = NULL,
    .cfg     = { W25QXX_SPI_CLOCK_HZ, W25QXX_SPI_MODE, W25QXX_SPI_USE_DMA },
    .cs_port = W25QXX_CS_PORT,
    .cs_pin  = W25QXX_CS_PIN,
};

/* 读方向哑元缓冲：SPI 是全双工，必须持续送 0xFF 才能产生时钟 */
static uint8_t s_ff[W25QXX_BULK_CHUNK];

/* ================================================================================== */
/* 总线回调实现（驱动通过这些函数访问硬件）                            */
/* ================================================================================== */

static void W25QXX_Port_CS_Low(void *ctx)
{
    W25QXX_PortCtx *p = (W25QXX_PortCtx *)ctx;

    /* 占用总线并切到本设备参数；被其它设备占用时保持 CS 高 */
    if (spi_bus_acquire(p->bus, p, &p->cfg) != 0) {
        return;
    }
    LL_GPIO_ResetOutputPin(p->cs_port, p->cs_pin);
}

static void W25QXX_Port_CS_High(void *ctx)
{
    W25QXX_PortCtx *p = (W25QXX_PortCtx *)ctx;

    /* 仅当本次 release 真正交还总线才拉高 CS；嵌套期间 CS 保持有效 */
    if (spi_bus_release(p->bus, p) > 0) {
        LL_GPIO_SetOutputPin(p->cs_port, p->cs_pin);
        Delay_us(10); /* 保证 Flash tSHSL 片选释放恢复时间与写操作锁存就绪 */
    }
}

static uint8_t W25QXX_Port_Swap(void *ctx, uint8_t tx)
{
    W25QXX_PortCtx *p = (W25QXX_PortCtx *)ctx;
    uint8_t rx = 0xFFU;

    /* 只有本设备仍持有总线时才收发 */
    if (spi_bus_owner(p->bus) == p) {
        (void)spi_bus_transfer(p->bus, p, &tx, &rx, 1U);
    }
    return rx;
}

static uint32_t W25QXX_Port_Millis(void *ctx)
{
    (void)ctx;
    return GetTick_ms();
}

/* 读数据段：发送 0xFF 哑元同时接收，长度超过阈值时由总线切到 DMA */
static int W25QXX_Port_ReadBulk(void *ctx, uint8_t *rx, uint16_t len)
{
    W25QXX_PortCtx *p = (W25QXX_PortCtx *)ctx;
    uint16_t done = 0;

    if (rx == NULL || spi_bus_owner(p->bus) != p) {
        return -1;
    }

    while (done < len) {
        uint16_t n = (uint16_t)(len - done);
        if (n > W25QXX_BULK_CHUNK) {
            n = W25QXX_BULK_CHUNK;
        }
        if (spi_bus_transfer(p->bus, p, s_ff, rx + done, n) != 0) {
            return -1;
        }
        done += n;
    }
    return 0;
}

/* 写数据段：只发不收 */
static int W25QXX_Port_WriteBulk(void *ctx, const uint8_t *tx, uint16_t len)
{
    W25QXX_PortCtx *p = (W25QXX_PortCtx *)ctx;

    if (tx == NULL || spi_bus_owner(p->bus) != p) {
        return -1;
    }
    return spi_bus_transfer(p->bus, p, tx, NULL, len);
}

/* 交给驱动的抽象总线（生命周期覆盖整个运行期） */
static const W25QXX_Bus s_w25qxx_bus = {
    W25QXX_Port_CS_Low,
    W25QXX_Port_CS_High,
    W25QXX_Port_Swap,
    W25QXX_Port_Millis,
    &s_ctx,
    W25QXX_Port_ReadBulk,
    W25QXX_Port_WriteBulk
};

/* ================================================================================== */
/* 片选引脚初始化                                                     */
/* ================================================================================== */

static void W25QXX_CS_Init(void)
{
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOA);

    GPIO_InitStruct.Pin        = W25QXX_CS_PIN;
    GPIO_InitStruct.Mode       = LL_GPIO_MODE_OUTPUT;
    GPIO_InitStruct.Speed      = LL_GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    LL_GPIO_Init(W25QXX_CS_PORT, &GPIO_InitStruct);

    /* 默认不选中 */
    LL_GPIO_SetOutputPin(W25QXX_CS_PORT, W25QXX_CS_PIN);
}

/* ================================================================================== */
/* 移植接口实现                                                       */
/* ================================================================================== */

W25QXX_Device g_flash;

void W25QXX_PortInit(void)
{
    /* 1. 取得共享的 SPI1 总线（GC9A01 若已初始化则直接复用） */
    s_ctx.bus = BSP_SPI1_GetBus();

    /* 2. 片选引脚 + 读哑元缓冲 */
    W25QXX_CS_Init();
    memset(s_ff, 0xFF, sizeof(s_ff));

    /* 3. 绑定总线并读 ID */
    (void)W25QXX_Init(&g_flash, &s_w25qxx_bus);
}
