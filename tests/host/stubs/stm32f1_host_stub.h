/**
 * @file  stm32f1_host_stub.h
 * @brief Host 单元测试用 STM32F1 LL 最小桩实现。
 *        仅提供 spi_stm32_ll.c 实际使用到的符号，用全局变量模拟寄存器，
 *        便于注入 DMA 错误、超时、BSY 等异常状态。
 *
 * 技巧：DWT->CYCCNT 与 dma->ISR 是直接成员读取，无法在结构体字段上加
 * 副作用。这里把对应字段定义为"函数指针成员 + 宏"，使读取变为函数调用，
 * 从而可以在读取时推进 CYCCNT（模拟时间流逝）以驱动超时判定。
 */
#ifndef STM32F1_HOST_STUB_H
#define STM32F1_HOST_STUB_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 基础类型 ===== */
typedef struct { volatile uint32_t CRH; volatile uint32_t CRL; volatile uint32_t IDR;
                 volatile uint32_t ODR; volatile uint32_t BSRR; volatile uint32_t BRR;
                 volatile uint32_t LCKR; } GPIO_TypeDef;

typedef struct { volatile uint32_t CR1; volatile uint32_t CR2; volatile uint32_t SR;
                 volatile uint32_t DR;  volatile uint32_t CRCPR; volatile uint32_t RXCRCR;
                 volatile uint32_t TXCRCR; volatile uint32_t I2SCFGR; volatile uint32_t I2SPR; } SPI_TypeDef;

/* DMA: ISR 用函数指针成员，使读取时可推进 CYCCNT */
typedef struct {
    uint32_t (*isr_read)(void);
    volatile uint32_t IFCR;
} DMA_TypeDef;
typedef struct { volatile uint32_t CCR; volatile uint32_t CNDTR; volatile uint32_t CPAR;
                 volatile uint32_t CMAR; } DMA_Channel_TypeDef;

/* DWT: CYCCNT 用函数指针成员 */
typedef struct {
    uint32_t (*cyccnt_read)(void);
    volatile uint32_t CTRL;
} DWT_Type;
typedef struct { volatile uint32_t DEMCR; } CoreDebug_Type;

/* ===== 外设实例（全局 mock） ===== */
extern GPIO_TypeDef mock_gpioa, mock_gpiob, mock_gpioc;
extern SPI_TypeDef  mock_spi1, mock_spi2;
extern DMA_TypeDef  mock_dma1;
extern DMA_Channel_TypeDef mock_dma_ch[8];
extern DWT_Type      mock_dwt;
extern CoreDebug_Type mock_coredbg;

#define GPIOA (&mock_gpioa)
#define GPIOB (&mock_gpiob)
#define GPIOC (&mock_gpioc)
#define SPI1  (&mock_spi1)
#define SPI2  (&mock_spi2)
#define DMA1  (&mock_dma1)
#define DWT   (&mock_dwt)
#define CoreDebug (&mock_coredbg)

/* 关键：把成员名重定向到带括号的函数调用 */
#define CYCCNT cyccnt_read()
#define ISR    isr_read()

/* ===== 可注入的标志/数据 ===== */
extern volatile uint32_t mock_cyccnt;
extern uint32_t          mock_dwt_step;     /* 每次 tick 推进的周期数 */
extern volatile uint32_t mock_spi_sr;       /* BSY/TXE/RXNE/OVR */
extern volatile uint8_t  mock_spi_rx;       /* ReceiveData8 返回值 */
extern volatile uint8_t  mock_spi_tx;       /* 最近一次 TransmitData8 的值 */
extern volatile uint32_t mock_dma_isr;      /* DMA ISR 镜像 */
extern volatile uint8_t  mock_dma_en[8];    /* 通道是否使能 */
extern volatile uint32_t mock_cndtr_log[8][64];
extern volatile uint32_t mock_cndtr_cnt[8];
extern volatile uint32_t mock_cmar_log[8][64]; /* 每块的 DMA 内存地址（Host 指针低 32 位） */
extern volatile uint32_t mock_cmar_cnt[8];
extern volatile uint32_t mock_bsy_checks;      /* LL SPI BSY 检查次数，用于逐块时序验收 */
extern volatile uint32_t mock_configure_calls; /* ll_configure 被调用次数 */

