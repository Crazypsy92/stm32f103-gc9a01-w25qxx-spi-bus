/**
 * @file    bsp_spi.c
 * @brief   板级 SPI1 总线实现（STM32F1）
 * @details 全工程唯一的 SPI1 引脚/外设配置点：GC9A01 与 W25QXX 共用。
 *          设备不再各自初始化 SPI，避免后初始化者覆盖前者的速率/模式。
 */
#include "bsp_spi.h"
#include "spi_stm32_ll.h"
#include "main.h"

/* ================================================================================== */
/* 板级配置（按实际硬件修改）                                         */
/* ================================================================================== */

#define BSP_SPI1_PERIPH     SPI1
#define BSP_SPI1_PCLK_HZ    72000000U   /* APB2 = 72MHz */

#define BSP_SPI1_SCK_PORT   GPIOA
#define BSP_SPI1_SCK_PIN    LL_GPIO_PIN_5
#define BSP_SPI1_MISO_PORT  GPIOA
#define BSP_SPI1_MISO_PIN   LL_GPIO_PIN_6
#define BSP_SPI1_MOSI_PORT  GPIOA
#define BSP_SPI1_MOSI_PIN   LL_GPIO_PIN_7

/* DMA1 固定映射：SPI1_TX → 通道3，SPI1_RX → 通道2 */
#define BSP_SPI1_DMA        DMA1
#define BSP_SPI1_DMA_TX_CH  LL_DMA_CHANNEL_3
#define BSP_SPI1_DMA_RX_CH  LL_DMA_CHANNEL_2

/* ================================================================================== */
/* 内部变量                                                           */
/* ================================================================================== */

static const spi_stm32_ll_hw_t s_spi1_hw = {
    .periph    = BSP_SPI1_PERIPH,
    .pclk_hz   = BSP_SPI1_PCLK_HZ,
    .sck_port  = BSP_SPI1_SCK_PORT,
    .sck_pin   = BSP_SPI1_SCK_PIN,
    .miso_port = BSP_SPI1_MISO_PORT,
    .miso_pin  = BSP_SPI1_MISO_PIN,
    .mosi_port = BSP_SPI1_MOSI_PORT,
    .mosi_pin  = BSP_SPI1_MOSI_PIN,
    .dma       = BSP_SPI1_DMA,
    .dma_tx_ch = BSP_SPI1_DMA_TX_CH,
    .dma_rx_ch = BSP_SPI1_DMA_RX_CH,
};

static spi_stm32_ll_t s_spi1;
static uint8_t s_spi1_inited = 0U;

/* ================================================================================== */
/* 对外接口                                                           */
/* ================================================================================== */

spi_bus_t *BSP_SPI1_GetBus(void)
{
    if (!s_spi1_inited) {
        spi_stm32_ll_init(&s_spi1, &s_spi1_hw);
        s_spi1_inited = 1U;
    }
    return spi_stm32_ll_bus(&s_spi1);
}

/* ================================================================================== */
/* 中断抢占探针（测试用，可整体删除）                                   */
/* ================================================================================== */

/* APB1 分频不为 1 时定时器时钟 = PCLK1 x 2 = 72MHz */
#define BSP_PROBE_TIM_CLK_HZ 72000000U

static spi_bus_t            *s_probe_bus;
static const spi_bus_config_t s_probe_cfg = { 1000000U, SPI_BUS_MODE_0, 0 };
static uint8_t               s_probe_owner;  /* 仅作唯一标识，探针不做真实传输 */
static volatile uint32_t     s_probe_probes;
static volatile uint32_t     s_probe_saw_busy;
static volatile uint32_t     s_probe_granted;

void BSP_SPI1_PreemptProbe_Init(void)
{
    s_probe_bus = BSP_SPI1_GetBus();

    /* 必须先开 TIM2 时钟，否则后续对 DIER/EGR 的写入不会生效（注入会失效） */
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
    TIM2->CR1  = 0;
    TIM2->DIER = 0;
    TIM2->SR   = (uint32_t)~TIM_SR_UIF;

    NVIC_SetPriority(TIM2_IRQn, 0);      /* 高于 SysTick(15)，保证能抢占主循环 */
    NVIC_EnableIRQ(TIM2_IRQn);
}

void BSP_SPI1_PreemptProbe_Start(uint32_t rate_hz)
{
    uint32_t arr;

    if (s_probe_bus == NULL) {
        BSP_SPI1_PreemptProbe_Init();
    }
    if (rate_hz == 0U) {
        rate_hz = 10000U;
    }

    s_probe_probes = 0U;
    s_probe_saw_busy = 0U;
    s_probe_granted = 0U;

    arr = 1000000U / rate_hz;

    /* 工程未内置 LL TIM 驱动，这里直接写寄存器：72MHz -> 1MHz 计数 */
    TIM2->CR1  = 0;
    TIM2->PSC  = (BSP_PROBE_TIM_CLK_HZ / 1000000U) - 1U;
    TIM2->ARR  = (arr == 0U) ? 0U : (arr - 1U);
    TIM2->EGR  = TIM_EGR_UG;        /* 立即装载 PSC/ARR */
    TIM2->SR   = (uint32_t)~TIM_SR_UIF;
    TIM2->DIER = TIM_DIER_UIE;
    TIM2->CR1  = TIM_CR1_CEN;
}

void BSP_SPI1_PreemptProbe_Stop(void)
{
    TIM2->CR1  = 0;
    TIM2->DIER = 0;
    TIM2->SR   = (uint32_t)~TIM_SR_UIF;
}

void BSP_SPI1_PreemptProbe_Inject(void)
{
    /* 软件触发一次更新事件：置 UIF 并立即进入 TIM2 中断（同步执行） */
    TIM2->DIER |= TIM_DIER_UIE;
    TIM2->EGR = TIM_EGR_UG;
    __DSB();
    __ISB();
}

void BSP_SPI1_PreemptProbe_Get(bsp_spi_probe_t *out)
{
    if (out == NULL) {
        return;
    }
    out->probes   = s_probe_probes;
    out->saw_busy = s_probe_saw_busy;
    out->granted  = s_probe_granted;
}

/**
 * @brief 探针中断：模拟"高优先级任务/中断也要用 SPI"
 * @note  期望行为：acquire 因 ISR 上下文被拒绝，granted 恒为 0；
 *        saw_busy 记录落在主循环事务正中间的次数
 */
void TIM2_IRQHandler(void)
{
    if ((TIM2->SR & TIM_SR_UIF) == 0U) {
        return;
    }
    TIM2->SR = (uint32_t)~TIM_SR_UIF;

    s_probe_probes++;
    if (spi_bus_owner(s_probe_bus) != NULL) {
        s_probe_saw_busy++;
    }
    if (spi_bus_acquire(s_probe_bus, &s_probe_owner, &s_probe_cfg) == 0) {
        s_probe_granted++;
        (void)spi_bus_release(s_probe_bus, &s_probe_owner);
    }
}
