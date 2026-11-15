/**
 * @file app_test.c
 * @brief 应用层显示与总线测试实现
 * @details GC9A01 上电自测、W25QXX 读写校验、总线接口契约检查与抢占压力测试。
 *          Stage 1.2: 引入安全测试模式开关，默认上电严禁执行任何物理 Flash 擦除或写入。
 */

#include "app_test.h"
#include "bsp_includes.h"

extern volatile uint32_t g_ll_recover_count;
extern volatile int32_t  g_ll_last_error;

/** @brief 总线契约检查失败位图（bit0~bit19 对应 20 项检查，0 表示全部通过） */
static uint32_t s_ct_fail;

/**
 * @brief AppBusTest 结果导出（供 SWD 读取做 HIL 验收）
 *        magic = 0x54534554 ('TEST') 表示结果已写入。
 */
typedef struct {
    uint32_t magic;
    uint32_t ct_pass, ct_total;
    uint32_t ct_fail;          /* s_ct_fail 位图 */
    uint32_t rounds_ok, rounds_total;
    uint32_t len_ok, len_count;
    uint32_t str_ok, str_total;
    uint32_t fl_ok, fl_total;
    uint32_t fl_last_result;     /* 失败时的 W25QXX_Result */
    uint32_t fl_fail_round;      /* 失败所在轮次 */
    uint32_t erase_count;        /* 实际物理扇区擦除次数（安全模式下严格为 0） */
    uint32_t rec_ok;
    uint32_t ll_recover_count;   /* LL 层恢复调用次数 */
    int32_t  ll_last_error;      /* LL 层最后一次错误码 */
    uint32_t owner_null;       /* 结束时 owner == NULL */
    uint32_t depth_zero;       /* 结束时 depth == 0 */
    uint32_t pass;             /* 总体 PASS */
    uint32_t elapsed_ms;
    /* Stage 1.2 安全与诊断字段 */
    uint32_t test_mode;        /* 0: SAFE/READ-ONLY, 1: DESTRUCTIVE */
    uint32_t test_sector_addr; /* 经确认的测试扇区物理地址 */
    uint32_t rounds_fail_stage;
    uint32_t rounds_fail_tx0;
    uint32_t rounds_fail_rx0;
} app_test_result_t;

volatile app_test_result_t g_app_test_result;

/**
 * @brief GC9A01 上电显示自测
 */
void AppTFT_Test(void)
{
    uint16_t bars[6] = { RED, GREEN, BLUE, YELLOW, MAGENTA, CYAN };
    uint8_t i;

    /* 1. 纯色刷屏（验证各基色与显存连续写） */
    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, RED);
    Delay_ms(300);
    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, GREEN);
    Delay_ms(300);
    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLUE);
    Delay_ms(300);
    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLACK);

    /* 2. 顶部 6 条色带 */
    for (i = 0; i < 6; i++)
    {
        LCD_Fill(&g_lcd, i * 40, 0, (i + 1) * 40, 40, bars[i]);
    }

    /* 3. 文字信息（不同字号） */
    LCD_ShowString(&g_lcd, 20, 60, "GC9A01 OK", WHITE, BLACK, 24, 0);
    LCD_ShowString(&g_lcd, 20, 100, "240x240 SPI1", CYAN, BLACK, 16, 0);
    LCD_ShowString(&g_lcd, 20, 120, "CS:PA9 RST:PA10", YELLOW, BLACK, 16, 0);
    LCD_ShowString(&g_lcd, 20, 140, "DC:PB13", GREEN, BLACK, 16, 0);
    LCD_ShowIntNum(&g_lcd, 20, 170, 2026, 4, LGRAY, BLACK, 16);

    /* 4. 边框与圆 */
    LCD_DrawRectangle(&g_lcd, 2, 2, 237, 237, WHITE);
    Draw_Circle(&g_lcd, 120, 205, 18, MAGENTA);

    /* 5. 清空心跳区 */
    LCD_Fill(&g_lcd, 108, 222, 132, 234, BLACK);
}

/**
 * @brief GC9A01 周期任务：右下角心跳方块每 500ms 翻转一次
 */
void AppTFT_Task(void)
{
    static uint32_t last_tick = 0;
    static uint8_t on = 0;
    uint32_t now = GetTick_ms();

    if (now - last_tick >= 500)
    {
        last_tick = now;
        on = !on;
        LCD_Fill(&g_lcd, 108, 222, 132, 234, on ? RED : BLACK);
    }
}

