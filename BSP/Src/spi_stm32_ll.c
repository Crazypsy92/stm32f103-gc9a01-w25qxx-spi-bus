/**
 * @file    spi_stm32_ll.c
 * @brief   SPI 总线的 STM32F1 LL 实现
 * @details 负责 GPIO 复用、SPI 外设时钟/参数配置与阻塞式全双工传输。
 *          这是本工程中唯一产生 SPI 时序的文件；上层只看到 spi_bus.h。
 *
 *          错误处理统一用 spi_ll_status_t：
 *          - 参数非法        SPI_LL_ERROR_PARAM
 *          - 等待超时        SPI_LL_ERROR_TIMEOUT
 *          - DMA 传输错误    SPI_LL_ERROR_DMA（TEIF）
 *          - 外设忙无法配置  SPI_LL_ERROR_BUSY
 *          - 恢复失败        SPI_LL_ERROR_RECOVERY
 *          所有错误路径都会调用 spi_recover() 把 DMA/SPI 拉回可用态，
 *          保证下一次事务仍可正常执行。
 */
#include "spi_stm32_ll.h"
#include <string.h>

/* 超时预算（单位：CPU 周期，72 MHz）。基于实际传输时间 + 余量，非随意循环次数。 */
#define SPI_BYTE_TIMEOUT_CYCLES   500000U   /* 单字节 TXE/RXNE：约 7 ms 余量充足 */
#define SPI_BSY_TIMEOUT_CYCLES    500000U   /* BSY 清零等待：约 7 ms */
#define SPI_RECOVER_BSY_CYCLES    1000000U  /* 恢复阶段等待 BSY 的最长时间：约 14 ms */
#define SPI_DMA_MARGIN_CYCLES     1000000U  /* DMA 超时的固定余量：约 14 ms */

#define SPI_DMA_MIN_LEN           16U       /* 小于该长度走轮询，DMA 启动开销更大 */
#define SPI_DMA_MAX_CHUNK         65535U    /* CNDTR 为 16 位，单次 DMA 上限 */

/* 调试计数（HIL 验收用） */
volatile uint32_t g_ll_recover_count = 0U;
volatile int32_t  g_ll_last_error    = 0;

static inline void spi_drain_rx_and_clear_ovr(SPI_TypeDef *spi)
{
    while (LL_SPI_IsActiveFlag_RXNE(spi)) {
        (void)LL_SPI_ReceiveData8(spi);
    }
    (void)LL_SPI_ReceiveData8(spi);
    LL_SPI_ClearFlag_OVR(spi);
}

/* ================================================================================== */
/* 时钟使能                                                           */
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

static void spi_clock_enable(SPI_TypeDef *periph)
{
    if (periph == SPI1) {
        LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SPI1);
    } else if (periph == SPI2) {
        LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_SPI2);
    }
}

static void dma_clock_enable(DMA_TypeDef *dma)
{
    /* STM32F103C8 只有 DMA1 */
    if (dma == DMA1) {
        LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
    }
}

/* ================================================================================== */
/* DWT 计时辅助（基于实际时间，处理计数器 32 位回绕）                                 */
/* ================================================================================== */

static inline uint32_t ll_dwt_cycles(void)
{
    return DWT->CYCCNT;
}

/** @brief 距 start 是否已经超过 timeout 个周期（无符号差值自动处理回绕） */
static inline int ll_timeout_expired(uint32_t start, uint32_t timeout)
{
    return ((ll_dwt_cycles() - start) > timeout) ? 1 : 0;
}

/* ================================================================================== */
/* 参数换算                                                           */
/* ================================================================================== */