#define SR_BSY   (1U << 7)
#define SR_OVR   (1U << 6)
#define SR_RXNE  (1U << 0)
#define SR_TXE   (1U << 1)

#define DMA_ISR_TCIF1 (1U << 1)
#define DMA_ISR_TEIF1 (1U << 3)
#define DMA_ISR_TCIF2 (1U << 5)
#define DMA_ISR_TEIF2 (1U << 7)
#define DMA_ISR_TCIF3 (1U << 9)
#define DMA_ISR_TEIF3 (1U << 11)

/* IFCR 写 1 清标志位（桩中写入 IFCR 不改变 mock_dma_isr，
   各测试通过 mock_reset() 复位状态） */
#define DMA_IFCR_CGIF1  (1U << 0)
#define DMA_IFCR_CTCIF1 (1U << 1)
#define DMA_IFCR_CHTIF1 (1U << 2)
#define DMA_IFCR_CTEIF1 (1U << 3)

/* 每次"读"推进时钟 */
static inline void mock_tick(void) { mock_cyccnt += mock_dwt_step; }

/* ===== RCC（无操作） ===== */
#define LL_APB2_GRP1_PERIPH_GPIOA 0
#define LL_APB2_GRP1_PERIPH_GPIOB 0
#define LL_APB2_GRP1_PERIPH_GPIOC 0
#define LL_APB2_GRP1_PERIPH_SPI1  0
#define LL_APB1_GRP1_PERIPH_SPI2  0
#define LL_AHB1_GRP1_PERIPH_DMA1  0
static inline void LL_APB2_GRP1_EnableClock(uint32_t x) { (void)x; }
static inline void LL_APB1_GRP1_EnableClock(uint32_t x) { (void)x; }
static inline void LL_AHB1_GRP1_EnableClock(uint32_t x) { (void)x; }

/* ===== GPIO（无操作） ===== */
typedef struct { uint32_t Pin; uint32_t Mode; uint32_t Speed; uint32_t OutputType;
                 uint32_t Pull; uint32_t Alternate; } LL_GPIO_InitTypeDef;
#define LL_GPIO_MODE_ALTERNATE   0
#define LL_GPIO_MODE_FLOATING    0
#define LL_GPIO_SPEED_FREQ_HIGH  0
#define LL_GPIO_OUTPUT_PUSHPULL  0
static inline void LL_GPIO_Init(GPIO_TypeDef *p, LL_GPIO_InitTypeDef *g) { (void)p; (void)g; }

/* ===== SPI ===== */
#define LL_SPI_BAUDRATEPRESCALER_DIV2   0
#define LL_SPI_BAUDRATEPRESCALER_DIV4   1
#define LL_SPI_BAUDRATEPRESCALER_DIV8   2
#define LL_SPI_BAUDRATEPRESCALER_DIV16  3
#define LL_SPI_BAUDRATEPRESCALER_DIV32  4
#define LL_SPI_BAUDRATEPRESCALER_DIV64  5
#define LL_SPI_BAUDRATEPRESCALER_DIV128 6
#define LL_SPI_BAUDRATEPRESCALER_DIV256 7
#define LL_SPI_POLARITY_LOW   0
#define LL_SPI_POLARITY_HIGH  1
#define LL_SPI_PHASE_1EDGE    0
#define LL_SPI_PHASE_2EDGE    1
#define LL_SPI_MODE_MASTER    0
#define LL_SPI_FULL_DUPLEX    0
#define LL_SPI_DATAWIDTH_8BIT 0
#define LL_SPI_NSS_SOFT       0