/**
 * @brief 双设备共享总线测试：TFT 与 W25QXX 交替占用 SPI1
 * @note  默认安全模式：仅执行非破坏性只读测试，绝对不擦除/改写 Flash 数据！
 */
void AppFlash_Test(void)
{
    #define FLASH_TEST_LEN    256U
    #define FLASH_TEST_ROUNDS 3U

#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
    static uint8_t tx[FLASH_TEST_LEN];
#endif
    static uint8_t rx[FLASH_TEST_LEN];
    char line[40];
    uint32_t cap_mb = g_flash.capacity / (1024U * 1024U);
    uint32_t elapsed;
    uint32_t sum = 0;
    uint16_t i;
    uint8_t r;
    uint8_t rounds_ok = 0;

    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLACK);

    elapsed = GetTick_ms();

#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
    /* ---- 破坏性测试模式（显式启用）：在经确认的安全扇区进行 3 轮写-读-校验 ---- */
    /* 严格禁止默认使用 0x000000，且必须检查扇区地址小于容量 */
    if (FLASH_SAFE_TEST_SECTOR_ADDR != 0U &&
        FLASH_SAFE_TEST_SECTOR_ADDR < g_flash.capacity &&
        (FLASH_SAFE_TEST_SECTOR_ADDR % W25QXX_SECTOR_SIZE) == 0U)
    {
        for (i = 0; i < FLASH_TEST_LEN; i++) {
            tx[i] = (uint8_t)(i ^ 0x5AU);
        }

        for (r = 0; r < FLASH_TEST_ROUNDS; r++)
        {
            tx[0] = r;
            memset(rx, 0, sizeof(rx));

            if (W25QXX_Write(&g_flash, tx, FLASH_SAFE_TEST_SECTOR_ADDR, FLASH_TEST_LEN) != W25QXX_OK) {
                break;
            }
            if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, FLASH_TEST_LEN) != W25QXX_OK) {
                break;
            }
            if (memcmp(rx, tx, FLASH_TEST_LEN) != 0) {
                break;
            }
            rounds_ok++;
            LCD_Fill(&g_lcd, 16 + r * 72, 172, 16 + r * 72 + 56, 216, GREEN);
        }
    }
#else
    /* ---- 默认安全模式：仅执行非破坏性只读验证，零擦除零写入 ---- */
    for (r = 0; r < FLASH_TEST_ROUNDS; r++)
    {
        memset(rx, 0, sizeof(rx));
        /* 采用非破坏性读操作读取 Flash 数据段（走 DMA） */
        if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, FLASH_TEST_LEN) != W25QXX_OK)
        {
            break;
        }
        rounds_ok++;

        /* TFT 与 Flash 共用 SPI1，交替占用总线刷块（验证总线仲裁） */
        LCD_Fill(&g_lcd, 16 + r * 72, 172, 16 + r * 72 + 56, 216, GREEN);
    }
#endif

    elapsed = GetTick_ms() - elapsed;

    for (i = 0; i < FLASH_TEST_LEN; i++)
    {
        sum += rx[i];
    }

    /* 3. 结果上屏 */
#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
    LCD_ShowString(&g_lcd, 12, 12, "TFT+FLASH WRITE TEST", CYAN, BLACK, 16, 0);
#else
    LCD_ShowString(&g_lcd, 12, 12, "TFT+FLASH SAFE MODE", GREEN, BLACK, 16, 0);
#endif

    snprintf(line, sizeof(line), "ID:%04X CAP:%luMB",
             g_flash.id, (unsigned long)cap_mb);
    LCD_ShowString(&g_lcd, 12, 40, line, WHITE, BLACK, 16, 0);

#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
    snprintf(line, sizeof(line), "MODE:DESTRUCT SEC:%05lX", (unsigned long)FLASH_SAFE_TEST_SECTOR_ADDR);
    LCD_ShowString(&g_lcd, 12, 64, line, RED, BLACK, 16, 0);
#else
    LCD_ShowString(&g_lcd, 12, 64, "MODE:READ-ONLY (ERASE:0)", LGRAY, BLACK, 16, 0);