/** @brief 按目标频率选择不大于目标值的最近分频（取整到 2 的幂） */
static uint32_t spi_select_prescaler(uint32_t pclk_hz, uint32_t target_hz)
{
    uint32_t div;

    if (target_hz == 0U || pclk_hz == 0U) {
        return LL_SPI_BAUDRATEPRESCALER_DIV8;
    }

    for (div = 2U; div < 256U; div <<= 1) {
        if ((pclk_hz / div) <= target_hz) {
            break;
        }
    }
    if (div > 256U) {
        div = 256U;
    }

    switch (div) {
        case 2U:   return LL_SPI_BAUDRATEPRESCALER_DIV2;
        case 4U:   return LL_SPI_BAUDRATEPRESCALER_DIV4;
        case 8U:   return LL_SPI_BAUDRATEPRESCALER_DIV8;
        case 16U:  return LL_SPI_BAUDRATEPRESCALER_DIV16;
        case 32U:  return LL_SPI_BAUDRATEPRESCALER_DIV32;
        case 64U:  return LL_SPI_BAUDRATEPRESCALER_DIV64;
        case 128U: return LL_SPI_BAUDRATEPRESCALER_DIV128;
        default:   return LL_SPI_BAUDRATEPRESCALER_DIV256;
    }
}

/* ================================================================================== */
/* 抽象总线操作实现                                                     */
/* ================================================================================== */

static int ll_configure(spi_bus_t *bus, const spi_bus_config_t *cfg)
{
    spi_stm32_ll_t *ll = (spi_stm32_ll_t *)bus->ctx;
    SPI_TypeDef *spi = ll->hw->periph;
    uint8_t need_spi_init;

    if (cfg == NULL || ll->hw == NULL) {
        return (int)SPI_LL_ERROR_PARAM;
    }
    /* 非法时钟：0 或超过外设时钟 */
    if (cfg->clock_hz == 0U || cfg->clock_hz > ll->hw->pclk_hz) {
        return (int)SPI_LL_ERROR_PARAM;
    }
    /* 非法模式 */
    if (cfg->mode > SPI_BUS_MODE_3) {
        return (int)SPI_LL_ERROR_PARAM;
    }
    /* 外设正忙或 DMA 通道仍在运行时禁止改配置 */
    if (LL_SPI_IsActiveFlag_BSY(spi)) {
        return (int)SPI_LL_ERROR_BUSY;
    }
    if (ll->hw->dma != NULL) {
        if (LL_DMA_IsEnabledChannel(ll->hw->dma, ll->hw->dma_tx_ch) ||
            LL_DMA_IsEnabledChannel(ll->hw->dma, ll->hw->dma_rx_ch)) {
            return (int)SPI_LL_ERROR_BUSY;
        }
    }

    /* SPI 寄存器只依赖 clock_hz 与 mode；use_dma 仅影响传输路径，无需改外设 */
    need_spi_init = (uint8_t)(!ll->configured ||
                              ll->current.clock_hz != cfg->clock_hz ||
                              ll->current.mode     != cfg->mode);

    if (need_spi_init) {
        LL_SPI_Disable(spi);

        LL_SPI_SetBaudRatePrescaler(spi, spi_select_prescaler(ll->hw->pclk_hz, cfg->clock_hz));

        switch (cfg->mode) {
            case SPI_BUS_MODE_1:
                LL_SPI_SetClockPolarity(spi, LL_SPI_POLARITY_LOW);
                LL_SPI_SetClockPhase(spi, LL_SPI_PHASE_2EDGE);
                break;
            case SPI_BUS_MODE_2:
                LL_SPI_SetClockPolarity(spi, LL_SPI_POLARITY_HIGH);
                LL_SPI_SetClockPhase(spi, LL_SPI_PHASE_1EDGE);
                break;
            case SPI_BUS_MODE_3:
                LL_SPI_SetClockPolarity(spi, LL_SPI_POLARITY_HIGH);
                LL_SPI_SetClockPhase(spi, LL_SPI_PHASE_2EDGE);
                break;
            case SPI_BUS_MODE_0:
            default:
                LL_SPI_SetClockPolarity(spi, LL_SPI_POLARITY_LOW);
                LL_SPI_SetClockPhase(spi, LL_SPI_PHASE_1EDGE);
                break;
        }

        LL_SPI_Enable(spi);
        spi_drain_rx_and_clear_ovr(spi);
    }

    /* use_dma 变化必须落到 current，无论是否重配外设 */
    ll->current = *cfg;
    ll->configured = 1U;
    return (int)SPI_LL_OK;
}

