/**
 * @file    spi_stm32_ll.c
 * @brief   SPI 总线的 STM32F1 LL 实现
 * @details 负责 GPIO 复用、SPI 外设时钟/参数配置与阻塞式全双工传输。
 *          这是本工程中唯一产生 SPI 时序的文件；上层只看到 spi_bus.h。
 */
#include "spi_stm32_ll.h"

#define SPI_TIMEOUT_VAL 100000U    /* 单字节等待 TXE/RXNE 的超时计数 */
#define SPI_DMA_MIN_LEN 16U        /* 小于该长度走轮询，DMA 启动开销更大 */
#define SPI_DMA_TIMEOUT 0xFFFFFFU  /* DMA 完成等待超时 */

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

    /* 参数未变化时直接返回，避免每次片选都重配外设 */
    if (ll->configured &&
        ll->current.clock_hz == cfg->clock_hz &&
        ll->current.mode == cfg->mode) {
        return 0;
    }

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

    ll->current = *cfg;
    ll->configured = 1U;
    return 0;
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

/** @brief 等待指定通道传输完成 */
static int dma_wait_tc(DMA_TypeDef *dma, uint32_t ch)
{
    uint32_t shift = (ch - 1U) * 4U;
    uint32_t timeout = SPI_DMA_TIMEOUT;

    while ((dma->ISR & (DMA_ISR_TCIF1 << shift)) == 0U) {
        if (--timeout == 0U) {
            return -1;
        }
    }
    return 0;
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

/** @brief DMA 传输（阻塞等待完成，总线仍归当前设备独占） */
static int ll_transfer_dma(spi_stm32_ll_t *ll, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    SPI_TypeDef *spi = ll->hw->periph;
    DMA_TypeDef *dma = ll->hw->dma;
    uint32_t txc = ll->hw->dma_tx_ch;
    uint32_t rxc = ll->hw->dma_rx_ch;
    uint32_t timeout;
    int result = 0;

    /* 1. 清掉上一轮残留（RXNE / OVR），避免首字节错位 */
    (void)LL_SPI_ReceiveData8(spi);
    LL_SPI_ClearFlag_OVR(spi);

    /* 2. 配置通道：先 RX 后 TX，保证时钟到来前接收端就绪 */
    if (rx != NULL) {
        dma_setup_channel(dma, rxc, rx, LL_SPI_DMA_GetRegAddr(spi),
                          LL_DMA_DIRECTION_PERIPH_TO_MEMORY, len);
    }
    dma_setup_channel(dma, txc, tx, LL_SPI_DMA_GetRegAddr(spi),
                      LL_DMA_DIRECTION_MEMORY_TO_PERIPH, len);

    /* 3. 打开 SPI 的 DMA 请求 */
    if (rx != NULL) {
        LL_SPI_EnableDMAReq_RX(spi);
        LL_DMA_EnableChannel(dma, rxc);
    }
    LL_SPI_EnableDMAReq_TX(spi);
    LL_DMA_EnableChannel(dma, txc);

    /* 4. 等待收发完成 */
    if (dma_wait_tc(dma, txc) != 0) {
        result = -1;
        goto cleanup;
    }
    if (rx != NULL && dma_wait_tc(dma, rxc) != 0) {
        result = -1;
        goto cleanup;
    }

    /* 5. 等最后一个字节真正移出移位寄存器，才能释放片选 */
    timeout = SPI_TIMEOUT_VAL;
    while (!LL_SPI_IsActiveFlag_TXE(spi)) {
        if (--timeout == 0U) {
            result = -1;
            goto cleanup;
        }
    }
    timeout = SPI_TIMEOUT_VAL;
    while (LL_SPI_IsActiveFlag_BSY(spi)) {
        if (--timeout == 0U) {
            result = -1;
            goto cleanup;
        }
    }

    /* 6. 只发不收时，丢弃接收寄存器内容并清溢出标志 */
    if (rx == NULL) {
        (void)LL_SPI_ReceiveData8(spi);
        LL_SPI_ClearFlag_OVR(spi);
    }

cleanup:
    LL_SPI_DisableDMAReq_TX(spi);
    LL_SPI_DisableDMAReq_RX(spi);
    LL_DMA_DisableChannel(dma, txc);
    if (rx != NULL) {
        LL_DMA_DisableChannel(dma, rxc);
    }
    dma_clear_flags(dma, txc);
    if (rx != NULL) {
        dma_clear_flags(dma, rxc);
    }
    return result;
}

/** @brief 轮询传输（小块数据，避免 DMA 启动开销） */
static int ll_transfer_poll(spi_stm32_ll_t *ll, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    SPI_TypeDef *spi = ll->hw->periph;
    uint32_t i;

    for (i = 0; i < len; i++) {
        uint32_t timeout = SPI_TIMEOUT_VAL;
        uint8_t rx_byte;

        while (!LL_SPI_IsActiveFlag_TXE(spi)) {
            if (--timeout == 0U) {
                return -1;
            }
        }
        LL_SPI_TransmitData8(spi, tx[i]);

        timeout = SPI_TIMEOUT_VAL;
        while (!LL_SPI_IsActiveFlag_RXNE(spi)) {
            if (--timeout == 0U) {
                return -1;
            }
        }
        rx_byte = LL_SPI_ReceiveData8(spi);

        if (rx != NULL) {
            rx[i] = rx_byte;
        }
    }

    return 0;
}

static int ll_transfer(spi_bus_t *bus, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    spi_stm32_ll_t *ll = (spi_stm32_ll_t *)bus->ctx;

    /* 设备按需选择：只有显式启用 DMA 且数据块足够大时才走 DMA */
    if (ll->current.use_dma && ll->hw->dma != NULL && len >= SPI_DMA_MIN_LEN) {
        return ll_transfer_dma(ll, tx, rx, len);
    }
    return ll_transfer_poll(ll, tx, rx, len);
}

/** @brief 当前是否处于中断上下文（IPSR 非 0 即 ISR） */
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
    LL_GPIO_InitTypeDef gpio = {0};
    LL_SPI_InitTypeDef spi_init = {0};
    spi_bus_config_t default_cfg = { 1000000U, SPI_BUS_MODE_0 };

    ll->hw = hw;
    ll->configured = 0U;
    ll->bus.ops = &s_stm32_ll_ops;
    ll->bus.ctx = ll;

    /* 1. 引脚：SCK/MOSI 复用推挽，MISO 浮空输入 */
    gpio_clock_enable(hw->sck_port);
    gpio_clock_enable(hw->mosi_port);
    if (hw->miso_port != NULL) {
        gpio_clock_enable(hw->miso_port);
    }

    gpio.Mode = LL_GPIO_MODE_ALTERNATE;
    gpio.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;

    gpio.Pin = hw->sck_pin;
    LL_GPIO_Init(hw->sck_port, &gpio);

    gpio.Pin = hw->mosi_pin;
    LL_GPIO_Init(hw->mosi_port, &gpio);

    if (hw->miso_port != NULL && hw->miso_pin != 0U) {
        gpio.Mode = LL_GPIO_MODE_FLOATING;
        gpio.Pin = hw->miso_pin;
        LL_GPIO_Init(hw->miso_port, &gpio);
    }

    /* 2. SPI 外设：主机、8 位、MSB 优先、软件 NSS */
    spi_clock_enable(hw->periph);
    if (hw->dma != NULL) {
        dma_clock_enable(hw->dma);
    }
    LL_SPI_Disable(hw->periph);

    spi_init.TransferDirection = LL_SPI_FULL_DUPLEX;
    spi_init.Mode              = LL_SPI_MODE_MASTER;
    spi_init.DataWidth         = LL_SPI_DATAWIDTH_8BIT;
    spi_init.BitOrder          = LL_SPI_MSB_FIRST;
    spi_init.NSS               = LL_SPI_NSS_SOFT;
    spi_init.CRCCalculation    = LL_SPI_CRCCALCULATION_DISABLE;
    LL_SPI_Init(hw->periph, &spi_init);

    /* 3. 默认参数（后续由设备按需 configure 切换） */
    (void)ll_configure(&ll->bus, &default_cfg);
}

spi_bus_t *spi_stm32_ll_bus(spi_stm32_ll_t *ll)
{
    return &ll->bus;
}