#endif

    snprintf(line, sizeof(line), "%uB x%u  R:%u/%u",
             (unsigned)FLASH_TEST_LEN, (unsigned)FLASH_TEST_ROUNDS,
             (unsigned)rounds_ok, (unsigned)FLASH_TEST_ROUNDS);
    LCD_ShowString(&g_lcd, 12, 88, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "%s SUM:%04X %lums",
             (rounds_ok == FLASH_TEST_ROUNDS) ? "PASS" : "FAIL",
             (unsigned)sum, (unsigned long)elapsed);
    LCD_ShowString(&g_lcd, 12, 112,
                   line,
                   (rounds_ok == FLASH_TEST_ROUNDS) ? GREEN : RED,
                   BLACK, 16, 0);

    snprintf(line, sizeof(line), "RX[0]=%02X RX[1]=%02X", rx[0], rx[1]);
    LCD_ShowString(&g_lcd, 12, 136, line, YELLOW, BLACK, 16, 0);

    LCD_DrawRectangle(&g_lcd, 2, 2, 237, 237, WHITE);
}

/**
 * @brief 总线并发与可靠性测试：契约检查 + 高优先级中断抢占压力测试
 * @note  Stage 1.2: 默认安全模式下不执行物理擦除与写入，全量契约与抢占保护仍然有效
 */