static inline uint32_t LL_SPI_IsActiveFlag_BSY(SPI_TypeDef *s)  { (void)s; mock_bsy_checks++; mock_tick(); return (mock_spi_sr & SR_BSY)  ? 1 : 0; }
static inline uint32_t LL_SPI_IsActiveFlag_TXE(SPI_TypeDef *s)  { (void)s; mock_tick(); return (mock_spi_sr & SR_TXE)  ? 1 : 0; }
static inline uint32_t LL_SPI_IsActiveFlag_RXNE(SPI_TypeDef *s) { (void)s; mock_tick(); return (mock_spi_sr & SR_RXNE) ? 1 : 0; }
static inline uint32_t LL_SPI_IsActiveFlag_OVR(SPI_TypeDef *s)  { (void)s; mock_tick(); return (mock_spi_sr & SR_OVR)  ? 1 : 0; }
static inline uint8_t  LL_SPI_ReceiveData8(SPI_TypeDef *s)      { (void)s; mock_spi_sr &= ~SR_RXNE; return (uint8_t)mock_spi_rx; }
static inline void     LL_SPI_TransmitData8(SPI_TypeDef *s, uint8_t b) { (void)s; mock_spi_tx = b; mock_spi_sr |= SR_RXNE; }
static inline void     LL_SPI_EnableDMAReq_TX(SPI_TypeDef *s)  { (void)s; }
static inline void     LL_SPI_DisableDMAReq_TX(SPI_TypeDef *s) { (void)s; }
static inline void     LL_SPI_EnableDMAReq_RX(SPI_TypeDef *s)  { (void)s; }
static inline void     LL_SPI_DisableDMAReq_RX(SPI_TypeDef *s) { (void)s; }
static inline void     LL_SPI_Enable(SPI_TypeDef *s)  { (void)s; }
static inline void     LL_SPI_Disable(SPI_TypeDef *s) { (void)s; }
static inline void     LL_SPI_SetBaudRatePrescaler(SPI_TypeDef *s, uint32_t p) { (void)s; (void)p; }
static inline void     LL_SPI_SetClockPolarity(SPI_TypeDef *s, uint32_t p)     { (void)s; (void)p; }
static inline void     LL_SPI_SetClockPhase(SPI_TypeDef *s, uint32_t p)        { (void)s; (void)p; }
static inline void     LL_SPI_SetMode(SPI_TypeDef *s, uint32_t p)              { (void)s; (void)p; }
static inline void     LL_SPI_SetTransferDirection(SPI_TypeDef *s, uint32_t p) { (void)s; (void)p; }
static inline void     LL_SPI_SetDataWidth(SPI_TypeDef *s, uint32_t p)         { (void)s; (void)p; }
static inline void     LL_SPI_SetNSSMode(SPI_TypeDef *s, uint32_t p)           { (void)s; (void)p; }
static inline void     LL_SPI_ClearFlag_OVR(SPI_TypeDef *s) { (void)s; mock_spi_sr &= ~SR_OVR; }
static inline uint32_t LL_SPI_DMA_GetRegAddr(SPI_TypeDef *s) { return (uint32_t)(uintptr_t)&s->DR; }

/* ===== DMA ===== */
#define LL_DMA_MODE_NORMAL            0
#define LL_DMA_PERIPH_NOINCREMENT     0
#define LL_DMA_MEMORY_INCREMENT       0
#define LL_DMA_PDATAALIGN_BYTE        0
#define LL_DMA_MDATAALIGN_BYTE        0
#define LL_DMA_PRIORITY_HIGH          0
#define LL_DMA_DIRECTION_PERIPH_TO_MEMORY 0
#define LL_DMA_DIRECTION_MEMORY_TO_PERIPH 1