/* ================================================================================== */
/* DMA 辅助                                                           */
/* ================================================================================== */

/** @brief 清指定通道的全部标志（GIF/TCIF/HTIF/TEIF，按通道号左移 4 位） */
static void dma_clear_flags(DMA_TypeDef *dma, uint32_t ch)
{
    uint32_t shift = (ch - 1U) * 4U;

    dma->IFCR = (DMA_IFCR_CGIF1 | DMA_IFCR_CTCIF1 | DMA_IFCR_CHTIF1 | DMA_IFCR_CTEIF1) << shift;
}

/**
 * @brief 等待指定通道完成：同时检查 TCIF（完成）与 TEIF（错误）
 * @return SPI_LL_OK / SPI_LL_ERROR_TIMEOUT / SPI_LL_ERROR_DMA
 */
static spi_ll_status_t dma_wait(DMA_TypeDef *dma, uint32_t ch, uint32_t timeout_cycles)
{
    uint32_t shift = (ch - 1U) * 4U;
    uint32_t tc_mask = DMA_ISR_TCIF1 << shift;
    uint32_t te_mask = DMA_ISR_TEIF1 << shift;
    uint32_t start = ll_dwt_cycles();

    while (1) {
        uint32_t isr = dma->ISR;
        if ((isr & te_mask) != 0U) {
            return SPI_LL_ERROR_DMA;       /* DMA 传输错误，立即终止 */
        }
        if ((isr & tc_mask) != 0U) {
            return SPI_LL_OK;
        }
        if (ll_timeout_expired(start, timeout_cycles)) {
            return SPI_LL_ERROR_TIMEOUT;
        }
    }
}

/** @brief 配置一个 DMA 通道（字节宽度、单次模式、内存自增） */
static void dma_setup_channel(DMA_TypeDef *dma, uint32_t ch,
                              const uint8_t *mem, uint32_t periph_addr,
                              uint32_t direction, uint32_t len)
{
    LL_DMA_DisableChannel(dma, ch);
    dma_clear_flags(dma, ch);

    LL_DMA_SetDataTransferDirection(dma, ch, direction);
    LL_DMA_SetMode(dma, ch, LL_DMA_MODE_NORMAL);
    LL_DMA_SetPeriphIncMode(dma, ch, LL_DMA_PERIPH_NOINCREMENT);
    LL_DMA_SetMemoryIncMode(dma, ch, LL_DMA_MEMORY_INCREMENT);
    LL_DMA_SetPeriphSize(dma, ch, LL_DMA_PDATAALIGN_BYTE);
    LL_DMA_SetMemorySize(dma, ch, LL_DMA_MDATAALIGN_BYTE);
    LL_DMA_SetChannelPriorityLevel(dma, ch, LL_DMA_PRIORITY_HIGH);

    if (direction == LL_DMA_DIRECTION_MEMORY_TO_PERIPH) {
        LL_DMA_ConfigAddresses(dma, ch, (uint32_t)mem, periph_addr, direction);
    } else {
        LL_DMA_ConfigAddresses(dma, ch, periph_addr, (uint32_t)mem, direction);
    }
    LL_DMA_SetDataLength(dma, ch, len);
}

/* ================================================================================== */
/* 统一恢复                                                           */
/* ================================================================================== */

/**
 * @brief 把 SPI/DMA 拉回可用态：关闭 DMA 请求、禁用通道、清标志、
 *        读 DR 清 OVR、等待 BSY 清零；必要时重新初始化 SPI。
 * @note  不触碰任何 CS 引脚——片选由 port 层按 depth 管理。
 */