void AppBusTest(void)
{
    #define BUS_TEST_ROUNDS 10U
    #define BUS_TEST_LEN    256U
    #define BUS_TEST_BUF    480U
    #define STRESS_TXN      1000U
    #define FLASH_ROUNDS    10U

    static uint8_t tx[BUS_TEST_BUF];
    static uint8_t rx[BUS_TEST_BUF];
    static uint8_t tag_a, tag_b;
    static const spi_bus_config_t cfg_a = { 18000000U, SPI_BUS_MODE_0, 1 };
    static const spi_bus_config_t cfg_b = {  9000000U, SPI_BUS_MODE_0, 1 };

    static const uint16_t len_list[] = {1,4,8,15,16,17,32,64,256,480};
    const uint8_t len_count = (uint8_t)(sizeof(len_list)/sizeof(len_list[0]));

    spi_bus_t *bus = BSP_SPI1_GetBus();
    bsp_spi_probe_t probe;
    char line[40];
    uint32_t elapsed;
    uint32_t sum = 0;
    uint16_t i;
    uint8_t r;
    uint8_t rounds_ok = 0;
    uint8_t ct_total = 0;
    uint8_t ct_pass = 0;
    const void *owner_after;
    uint16_t depth_after;
    uint8_t pass;

    uint8_t len_ok = 0;
    uint16_t str_ok = 0;
    uint16_t fl_ok = 0;
    uint8_t rec_ok = 0;
    uint32_t erase_count = 0;

    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLACK);

    /* ---------------- 1. 契约检查（纯状态机断言，与物理 Flash 擦写无关） ---------------- */
    BSP_SPI1_PreemptProbe_Init();
    s_ct_fail = 0U;

    /* bit0: 未占用时不允许传输 */
    ct_total++;
    if (spi_bus_transfer(bus, &tag_a, tx, NULL, 4) != 0) ct_pass++; else s_ct_fail |= (1u << 0);

    /* bit1: depth==0 时 release 应失败 */
    ct_total++;
    if (spi_bus_release(bus, &tag_a) != 0) ct_pass++; else s_ct_fail |= (1u << 1);

    /* bit2: A 可以占用 */
    ct_total++;
    if (spi_bus_acquire(bus, &tag_a, &cfg_a) == 0) ct_pass++; else s_ct_fail |= (1u << 2);

    /* bit3: B 试图抢占 A 应被拒绝 */
    ct_total++;
    if (spi_bus_acquire(bus, &tag_b, &cfg_b) != 0) ct_pass++; else s_ct_fail |= (1u << 3);

    /* bit4: 主循环持总线时，手动注入一次高优先级中断：ISR 必须被拒绝 */
    BSP_SPI1_PreemptProbe_Inject();
    BSP_SPI1_PreemptProbe_Get(&probe);
    ct_total++;
    if (probe.probes == 1U && probe.saw_busy == 1U && probe.granted == 0U) ct_pass++; else s_ct_fail |= (1u << 4);

    /* bit5: 同一 owner 相同配置可嵌套（不重配） */
    ct_total++;
    if (spi_bus_acquire(bus, &tag_a, &cfg_a) == 0) ct_pass++; else s_ct_fail |= (1u << 5);

    /* bit6: 嵌套第 1 次释放返回 0（CS 保持） */
    ct_total++;
    if (spi_bus_release(bus, &tag_a) == 0) ct_pass++; else s_ct_fail |= (1u << 6);

    /* bit7: 占用期间不允许外部改配置 */
    ct_total++;
    if (spi_bus_configure(bus, &cfg_b) != 0) ct_pass++; else s_ct_fail |= (1u << 7);

    /* bit8: 嵌套第 2 次释放返回 1（完全交还） */
    ct_total++;
    if (spi_bus_release(bus, &tag_a) == 1) ct_pass++; else s_ct_fail |= (1u << 8);

    /* bit9: 完全释放后 owner 应为空 */
    ct_total++;
    if (spi_bus_owner(bus) == NULL) ct_pass++; else s_ct_fail |= (1u << 9);

    /* bit10: NULL owner acquire 被拒绝 */
    ct_total++;
    if (spi_bus_acquire(bus, NULL, &cfg_a) != 0) ct_pass++; else s_ct_fail |= (1u << 10);

    /* bit11: NULL owner release 被拒绝 */
    ct_total++;
    if (spi_bus_release(bus, NULL) != 0) ct_pass++; else s_ct_fail |= (1u << 11);

    /* A 占用，验证非 owner 行为 */
    (void)spi_bus_acquire(bus, &tag_a, &cfg_a);

    /* bit12: 非当前 owner 的 transfer 被拒绝 */
    ct_total++;
    if (spi_bus_transfer(bus, &tag_b, tx, NULL, 4) != 0) ct_pass++; else s_ct_fail |= (1u << 12);

    /* bit13: 非当前 owner 的 release 被拒绝 */
    ct_total++;
    if (spi_bus_release(bus, &tag_b) != 0) ct_pass++; else s_ct_fail |= (1u << 13);

    /* bit17: 同 owner 不同配置嵌套被拒绝，且状态不变 */
    ct_total++;
    {
        uint16_t d0 = spi_bus_depth(bus);
        int rc = spi_bus_acquire(bus, &tag_a, &cfg_b);
        if (rc != 0 && spi_bus_owner(bus) == &tag_a && spi_bus_depth(bus) == d0)
            ct_pass++;
        else
            s_ct_fail |= (1u << 17);
    }

    /* bit18: 嵌套期间配置不发生改变 —— owner 仍为 A 且仍可正常 transfer */
    ct_total++;
    {
        uint8_t t = 0xA5U, r_b = 0U;
        int rc = spi_bus_transfer(bus, &tag_a, &t, &r_b, 1U);
        if (rc == 0) ct_pass++; else s_ct_fail |= (1u << 18);
    }

    /* 释放 A */
    (void)spi_bus_release(bus, &tag_a);

    /* bit14: depth 溢出保护 —— UINT16_MAX 饱和 */
    ct_total++;
    {
        uint32_t k;
        (void)spi_bus_acquire(bus, &tag_a, &cfg_a);
        for (k = 1U; k < (uint32_t)UINT16_MAX; k++) {
            (void)spi_bus_acquire(bus, &tag_a, &cfg_a);
        }
        int rc = spi_bus_acquire(bus, &tag_a, &cfg_a);
        if (rc != 0 && spi_bus_depth(bus) == UINT16_MAX)
            ct_pass++;
        else
            s_ct_fail |= (1u << 14);

        for (k = 0; k < (uint32_t)UINT16_MAX; k++) {
            (void)spi_bus_release(bus, &tag_a);
        }
    }

    /* bit19: 所有 release 完成后 owner=NULL 且 depth=0 */
    ct_total++;
    if (spi_bus_owner(bus) == NULL && spi_bus_depth(bus) == 0U) ct_pass++; else s_ct_fail |= (1u << 19);

    /* ---------------- 2. 抢占压力测试（20kHz 高频中断探针） ---------------- */
    BSP_SPI1_PreemptProbe_Start(20000U);
    elapsed = GetTick_ms();

    for (r = 0; r < BUS_TEST_ROUNDS; r++)
    {
        memset(rx, 0, sizeof(rx));

#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
        for (i = 0; i < BUS_TEST_LEN; i++) {
            tx[i] = (uint8_t)((i ^ 0x5AU) + r);
        }
        if (W25QXX_Write(&g_flash, tx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_LEN) != W25QXX_OK) {
            g_app_test_result.rounds_fail_stage = 1;
            break;
        }
        erase_count++;
        if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_LEN) != W25QXX_OK) {
            g_app_test_result.rounds_fail_stage = 2;
            break;
        }
        if (memcmp(rx, tx, BUS_TEST_LEN) != 0) {
            g_app_test_result.rounds_fail_stage = 3;
            break;
        }
