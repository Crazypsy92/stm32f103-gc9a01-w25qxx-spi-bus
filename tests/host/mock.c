/**
 * @file  mock.c
 * @brief Host 桩的全局状态与读时副作用函数实现。
 */
#include "stm32f1_host_stub.h"

GPIO_TypeDef mock_gpioa, mock_gpiob, mock_gpioc;
SPI_TypeDef  mock_spi1, mock_spi2;
DMA_TypeDef  mock_dma1;
DMA_Channel_TypeDef mock_dma_ch[8];
DWT_Type      mock_dwt;
CoreDebug_Type mock_coredbg;

volatile uint32_t mock_cyccnt = 0;
uint32_t          mock_dwt_step = 0;
volatile uint32_t mock_spi_sr = 0;
volatile uint8_t  mock_spi_rx = 0;
volatile uint8_t  mock_spi_tx = 0;
volatile uint32_t mock_dma_isr = 0;
volatile uint8_t  mock_dma_en[8] = {0};
volatile uint32_t mock_cndtr_log[8][64];
volatile uint32_t mock_cndtr_cnt[8] = {0};
volatile uint32_t mock_configure_calls = 0;

/* CYCCNT 读取：每次推进一个步长（模拟时间流逝） */
static uint32_t mock_cyccnt_rd(void)
{
    mock_cyccnt += mock_dwt_step;
    return mock_cyccnt;
}

/* DMA ISR 读取：同样推进时钟，使等待循环能超时 */
static uint32_t mock_dma_isr_rd(void)
{
    mock_cyccnt += mock_dwt_step;
    return mock_dma_isr;
}

void mock_reset(void)
{
    memset(&mock_gpioa, 0, sizeof(mock_gpioa));
    memset(&mock_gpiob, 0, sizeof(mock_gpiob));
    memset(&mock_gpioc, 0, sizeof(mock_gpioc));
    memset(&mock_spi1, 0, sizeof(mock_spi1));
    memset(&mock_spi2, 0, sizeof(mock_spi2));
    memset(&mock_dma1, 0, sizeof(mock_dma1));
    memset(mock_dma_ch, 0, sizeof(mock_dma_ch));
    memset(&mock_dwt, 0, sizeof(mock_dwt));
    memset(&mock_coredbg, 0, sizeof(mock_coredbg));

    mock_dwt.cyccnt_read = mock_cyccnt_rd;
    mock_dma1.isr_read = mock_dma_isr_rd;

    mock_cyccnt = 0;
    mock_dwt_step = 0;
    mock_spi_sr = 0;
    mock_spi_rx = 0;
    mock_spi_tx = 0;
    mock_dma_isr = 0;
    memset((void*)mock_dma_en, 0, sizeof(mock_dma_en));
    memset((void*)mock_cndtr_cnt, 0, sizeof(mock_cndtr_cnt));
    memset((void*)mock_cndtr_log, 0, sizeof(mock_cndtr_log));
    mock_configure_calls = 0;
}
