/**
 * @file  test_bus.c
 * @brief Host 单元测试：spi_bus.h 所有权 / 嵌套 / 配置规则。
 *        用 mock ops 代替真实 LL 实现，验证接口契约（与硬件无关）。
 */
#include "spi_bus.h"
#include <stdio.h>
#include <string.h>

static int g_cfg_calls = 0;
static int g_xfer_calls = 0;
static int g_in_isr = 0;
static spi_bus_config_t g_last_cfg;
static uint8_t g_rx_buf[64];

static int mock_configure(spi_bus_t *bus, const spi_bus_config_t *cfg)
{
    (void)bus;
    g_cfg_calls++;
    g_last_cfg = *cfg;
    return 0;
}

static int mock_transfer(spi_bus_t *bus, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    (void)bus;
    g_xfer_calls++;
    if (rx != NULL && len <= sizeof(g_rx_buf)) {
        memcpy(rx, tx, len);   /* 环回 */
    }
    return 0;
}

static int mock_in_isr(spi_bus_t *bus) { (void)bus; return g_in_isr; }

static const spi_bus_ops_t mock_ops = {
    .configure = mock_configure,
    .transfer  = mock_transfer,
    .in_isr    = mock_in_isr,
};

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
    else { printf("  ok: %s\n", msg); } \
} while (0)

int main(void)
{
    spi_bus_t bus;
    const spi_bus_config_t cfg_a = { 18000000U, SPI_BUS_MODE_0, 1 };
    const spi_bus_config_t cfg_b = {  9000000U, SPI_BUS_MODE_0, 1 };
    const spi_bus_config_t cfg_diff_dma = { 18000000U, SPI_BUS_MODE_0, 0 };
    uint8_t owner_a = 0xAA, owner_b = 0xBB;
    uint8_t tx[8] = {1,2,3,4,5,6,7,8};
    uint8_t rx[8] = {0};
    int r;

    memset(&bus, 0, sizeof(bus));
    bus.ops = &mock_ops;
    g_cfg_calls = g_xfer_calls = g_in_isr = 0;

    printf("== spi_bus ownership contract ==\n");

    /* 1. NULL owner acquire 必须拒绝，状态不变 */
    r = spi_bus_acquire(&bus, NULL, &cfg_a);
    CHECK(r == -1, "NULL owner acquire rejected");
    CHECK(bus.owner == NULL && bus.depth == 0, "state unchanged after NULL acquire");

    /* 2. NULL owner release 必须拒绝 */
    r = spi_bus_release(&bus, NULL);
    CHECK(r == -1, "NULL owner release rejected");

    /* 3. 未占用时 transfer 必须拒绝（owner 不匹配 / depth=0） */
    r = spi_bus_transfer(&bus, &owner_a, tx, rx, 4);
    CHECK(r == -1, "transfer without acquire rejected");

    /* 4. 正常 acquire */
    r = spi_bus_acquire(&bus, &owner_a, &cfg_a);
    CHECK(r == 0, "acquire A ok");
    CHECK(bus.owner == &owner_a && bus.depth == 1, "owner=A depth=1");
    CHECK(g_cfg_calls == 1, "configure called once on first acquire");

    /* 5. 非当前 owner transfer 必须拒绝 */
    r = spi_bus_transfer(&bus, &owner_b, tx, rx, 4);
    CHECK(r == -1, "non-owner transfer rejected");

    /* 6. 非当前 owner release 必须拒绝 */
    r = spi_bus_release(&bus, &owner_b);
    CHECK(r == -1, "non-owner release rejected");
    CHECK(bus.depth == 1, "depth unchanged after non-owner release");

    /* 7. 同 owner 相同配置嵌套：不重新 configure */
    g_cfg_calls = 0;
    r = spi_bus_acquire(&bus, &owner_a, &cfg_a);
    CHECK(r == 0, "nested acquire same cfg ok");
    CHECK(bus.depth == 2, "depth -> 2");
    CHECK(g_cfg_calls == 0, "no reconfigure on nested same-cfg acquire");

    /* 8. 嵌套期间 transfer 正常工作 */
    g_xfer_calls = 0;
    memset(rx, 0, sizeof(rx));
    r = spi_bus_transfer(&bus, &owner_a, tx, rx, 4);
    CHECK(r == 0 && g_xfer_calls == 1, "nested transfer ok");
    CHECK(memcmp(rx, tx, 4) == 0, "nested transfer data correct");

    /* 9. 嵌套期间禁止改配置 */
    r = spi_bus_configure(&bus, &cfg_b);
    CHECK(r == -1, "configure while owned rejected");

    /* 10. 同 owner 不同配置嵌套必须拒绝，且状态不变 */
    uint16_t d_before = bus.depth;
    r = spi_bus_acquire(&bus, &owner_a, &cfg_diff_dma);
    CHECK(r == -1, "nested acquire diff cfg rejected");
    CHECK(bus.depth == d_before, "depth unchanged after diff-cfg reject");

    /* 11. depth 为 0 时 release 拒绝 —— 先全部释放再测 */
    r = spi_bus_release(&bus, &owner_a);   /* depth 2 -> 1, returns 0 (still nested) */
    CHECK(r == 0, "first nested release returns 0");
    r = spi_bus_release(&bus, &owner_a);   /* depth 1 -> 0, returns 1 (freed) */
    CHECK(r == 1, "final release returns 1");
    CHECK(bus.owner == NULL && bus.depth == 0, "owner=NULL depth=0 after release");

    r = spi_bus_release(&bus, &owner_a);   /* depth already 0 */
    CHECK(r == -1, "release at depth 0 rejected");

    /* 12. depth 溢出保护：推到 UINT16_MAX 后拒绝继续嵌套，状态不变 */
    /* 先 acquire 一次建立 owner */
    spi_bus_acquire(&bus, &owner_a, &cfg_a);
    /* 把 depth 直接推到 UINT16_MAX（模拟已嵌套到顶） */
    bus.depth = UINT16_MAX;
    uint16_t depth_cap = bus.depth;
    r = spi_bus_acquire(&bus, &owner_a, &cfg_a);
    CHECK(r == -1, "acquire at UINT16_MAX depth rejected");
    CHECK(bus.depth == depth_cap, "depth unchanged on overflow reject");
    /* 恢复 depth 以便释放 */
    bus.depth = 1;
    spi_bus_release(&bus, &owner_a);
    CHECK(bus.owner == NULL && bus.depth == 0, "clean state after overflow test");

    /* 13. ISR 上下文 acquire 必须拒绝（不留下占用） */
    g_in_isr = 1;
    r = spi_bus_acquire(&bus, &owner_a, &cfg_a);
    CHECK(r == -1, "acquire in ISR rejected");
    CHECK(bus.owner == NULL && bus.depth == 0, "no leak after ISR reject");
    g_in_isr = 0;

    printf("\n%s: %d failures\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
