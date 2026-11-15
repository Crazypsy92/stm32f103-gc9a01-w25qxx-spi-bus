/**
 * @file  test_ll.c
 * @brief Host 单元测试：spi_stm32_ll.c 的 DMA 错误检测、分块、超时、恢复。
 *        直接编译真实 spi_stm32_ll.c，用桩头模拟 SPI/DMA 寄存器。
 */
#include "spi_stm32_ll.h"
#include <stdio.h>
#include <string.h>

#define LARGE_DMA_LEN 70000U
/* Host 专用静态缓冲区：避免旧版 480B 数组被当作 70000B 使用。 */
static uint8_t s_large_tx[LARGE_DMA_LEN];
static uint8_t s_large_rx[LARGE_DMA_LEN];

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
    else { printf("  ok: %s\n", msg); } \
} while (0)

static spi_stm32_ll_t s_ll;
static const spi_stm32_ll_hw_t s_hw = {
    .periph    = SPI1,
    .pclk_hz   = 72000000U,
    .sck_port  = GPIOA, .sck_pin  = 0,
    .miso_port = GPIOA, .miso_pin = 0,
    .mosi_port = GPIOA, .mosi_pin = 0,
    .dma       = DMA1,
    .dma_tx_ch = 3,
    .dma_rx_ch = 2,
};

static void setup(void)
{
    mock_reset();
    memset(&s_ll, 0, sizeof(s_ll));
    spi_stm32_ll_init(&s_ll, &s_hw);
}

/* 配置一次（bus owner 必须为 NULL，init 后满足） */
static void cfg(const spi_bus_config_t *c)
{
    spi_bus_configure(&s_ll.bus, c);
}