static spi_ll_status_t spi_recover(spi_stm32_ll_t *ll)
{
    SPI_TypeDef *spi = ll->hw->periph;
    DMA_TypeDef *dma = ll->hw->dma;
    uint32_t start;

    g_ll_recover_count++;

    /* 1. 关 SPI 的 DMA 请求 */
    LL_SPI_DisableDMAReq_TX(spi);
    LL_SPI_DisableDMAReq_RX(spi);

    /* 2. 禁用 DMA 通道并清标志 */
    if (dma != NULL) {
        LL_DMA_DisableChannel(dma, ll->hw->dma_tx_ch);
        LL_DMA_DisableChannel(dma, ll->hw->dma_rx_ch);
        dma_clear_flags(dma, ll->hw->dma_tx_ch);
        dma_clear_flags(dma, ll->hw->dma_rx_ch);
    }

    /* 3. 彻底排空 RX 缓冲区并清 OVR */
    spi_drain_rx_and_clear_ovr(spi);

    /* 4. 等 BSY 清零；超时则重新初始化 SPI 外设 */
    start = ll_dwt_cycles();
    while (LL_SPI_IsActiveFlag_BSY(spi)) {
        if (ll_timeout_expired(start, SPI_RECOVER_BSY_CYCLES)) {
            LL_SPI_Disable(spi);
            LL_SPI_Enable(spi);
            spi_drain_rx_and_clear_ovr(spi);
            break;
        }
    }

    /* 5. 强制按当前配置重配一次，确保寄存器处于已知状态 */
    {
        spi_bus_config_t saved = ll->current;
        ll->configured = 0U;                 /* 强制走完整初始化分支 */
        if (ll_configure(&ll->bus, &saved) != (int)SPI_LL_OK) {
            return SPI_LL_ERROR_RECOVERY;
        }
    }

    return SPI_LL_OK;
}

/* ================================================================================== */
/* 传输实现                                                           */
/* ================================================================================== */

/**
 * @brief DMA 传输（阻塞），自动处理 >65535 的分块
 * @note  分块期间 CS 由 port 层保持（不 release、不 configure）。
 */
static spi_ll_status_t ll_transfer_dma(spi_stm32_ll_t *ll,
                                       const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    SPI_TypeDef *spi = ll->hw->periph;
    DMA_TypeDef *dma = ll->hw->dma;
    uint32_t txc = ll->hw->dma_tx_ch;
    uint32_t rxc = ll->hw->dma_rx_ch;
    uint32_t offset = 0U;
    spi_ll_status_t status = SPI_LL_OK;

    while (offset < len) {
        uint32_t chunk = len - offset;
        uint32_t timeout;
        if (chunk > SPI_DMA_MAX_CHUNK) {
            chunk = SPI_DMA_MAX_CHUNK;
        }

        /* 清掉上一轮残留（RXNE / OVR），避免首字节错位 */
        spi_drain_rx_and_clear_ovr(spi);

        /* 先 RX 后 TX，保证时钟到来前接收端就绪 */
        if (rx != NULL) {
            dma_setup_channel(dma, rxc, rx + offset, LL_SPI_DMA_GetRegAddr(spi),
                              LL_DMA_DIRECTION_PERIPH_TO_MEMORY, chunk);
        }
        dma_setup_channel(dma, txc, tx + offset, LL_SPI_DMA_GetRegAddr(spi),
                          LL_DMA_DIRECTION_MEMORY_TO_PERIPH, chunk);

        if (rx != NULL) {
            LL_SPI_EnableDMAReq_RX(spi);
            LL_DMA_EnableChannel(dma, rxc);
        }
        LL_SPI_EnableDMAReq_TX(spi);
        LL_DMA_EnableChannel(dma, txc);

        /* 按实际传输字节数计算超时：chunk*8 / spi_clk * cpu_clk + 余量 */
        timeout = (chunk * 8U * (ll->hw->pclk_hz / ll->current.clock_hz)) + SPI_DMA_MARGIN_CYCLES;

        status = dma_wait(dma, txc, timeout);
        if (status == SPI_LL_OK && rx != NULL) {
            status = dma_wait(dma, rxc, timeout);
        }

        if (status != SPI_LL_OK) {
            goto cleanup;
        }

        /* CNDTR/TCIF 仅代表 DMA 已搬完数据，不代表 SPI 最后一位已离开 SCK。
           必须逐块等 TXE 和 BSY，尤其避免下一块 DMA 覆盖尚在发送的尾字节。 */
        {
            uint32_t start = ll_dwt_cycles();
            while (!LL_SPI_IsActiveFlag_TXE(spi)) {
                if (ll_timeout_expired(start, SPI_BYTE_TIMEOUT_CYCLES)) {
                    status = SPI_LL_ERROR_TIMEOUT;
                    goto cleanup;
                }
            }
            start = ll_dwt_cycles();
            while (LL_SPI_IsActiveFlag_BSY(spi)) {
                if (ll_timeout_expired(start, SPI_BSY_TIMEOUT_CYCLES)) {
                    status = SPI_LL_ERROR_TIMEOUT;
                    goto cleanup;
                }
            }
        }

        /* SPI 已空闲，此时才能关闭当前 DMA 请求和通道、进入下一块。
           设备 CS 属于 Port 层，整个分块循环期间始终保持低电平。 */
        LL_SPI_DisableDMAReq_TX(spi);
        LL_SPI_DisableDMAReq_RX(spi);
        LL_DMA_DisableChannel(dma, txc);
        LL_DMA_DisableChannel(dma, rxc);
        dma_clear_flags(dma, txc);
        dma_clear_flags(dma, rxc);
        if (rx == NULL) {
            spi_drain_rx_and_clear_ovr(spi);
        }
        offset += chunk;
    }

cleanup:
    LL_SPI_DisableDMAReq_TX(spi);
    LL_SPI_DisableDMAReq_RX(spi);
    if (dma != NULL) {
        LL_DMA_DisableChannel(dma, txc);
        LL_DMA_DisableChannel(dma, rxc);
        dma_clear_flags(dma, txc);
        dma_clear_flags(dma, rxc);
    }

    if (status != SPI_LL_OK) {
        (void)spi_recover(ll);
    }
    return status;
}