#else
        /* 安全模式：纯非破坏性读取，在 20kHz 中断压力下验证总线仲裁 */
        if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_LEN) != W25QXX_OK) {
            g_app_test_result.rounds_fail_stage = 2;
            break;
        }
#endif
        rounds_ok++;
        LCD_Fill(&g_lcd, 10 + r * 22, 200, 10 + r * 22 + 16, 216, GREEN);
    }
    BSP_SPI1_PreemptProbe_Stop();
    BSP_SPI1_PreemptProbe_Get(&probe);

    /* ---------------- 3. 传输长度矩阵（1~480 B，覆盖轮询与 DMA 切换） ---------------- */
    {
#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
        for (i = 0; i < BUS_TEST_BUF; i++) {
            tx[i] = (uint8_t)(i ^ 0xA5U);
        }
        if (W25QXX_Write(&g_flash, tx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_BUF) == W25QXX_OK) {
            erase_count++;
            for (uint8_t li = 0; li < len_count; li++) {
                uint16_t L = len_list[li];
                memset(rx, 0, L);
                if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, L) == W25QXX_OK &&
                    memcmp(rx, tx, L) == 0) {
                    len_ok++;
                }
            }
        }
#else
        /* 安全模式：对各长度执行纯读取测试，验证轮询与 DMA 切换无故障 */
        for (uint8_t li = 0; li < len_count; li++) {
            uint16_t L = len_list[li];
            memset(rx, 0, L);
            if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, L) == W25QXX_OK) {
                len_ok++;
            }
        }
#endif
    }

    /* ---------------- 4. 1000 次 SPI 事务压力（纯读操作，零擦除） ---------------- */
    {
        uint16_t k;
        for (k = 0; k < STRESS_TXN; k++) {
            uint8_t b = 0U;
            if (W25QXX_Read(&g_flash, &b, FLASH_SAFE_TEST_SECTOR_ADDR, 1U) == W25QXX_OK) {
                str_ok++;
            }
        }
    }

    /* ---------------- 5. Flash 擦写读轮次（安全模式下跳过擦写） ---------------- */
#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
    {
        uint16_t k;
        W25QXX_Result wr = W25QXX_OK, rr = W25QXX_OK;
        for (k = 0; k < FLASH_ROUNDS; k++) {
            for (i = 0; i < BUS_TEST_LEN; i++) {
                tx[i] = (uint8_t)((i ^ 0x3CU) + k);
            }
            memset(rx, 0x55, BUS_TEST_LEN);
            wr = W25QXX_Write(&g_flash, tx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_LEN);
            if (wr != W25QXX_OK) { g_app_test_result.fl_fail_round = k; g_app_test_result.fl_last_result = (uint32_t)wr; break; }
            erase_count++;
            rr = W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_LEN);
            if (rr != W25QXX_OK) { g_app_test_result.fl_fail_round = k; g_app_test_result.fl_last_result = (uint32_t)rr; break; }
            if (memcmp(rx, tx, BUS_TEST_LEN) != 0) {
                g_app_test_result.fl_fail_round = k;
                break;
            }
            fl_ok++;
        }
    }
#else
    fl_ok = 0;
#endif

    /* ---------------- 6. 统一异常恢复测试（调用 BSP_SPI1_Recover 并验证通信正常） ---------------- */
    {
        if (BSP_SPI1_Recover() == 0) {
            memset(rx, 0, BUS_TEST_LEN);
            if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_LEN) == W25QXX_OK) {
                rec_ok = 1;
            }
        }
    }

    elapsed = GetTick_ms() - elapsed;

    for (i = 0; i < BUS_TEST_LEN; i++) {
        sum += rx[i];
    }

    owner_after = spi_bus_owner(bus);
    depth_after = bus->depth;

#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
    pass = (ct_pass == ct_total) &&
           (rounds_ok == BUS_TEST_ROUNDS) &&
           (len_ok == len_count) &&
           (str_ok == STRESS_TXN) &&
           (fl_ok == FLASH_ROUNDS) &&
           (rec_ok == 1U) &&
           (probe.granted == 0U) &&
           (owner_after == NULL) &&
           (depth_after == 0U);