int main(void)
{
    static const spi_bus_config_t cfg_dma = { 9000000U, SPI_BUS_MODE_0, 1 };
    static const spi_bus_config_t cfg_poll = { 9000000U, SPI_BUS_MODE_0, 0 };
    uint8_t tx[480], rx[480];
    int st;

    for (uint32_t i = 0; i < sizeof(tx); i++) tx[i] = (uint8_t)(i & 0xFF);

    /* ---------- 1. 正常 DMA 传输 ---------- */
    printf("-- DMA normal --\n");
    setup();
    cfg(&cfg_dma);
    mock_dma_isr = DMA_ISR_TCIF3 | DMA_ISR_TCIF2;   /* TX/RX 都完成 */
    mock_spi_sr  = SR_TXE;                            /* 传输后 TXE=1, BSY=0 */
    mock_dwt_step = 0;
    memset(rx, 0, sizeof(rx));
    st = s_ll.bus.ops->transfer(&s_ll.bus, tx, rx, 32U);
    CHECK(st == (int)SPI_LL_OK, "DMA 32B returns OK");
    CHECK(mock_cndtr_cnt[3] == 1 && mock_cndtr_log[3][0] == 32, "CNDTR set to 32");
    /* mock 不模拟 DMA 数据搬运，改为校验 RX 通道内存地址已指向 rx（证明 DMA RX 通路已配置） */
    CHECK(mock_dma_ch[2].CMAR == (uint32_t)(uintptr_t)rx, "RX DMA CMAR points to rx buffer");

    /* ---------- 2. DMA TX TEIF 必须返回 ERROR_DMA ---------- */
    printf("-- DMA TEIF --\n");
    setup();
    cfg(&cfg_dma);
    mock_dma_isr = DMA_ISR_TEIF3;
    mock_spi_sr  = 0;
    mock_dwt_step = 0;
    st = s_ll.bus.ops->transfer(&s_ll.bus, tx, rx, 32U);
    CHECK(st == (int)SPI_LL_ERROR_DMA, "TX TEIF -> SPI_LL_ERROR_DMA");
    /* 恢复后下一次必须成功 */
    mock_reset();
    mock_dma_isr = DMA_ISR_TCIF3 | DMA_ISR_TCIF2;
    mock_spi_sr  = SR_TXE;
    st = spi_stm32_ll_recover(&s_ll);
    CHECK(st == 0, "recover after TEIF succeeds");
    st = s_ll.bus.ops->transfer(&s_ll.bus, tx, rx, 32U);
    CHECK(st == (int)SPI_LL_OK, "transfer OK after recovery from TEIF");

    /* ---------- 3. DMA 超时（无 TC、无 TE，时钟推进） ---------- */
    printf("-- DMA timeout --\n");
    setup();
    cfg(&cfg_dma);
    mock_dma_isr = 0;
    mock_dwt_step = 100000U;        /* 每次 ISR 读推进 100k 周期 */
    st = s_ll.bus.ops->transfer(&s_ll.bus, tx, rx, 16U);
    CHECK(st == (int)SPI_LL_ERROR_TIMEOUT, "DMA no-complete -> SPI_LL_ERROR_TIMEOUT");
    /* 恢复后重试正常 */
    mock_reset();
    mock_dma_isr = DMA_ISR_TCIF3 | DMA_ISR_TCIF2;
    mock_spi_sr  = SR_TXE;
    (void)spi_stm32_ll_recover(&s_ll);
    st = s_ll.bus.ops->transfer(&s_ll.bus, tx, rx, 16U);
    CHECK(st == (int)SPI_LL_OK, "transfer OK after recovery from timeout");

    /* ---------- 4. 真缓冲区验证 65535/65536/70000 B DMA 分块边界 ---------- */
    printf("-- DMA chunking boundaries (65535/65536/70000 B) --\n");
    for (uint32_t k = 0; k < LARGE_DMA_LEN; ++k) {
        s_large_tx[k] = (uint8_t)(k ^ 0x5AU);
    }
    {
        const uint32_t sizes[] = { 65535U, 65536U, 70000U };
        for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            uint32_t n = sizes[i], parts = (n > 65535U) ? 2U : 1U;
            setup();
            cfg(&cfg_dma);
            mock_dma_isr = DMA_ISR_TCIF3 | DMA_ISR_TCIF2;
            mock_spi_sr = SR_TXE;
            mock_bsy_checks = 0;
            st = s_ll.bus.ops->transfer(&s_ll.bus, s_large_tx, s_large_rx, n);
            CHECK(st == (int)SPI_LL_OK, "large full-duplex DMA returns OK");
            CHECK(mock_cndtr_cnt[3] == parts && mock_cndtr_cnt[2] == parts, "TX/RX have correct chunk count");
            CHECK(mock_cndtr_log[3][0] == 65535U && mock_cndtr_log[2][0] == 65535U, "first chunk is 65535");
            if (parts == 2U) {
                CHECK(mock_cndtr_log[3][1] == n - 65535U && mock_cndtr_log[2][1] == n - 65535U,
                      "second chunk is exact remainder");
                CHECK(mock_cmar_log[3][1] == (uint32_t)(uintptr_t)(s_large_tx + 65535U) &&
                      mock_cmar_log[2][1] == (uint32_t)(uintptr_t)(s_large_rx + 65535U),
                      "second TX/RX chunk memory address starts at offset 65535");
            }
            CHECK(mock_cmar_log[3][0] == (uint32_t)(uintptr_t)s_large_tx &&
                  mock_cmar_log[2][0] == (uint32_t)(uintptr_t)s_large_rx,
                  "first TX/RX DMA addresses match valid buffers");
            CHECK(mock_bsy_checks >= parts, "BSY checked after every DMA chunk");
            CHECK(mock_dma_en[2] == 0 && mock_dma_en[3] == 0, "DMA channels disabled after transfer");
        }
    }

    /* 发送屏幕像素时 TX-only DMA 的分块也要正确完成。 */
    printf("-- DMA TX-only 70000 B --\n");
    setup();
    cfg(&cfg_dma);
    mock_dma_isr = DMA_ISR_TCIF3;
    mock_spi_sr = SR_TXE;
    mock_bsy_checks = 0;
    st = s_ll.bus.ops->transfer(&s_ll.bus, s_large_tx, NULL, LARGE_DMA_LEN);
    CHECK(st == (int)SPI_LL_OK, "70000B TX-only returns OK");
    CHECK(mock_cndtr_cnt[3] == 2 && mock_cndtr_cnt[2] == 0, "TX-only uses no RX DMA channel");
    CHECK(mock_bsy_checks >= 2, "TX-only checks BSY at each chunk boundary");

    /* BSY 卡死时不得进入第二个分块，且必须有界超时。 */
    printf("-- DMA stuck BSY prevents next chunk --\n");
    setup();
    cfg(&cfg_dma);
    mock_dma_isr = DMA_ISR_TCIF3;
    mock_spi_sr = SR_TXE | SR_BSY;
    mock_dwt_step = 200000U;
    st = s_ll.bus.ops->transfer(&s_ll.bus, s_large_tx, NULL, LARGE_DMA_LEN);
    CHECK(st == (int)SPI_LL_ERROR_TIMEOUT, "BSY stuck returns bounded timeout");
    CHECK(mock_cndtr_cnt[3] == 1, "no second DMA chunk starts while SPI busy");

    /* ---------- 5. 轮询正常 ---------- */
    printf("-- Polling normal --\n");
    setup();
    cfg(&cfg_poll);
    mock_spi_sr  = SR_TXE | SR_RXNE;
    mock_spi_rx  = 0xA5;
    mock_dwt_step = 0;
    st = s_ll.bus.ops->transfer(&s_ll.bus, tx, rx, 8U);
    CHECK(st == (int)SPI_LL_OK, "poll 8B returns OK");

    /* ---------- 6. 轮询 TXE 超时 ---------- */
    printf("-- Polling TXE timeout --\n");
    setup();
    cfg(&cfg_poll);
    mock_spi_sr  = 0;
    mock_dwt_step = 200000U;
    st = s_ll.bus.ops->transfer(&s_ll.bus, tx, rx, 4U);
    CHECK(st == (int)SPI_LL_ERROR_TIMEOUT, "poll TXE timeout -> ERROR_TIMEOUT");

    /* ---------- 7. use_dma 缓存：clock/mode 不变时切换 use_dma 必须生效 ---------- */
    printf("-- use_dma cache --\n");
    setup();
    cfg(&cfg_dma);                                  /* use_dma=1 */
    mock_dma_isr = DMA_ISR_TCIF3 | DMA_ISR_TCIF2;
    mock_spi_sr  = SR_TXE;
    mock_cndtr_cnt[3] = 0;
    st = s_ll.bus.ops->transfer(&s_ll.bus, tx, rx, 16U);
    CHECK(st == (int)SPI_LL_OK && mock_cndtr_cnt[3] == 1, "use_dma=1 uses DMA");

    cfg(&cfg_poll);                                 /* 同 clock/mode，use_dma=0 */
    mock_cndtr_cnt[3] = 0;
    mock_spi_sr = SR_TXE | SR_RXNE;
    st = s_ll.bus.ops->transfer(&s_ll.bus, tx, rx, 16U);
    CHECK(st == (int)SPI_LL_OK && mock_cndtr_cnt[3] == 0, "use_dma=0 uses polling (no CNDTR write)");

    printf("\n%s: %d failures\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