/** @brief 轮询传输：每字节 TXE/RXNE 等待均带超时，末尾等 BSY 清零 */
static spi_ll_status_t ll_transfer_poll(spi_stm32_ll_t *ll,
                                        const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    SPI_TypeDef *spi = ll->hw->periph;
    uint32_t i;

    /* 清残留 */
    spi_drain_rx_and_clear_ovr(spi);

    for (i = 0U; i < len; i++) {
        uint32_t start = ll_dwt_cycles();
        while (!LL_SPI_IsActiveFlag_TXE(spi)) {
            if (ll_timeout_expired(start, SPI_BYTE_TIMEOUT_CYCLES)) {
                (void)spi_recover(ll);
                return SPI_LL_ERROR_TIMEOUT;
            }
        }
        LL_SPI_TransmitData8(spi, tx[i]);

        start = ll_dwt_cycles();
        while (!LL_SPI_IsActiveFlag_RXNE(spi)) {
            if (ll_timeout_expired(start, SPI_BYTE_TIMEOUT_CYCLES)) {
                (void)spi_recover(ll);
                return SPI_LL_ERROR_TIMEOUT;
            }
        }
        uint8_t b = LL_SPI_ReceiveData8(spi);
        if (rx != NULL) {
            rx[i] = b;
        }
    }

    /* 最后一字节移出移位寄存器后才算结束，否则 CS 拉高会截断末位 */
    {
        uint32_t start = ll_dwt_cycles();
        while (LL_SPI_IsActiveFlag_BSY(spi)) {
            if (ll_timeout_expired(start, SPI_BSY_TIMEOUT_CYCLES)) {
                (void)spi_recover(ll);
                return SPI_LL_ERROR_TIMEOUT;
            }
        }
    }

    /* TX-only 清 OVR（全双工下 RXNE 已在循环中消费，一般无 OVR，这里保险） */
    if (rx == NULL) {
        spi_drain_rx_and_clear_ovr(spi);
    }
    return SPI_LL_OK;
}