#else
    pass = (ct_pass == ct_total) &&
           (rounds_ok == BUS_TEST_ROUNDS) &&
           (len_ok == len_count) &&
           (str_ok == STRESS_TXN) &&
           (rec_ok == 1U) &&
           (probe.granted == 0U) &&
           (owner_after == NULL) &&
           (depth_after == 0U) &&
           (erase_count == 0U);
#endif

    /* ---------------- 7. 导出结构体与上屏 ---------------- */
    g_app_test_result.magic            = 0x54534554U;   /* 'TEST' */
    g_app_test_result.ct_pass          = ct_pass;
    g_app_test_result.ct_total         = ct_total;
    g_app_test_result.ct_fail          = s_ct_fail;
    g_app_test_result.rounds_ok        = rounds_ok;
    g_app_test_result.rounds_total     = BUS_TEST_ROUNDS;
    g_app_test_result.len_ok           = len_ok;
    g_app_test_result.len_count        = len_count;
    g_app_test_result.str_ok           = str_ok;
    g_app_test_result.str_total        = STRESS_TXN;
    g_app_test_result.fl_ok            = fl_ok;
    g_app_test_result.fl_total         = (CONFIG_FLASH_DESTRUCTIVE_TEST != 0) ? FLASH_ROUNDS : 0U;
    g_app_test_result.erase_count      = erase_count;
    g_app_test_result.rec_ok           = rec_ok;
    g_app_test_result.ll_recover_count = g_ll_recover_count;
    g_app_test_result.ll_last_error    = g_ll_last_error;
    g_app_test_result.owner_null       = (owner_after == NULL) ? 1U : 0U;
    g_app_test_result.depth_zero       = (depth_after == 0U) ? 1U : 0U;
    g_app_test_result.pass             = pass ? 1U : 0U;
    g_app_test_result.elapsed_ms       = elapsed;
    g_app_test_result.test_mode        = CONFIG_FLASH_DESTRUCTIVE_TEST;
    g_app_test_result.test_sector_addr = FLASH_SAFE_TEST_SECTOR_ADDR;

    LCD_ShowString(&g_lcd, 12, 12, "BUS RELIABILITY TEST", CYAN, BLACK, 16, 0);

    snprintf(line, sizeof(line), "CT:%u/%u G:%lu E:%05X",
             (unsigned)ct_pass, (unsigned)ct_total,
             (unsigned long)probe.granted, (unsigned)s_ct_fail);
    LCD_ShowString(&g_lcd, 12, 34, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "IRQ:%lu HIT:%lu",
             (unsigned long)probe.probes, (unsigned long)probe.saw_busy);
    LCD_ShowString(&g_lcd, 12, 56, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "LEN:%u/%u  STR:%u/%u",
             (unsigned)len_ok, (unsigned)len_count,
             (unsigned)str_ok, (unsigned)STRESS_TXN);
    LCD_ShowString(&g_lcd, 12, 80, line, WHITE, BLACK, 16, 0);

#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
    snprintf(line, sizeof(line), "FL:%u/%u EC:%lu",
             (unsigned)fl_ok, (unsigned)FLASH_ROUNDS, (unsigned long)erase_count);
#else
    snprintf(line, sizeof(line), "MODE:SAFE (ERASE:0)");
#endif
    LCD_ShowString(&g_lcd, 12, 104, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "REC:%s", rec_ok ? "OK" : "FAIL");
    LCD_ShowString(&g_lcd, 12, 128, line, rec_ok ? GREEN : RED, BLACK, 16, 0);

    snprintf(line, sizeof(line), "OWNER:%s DEPTH:%u",
             (owner_after == NULL) ? "NULL" : "LEFT", (unsigned)depth_after);
    LCD_ShowString(&g_lcd, 12, 152, line,
                   (owner_after == NULL && depth_after == 0U) ? WHITE : RED,
                   BLACK, 16, 0);

    snprintf(line, sizeof(line), "%s %lums", pass ? "PASS" : "FAIL",
             (unsigned long)elapsed);
    LCD_ShowString(&g_lcd, 12, 176, line, pass ? GREEN : RED, BLACK, 16, 0);

    LCD_DrawRectangle(&g_lcd, 2, 2, 237, 237, WHITE);
}