static inline uint32_t LL_DMA_IsEnabledChannel(DMA_TypeDef *d, uint32_t ch) { (void)d; return mock_dma_en[ch] ? 1 : 0; }
static inline void LL_DMA_EnableChannel(DMA_TypeDef *d, uint32_t ch)  { (void)d; mock_dma_en[ch] = 1; }
static inline void LL_DMA_DisableChannel(DMA_TypeDef *d, uint32_t ch) { (void)d; mock_dma_en[ch] = 0; }
static inline void LL_DMA_SetDataTransferDirection(DMA_TypeDef *d, uint32_t ch, uint32_t dir) { (void)d; (void)ch; (void)dir; }
static inline void LL_DMA_SetMode(DMA_TypeDef *d, uint32_t ch, uint32_t m) { (void)d; (void)ch; (void)m; }
static inline void LL_DMA_SetPeriphIncMode(DMA_TypeDef *d, uint32_t ch, uint32_t m) { (void)d; (void)ch; (void)m; }
static inline void LL_DMA_SetMemoryIncMode(DMA_TypeDef *d, uint32_t ch, uint32_t m) { (void)d; (void)ch; (void)m; }
static inline void LL_DMA_SetPeriphSize(DMA_TypeDef *d, uint32_t ch, uint32_t m) { (void)d; (void)ch; (void)m; }
static inline void LL_DMA_SetMemorySize(DMA_TypeDef *d, uint32_t ch, uint32_t m) { (void)d; (void)ch; (void)m; }
static inline void LL_DMA_SetChannelPriorityLevel(DMA_TypeDef *d, uint32_t ch, uint32_t m) { (void)d; (void)ch; (void)m; }
static inline void LL_DMA_ConfigAddresses(DMA_TypeDef *d, uint32_t ch, uint32_t src, uint32_t dst, uint32_t dir) {
    (void)d;
    /* LL 语义：SrcAddress/DstAddress，方向决定哪个是内存 */
    if (dir == LL_DMA_DIRECTION_MEMORY_TO_PERIPH) {
        mock_dma_ch[ch].CMAR = src;   /* Src = memory */
        mock_dma_ch[ch].CPAR = dst;   /* Dst = periph */
    } else {
        mock_dma_ch[ch].CPAR = src;   /* Src = periph */
        mock_dma_ch[ch].CMAR = dst;   /* Dst = memory */
    }
    if (mock_cmar_cnt[ch] < 64U) {
        mock_cmar_log[ch][mock_cmar_cnt[ch]++] = mock_dma_ch[ch].CMAR;
    }
}
static inline void LL_DMA_SetDataLength(DMA_TypeDef *d, uint32_t ch, uint32_t len) {
    (void)d;
    mock_dma_ch[ch].CNDTR = len;
    if (mock_cndtr_cnt[ch] < 64) mock_cndtr_log[ch][mock_cndtr_cnt[ch]++] = len;
}
static inline void LL_DMA_ClearFlag_TC3(DMA_TypeDef *d) { (void)d; mock_dma_isr &= ~DMA_ISR_TCIF3; }
static inline void LL_DMA_ClearFlag_TE3(DMA_TypeDef *d) { (void)d; mock_dma_isr &= ~DMA_ISR_TEIF3; }
static inline void LL_DMA_ClearFlag_TC2(DMA_TypeDef *d) { (void)d; mock_dma_isr &= ~DMA_ISR_TCIF2; }
static inline void LL_DMA_ClearFlag_TE2(DMA_TypeDef *d) { (void)d; mock_dma_isr &= ~DMA_ISR_TEIF2; }

/* ===== DWT / CoreDebug 常量 ===== */
#define CoreDebug_DEMCR_TRCENA_Msk (1U << 24)
#define DWT_CTRL_CYCCNTENA_Msk     (1U << 0)

/* Host 上无中断，IPSR 恒为 0 */
static inline uint32_t __get_IPSR(void) { return 0U; }

/* ===== mock 复位 ===== */
void mock_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* STM32F1_HOST_STUB_H */