static int ll_transfer(spi_bus_t *bus, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    spi_stm32_ll_t *ll = (spi_stm32_ll_t *)bus->ctx;
    int ret;

    if (tx == NULL || len == 0U) {
        return (int)SPI_LL_ERROR_PARAM;
    }
    if (ll->current.use_dma && ll->hw->dma != NULL && len >= SPI_DMA_MIN_LEN) {
        ret = (int)ll_transfer_dma(ll, tx, rx, len);
    } else {
        ret = (int)ll_transfer_poll(ll, tx, rx, len);
    }
    if (ret != (int)SPI_LL_OK) {
        g_ll_last_error = ret;
    }
    return ret;
}

static int ll_in_isr(spi_bus_t *bus)
{
    (void)bus;
    return (__get_IPSR() != 0U) ? 1 : 0;
}

static const spi_bus_ops_t s_stm32_ll_ops = {
    .configure = ll_configure,
    .transfer  = ll_transfer,
    .in_isr    = ll_in_isr,
};

/* ================================================================================== */
/* 对外接口                                                           */
/* ================================================================================== */

void spi_stm32_ll_init(spi_stm32_ll_t *ll, const spi_stm32_ll_hw_t *hw)
{
    SPI_TypeDef *spi;
    LL_GPIO_InitTypeDef gpio = {0};

    if (ll == NULL || hw == NULL) {
        return;
    }

    ll->hw = hw;
    ll->bus.ops = &s_stm32_ll_ops;
    ll->bus.ctx = ll;
    ll->bus.owner = NULL;
    ll->bus.depth = 0U;
    ll->bus.rejects = 0U;
    ll->configured = 0U;
    memset(&ll->current, 0, sizeof(ll->current));

    spi = hw->periph;

    gpio_clock_enable(hw->sck_port);
    gpio_clock_enable(hw->miso_port);
    gpio_clock_enable(hw->mosi_port);
    spi_clock_enable(spi);
    if (hw->dma != NULL) {
        dma_clock_enable(hw->dma);
    }

    /* SCK / MOSI：复用推挽；MISO：浮空输入 */
    gpio.Mode = LL_GPIO_MODE_ALTERNATE;
    gpio.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Pin = hw->sck_pin;
    LL_GPIO_Init(hw->sck_port, &gpio);
    gpio.Pin = hw->mosi_pin;
    LL_GPIO_Init(hw->mosi_port, &gpio);

    gpio.Mode = LL_GPIO_MODE_FLOATING;
    gpio.Pin = hw->miso_pin;
    LL_GPIO_Init(hw->miso_port, &gpio);

    /* SPI 主机、全双工、8 位、软件 NSS（片选由各设备自己管） */
    LL_SPI_Disable(spi);
    LL_SPI_SetMode(spi, LL_SPI_MODE_MASTER);
    LL_SPI_SetTransferDirection(spi, LL_SPI_FULL_DUPLEX);
    LL_SPI_SetDataWidth(spi, LL_SPI_DATAWIDTH_8BIT);
    LL_SPI_SetNSSMode(spi, LL_SPI_NSS_SOFT);
    LL_SPI_SetBaudRatePrescaler(spi, LL_SPI_BAUDRATEPRESCALER_DIV8);
    LL_SPI_SetClockPolarity(spi, LL_SPI_POLARITY_LOW);
    LL_SPI_SetClockPhase(spi, LL_SPI_PHASE_1EDGE);
    LL_SPI_Enable(spi);

    /* 读一次 DR 避免首字节异常 */
    (void)LL_SPI_ReceiveData8(spi);
}

spi_bus_t *spi_stm32_ll_bus(spi_stm32_ll_t *ll)
{
    return (ll == NULL) ? NULL : &ll->bus;
}

int spi_stm32_ll_recover(spi_stm32_ll_t *ll)
{
    if (ll == NULL || ll->hw == NULL) {
        return (int)SPI_LL_ERROR_PARAM;
    }
    return (int)spi_recover(ll);
}
