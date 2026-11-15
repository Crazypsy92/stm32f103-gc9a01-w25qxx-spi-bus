/**
 * @file app_test.c
 * @brief 应用层显示测试模块实现
 * @details GC9A01 上电自测：依次刷红/绿/蓝确认数据链路，再画色条、文字、
 *          边框与圆；随后由主循环调用 AppTFT_Task() 做心跳闪烁指示。
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
    uint32_t erase_count;
    uint32_t rec_ok;
    uint32_t ll_recover_count;   /* LL 层恢复调用次数 */
    int32_t  ll_last_error;      /* LL 层最后一次错误码 */
    uint32_t owner_null;       /* 结束时 owner == NULL */
    uint32_t depth_zero;       /* 结束时 depth == 0 */
    uint32_t pass;             /* 总体 PASS */
    uint32_t elapsed_ms;
    /* 调试字段放末尾 */
    uint32_t rounds_fail_stage;
    uint32_t rounds_fail_tx0;
    uint32_t rounds_fail_rx0;
} app_test_result_t;

volatile app_test_result_t g_app_test_result;

/**
 * @brief GC9A01 上电显示自测
 * @note  文字位置按 240x240 竖屏排列，肉眼即可确认 SPI 通信与显存写入正常
 */
void AppTFT_Test(void)
{
    const uint16_t bars[6] = {RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA};
    uint8_t i;

    /* 1. 全屏换色：确认像素写入与色彩顺序 */
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
 * @brief 双 DMA 共享总线测试：TFT 与 W25QXX 都走 DMA，交替占用 SPI1
 * @note  1) Flash 侧 256B 写/读，长度超过总线阈值（16B），走 DMA1_Ch3(发)/Ch2(收)
 *        2) 每轮之间用 TFT 刷色块，TFT 同样走 DMA1_Ch3
 *        3) 两设备靠 spi_bus_acquire/release 串行化，DMA 传输是阻塞等待完成的，
 *           因此共用 DMA1 不会出现通道被交叉改写的问题
 */
void AppFlash_Test(void)
{
    #define FLASH_TEST_LEN    256U
    #define FLASH_TEST_ROUNDS 3U

    static uint8_t tx[FLASH_TEST_LEN];
    static uint8_t rx[FLASH_TEST_LEN];
    char line[40];
    uint32_t cap_mb = g_flash.capacity / (1024U * 1024U);
    uint32_t elapsed;
    uint32_t sum = 0;
    uint16_t i;
    uint8_t r;
    uint8_t rounds_ok = 0;

    /* 1. 准备 0..255 的测试图案（每轮首字节不同，避免读到上轮残留） */
    for (i = 0; i < FLASH_TEST_LEN; i++)
    {
        tx[i] = (uint8_t)(i ^ 0x5AU);
    }

    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLACK);

    /* 2. 三轮 Flash 写-读-校验，每轮后插入 TFT DMA 刷块 */
    elapsed = GetTick_ms();
    for (r = 0; r < FLASH_TEST_ROUNDS; r++)
    {
        tx[0] = r;
        memset(rx, 0, sizeof(rx));

        if (W25QXX_Write(&g_flash, tx, 0x0000U, FLASH_TEST_LEN) != W25QXX_OK)
        {
            break;
        }
        W25QXX_Read(&g_flash, rx, 0x0000U, FLASH_TEST_LEN);
        if (memcmp(rx, tx, FLASH_TEST_LEN) != 0)
        {
            break;
        }
        rounds_ok++;

        /* TFT 与 Flash 共用 SPI1，这里紧接着占用总线刷一块（TFT 也走 DMA） */
        LCD_Fill(&g_lcd, 16 + r * 72, 172, 16 + r * 72 + 56, 216, GREEN);
    }
    elapsed = GetTick_ms() - elapsed;

    for (i = 0; i < FLASH_TEST_LEN; i++)
    {
        sum += rx[i];
    }

    /* 3. 结果上屏 */
    LCD_ShowString(&g_lcd, 12, 12, "TFT+FLASH DMA TEST", CYAN, BLACK, 16, 0);

    snprintf(line, sizeof(line), "ID:%04X CAP:%luMB",
             g_flash.id, (unsigned long)cap_mb);
    LCD_ShowString(&g_lcd, 12, 40, line, WHITE, BLACK, 16, 0);

    LCD_ShowString(&g_lcd, 12, 64, "TFT:DMA FLASH:DMA", LGRAY, BLACK, 16, 0);

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
 * @brief 总线并发测试：契约检查 + 高优先级中断抢占压力测试
 * @note  1) 契约：未占用不能传/不能释放、他设备不能抢占、同设备可重入、
 *           重复释放被拒、ISR 上下文永远拿不到总线
 *        2) 压力：TIM2(20kHz, 优先级 0) 不断尝试占用 SPI1，同时主循环做
 *           10 轮 Flash 写-读-校验 + TFT 刷块，校验数据必须完整
 *        3) 结束后要求 owner=NULL、depth=0，证明没有泄漏
 */
void AppBusTest(void)
{
    #define BUS_TEST_ROUNDS 10U
    #define BUS_TEST_LEN    256U
    #define BUS_TEST_BUF    480U       /* 长度矩阵最大 480 B */
    #define STRESS_TXN      1000U
    #define FLASH_ROUNDS    10U
    #define FLASH_TEST_BASE 0x00100000U /* 预留测试扇区，1 MB 处，远离 0x0000 */

    static uint8_t tx[BUS_TEST_BUF];
    static uint8_t rx[BUS_TEST_BUF];
    static uint8_t tag_a, tag_b;                  /* 伪造的"设备上下文"标识 */
    static const spi_bus_config_t cfg_a = { 18000000U, SPI_BUS_MODE_0, 1 };
    static const spi_bus_config_t cfg_b = {  9000000U, SPI_BUS_MODE_0, 1 };

    /* 长度矩阵 */
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

    /* 新增测试统计 */
    uint8_t len_ok = 0;
    uint16_t str_ok = 0;
    uint16_t fl_ok = 0;
    uint8_t rec_ok = 0;
    uint32_t erase_count = 0;

    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLACK);

    /* ---------------- 1. 契约检查（确定性，与时间无关） ---------------- */
    BSP_SPI1_PreemptProbe_Init();
    s_ct_fail = 0U;

    /* bit0: 未占用时不允许传输（owner 非当前占用者） */
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

    /* bit6: 第一次 release 仍为嵌套（返回 0），CS 不应拉高 */
    ct_total++;
    if (spi_bus_release(bus, &tag_a) == 0) ct_pass++; else s_ct_fail |= (1u << 6);

    /* bit7: 第二次 release 真正释放（返回 >0） */
    ct_total++;
    if (spi_bus_release(bus, &tag_a) > 0) ct_pass++; else s_ct_fail |= (1u << 7);

    /* bit8: 多释放一次应失败 */
    ct_total++;
    if (spi_bus_release(bus, &tag_a) != 0) ct_pass++; else s_ct_fail |= (1u << 8);

    /* bit9: 完全释放后 owner 应为空 */
    ct_total++;
    if (spi_bus_owner(bus) == NULL) ct_pass++; else s_ct_fail |= (1u << 9);

    /* ---- 新增：NULL / 非 owner / 嵌套配置一致性 ---- */

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

    /* bit17: 同 owner 不同配置嵌套被拒绝，且状态不变（owner 仍为 A、depth 仍为 1） */
    ct_total++;
    {
        uint16_t d0 = spi_bus_depth(bus);
        int rc = spi_bus_acquire(bus, &tag_a, &cfg_b);   /* cfg_b 与 cfg_a 速率不同 */
        if (rc != 0 && spi_bus_owner(bus) == &tag_a && spi_bus_depth(bus) == d0)
            ct_pass++;
        else
            s_ct_fail |= (1u << 17);
    }

    /* bit18: 嵌套期间配置不发生改变 —— owner 仍为 A 且仍可正常 transfer */
    ct_total++;
    {
        uint8_t t = 0xA5U, r = 0U;
        int rc = spi_bus_transfer(bus, &tag_a, &t, &r, 1U);
        if (rc == 0) ct_pass++; else s_ct_fail |= (1u << 18);
    }

    /* 释放 A */
    (void)spi_bus_release(bus, &tag_a);

    /* bit14: depth 溢出保护 —— 把 depth 推到 UINT16_MAX 后再 acquire 应失败，状态不变 */
    ct_total++;
    {
        uint32_t k;
        /* 先占用一次，再嵌套填满到 UINT16_MAX */
        (void)spi_bus_acquire(bus, &tag_a, &cfg_a);   /* depth = 1 */
        for (k = 1U; k < (uint32_t)UINT16_MAX; k++) {
            (void)spi_bus_acquire(bus, &tag_a, &cfg_a);   /* 同 owner 同配置，仅 depth++ */
        }
        /* depth 应已达 UINT16_MAX */
        int rc = spi_bus_acquire(bus, &tag_a, &cfg_a);
        if (rc != 0 && spi_bus_depth(bus) == UINT16_MAX)
            ct_pass++;
        else
            s_ct_fail |= (1u << 14);
        /* 全部释放，确认总线归还 */
        for (k = 0; k < (uint32_t)UINT16_MAX; k++) {
            (void)spi_bus_release(bus, &tag_a);
        }
    }

    /* bit19: 所有 release 完成后 owner=NULL 且 depth=0 */
    ct_total++;
    if (spi_bus_owner(bus) == NULL && spi_bus_depth(bus) == 0U) ct_pass++; else s_ct_fail |= (1u << 19);

    /* ---------------- 2. 抢占压力测试（10 轮 Flash + TFT，20kHz 中断） ---------------- */
    BSP_SPI1_PreemptProbe_Start(20000U);   /* 20kHz，高于 SysTick */

    elapsed = GetTick_ms();
    for (r = 0; r < BUS_TEST_ROUNDS; r++)
    {
        for (i = 0; i < BUS_TEST_LEN; i++)
        {
            tx[i] = (uint8_t)((i ^ 0x5AU) + r);
        }
        memset(rx, 0, sizeof(rx));

        if (W25QXX_Write(&g_flash, tx, 0x0000U, BUS_TEST_LEN) != W25QXX_OK)
        {
            g_app_test_result.rounds_fail_stage = 1;
            break;
        }
        if (W25QXX_Read(&g_flash, rx, 0x0000U, BUS_TEST_LEN) != W25QXX_OK)
        {
            g_app_test_result.rounds_fail_stage = 2;
            break;
        }
        if (memcmp(rx, tx, BUS_TEST_LEN) != 0)
        {
            uint16_t m;
            for (m = 0; m < BUS_TEST_LEN; m++) {
                if (rx[m] != tx[m]) break;
            }
            g_app_test_result.rounds_fail_stage = 3;
            g_app_test_result.rounds_fail_tx0 = (uint32_t)(m | (tx[m] << 16));
            g_app_test_result.rounds_fail_rx0 = (uint32_t)rx[m];
            break;
        }
        rounds_ok++;

        /* TFT 同时抢总线（DMA），与 Flash 交替 */
        LCD_Fill(&g_lcd, 10 + r * 22, 200, 10 + r * 22 + 16, 216, GREEN);
    }
    BSP_SPI1_PreemptProbe_Stop();
    BSP_SPI1_PreemptProbe_Get(&probe);

    /* ---------------- 3. 传输长度矩阵（1~480 B，TX+RX） ---------------- */
    {
        for (i = 0; i < BUS_TEST_BUF; i++) {
            tx[i] = (uint8_t)(i ^ 0xA5U);
        }
        /* 写一次 480 B 到预留扇区（1 次擦除），再按各长度读回校验前缀 */
        if (W25QXX_Write(&g_flash, tx, FLASH_TEST_BASE, BUS_TEST_BUF) == W25QXX_OK) {
            erase_count++;
            uint8_t li;
            for (li = 0; li < len_count; li++) {
                uint16_t L = len_list[li];
                memset(rx, 0, L);
                if (W25QXX_Read(&g_flash, rx, FLASH_TEST_BASE, L) == W25QXX_OK &&
                    memcmp(rx, tx, L) == 0) {
                    len_ok++;
                }
            }
        }
    }

    /* ---------------- 4. 1000 次 SPI 事务压力（1 B 读，含 DMA/轮询切换） ---------------- */
    {
        uint16_t k;
        for (k = 0; k < STRESS_TXN; k++) {
            uint8_t b = 0U;
            if (W25QXX_Read(&g_flash, &b, FLASH_TEST_BASE, 1U) == W25QXX_OK &&
                b == tx[0]) {
                str_ok++;
            }
        }
    }

    /* ---------------- 5. 100 轮 Flash 256 B 写读（预留扇区，记录擦除次数） ---------------- */
    {
        uint16_t k;
        W25QXX_Result wr = W25QXX_OK, rr = W25QXX_OK;
        for (k = 0; k < FLASH_ROUNDS; k++) {
            for (i = 0; i < BUS_TEST_LEN; i++) {
                tx[i] = (uint8_t)((i ^ 0x3CU) + k);
            }
            memset(rx, 0x55, BUS_TEST_LEN);
            wr = W25QXX_Write(&g_flash, tx, FLASH_TEST_BASE, BUS_TEST_LEN);
            if (wr != W25QXX_OK) { g_app_test_result.fl_fail_round = k; g_app_test_result.fl_last_result = (uint32_t)wr; break; }
            erase_count++;
            rr = W25QXX_Read(&g_flash, rx, FLASH_TEST_BASE, BUS_TEST_LEN);
            if (rr != W25QXX_OK) { g_app_test_result.fl_fail_round = k; g_app_test_result.fl_last_result = (uint32_t)rr; break; }
            if (memcmp(rx, tx, BUS_TEST_LEN) != 0) {
                uint16_t m;
                for (m = 0; m < BUS_TEST_LEN; m++) {
                    if (rx[m] != tx[m]) break;
                }
                g_app_test_result.fl_fail_round = k;
                g_app_test_result.fl_last_result = (uint32_t)(m | (rx[m] << 8) | (tx[m] << 16));
                break;
            }
            fl_ok++;
        }
    }

    /* ---------------- 6. 恢复后下一次传输必须成功 ---------------- */
    {
        if (BSP_SPI1_Recover() == 0) {
            /* 读回 0x0000 处第 10 轮（rounds_ok-1）的数据并校验 */
            uint8_t expect_r = (rounds_ok > 0U) ? (uint8_t)(rounds_ok - 1U) : 0U;
            memset(rx, 0, BUS_TEST_LEN);
            if (W25QXX_Read(&g_flash, rx, 0x0000U, BUS_TEST_LEN) == W25QXX_OK) {
                uint16_t m;
                for (m = 0; m < BUS_TEST_LEN; m++) {
                    if (rx[m] != (uint8_t)((m ^ 0x5AU) + expect_r)) break;
                }
                if (m == BUS_TEST_LEN) rec_ok = 1;
            }
        }
    }

    elapsed = GetTick_ms() - elapsed;

    for (i = 0; i < BUS_TEST_LEN; i++)
    {
        sum += rx[i];
    }

    owner_after = spi_bus_owner(bus);
    depth_after = bus->depth;

    pass = (ct_pass == ct_total) &&
           (rounds_ok == BUS_TEST_ROUNDS) &&
           (len_ok == len_count) &&
           (str_ok == STRESS_TXN) &&
           (fl_ok == FLASH_ROUNDS) &&
           (rec_ok == 1U) &&
           (probe.granted == 0U) &&
           (owner_after == NULL) &&
           (depth_after == 0U);

    /* ---------------- 7. 结果上屏 ---------------- */
    g_app_test_result.magic       = 0x54534554U;   /* 'TEST' */
    g_app_test_result.ct_pass     = ct_pass;
    g_app_test_result.ct_total    = ct_total;
    g_app_test_result.ct_fail     = s_ct_fail;
    g_app_test_result.rounds_ok   = rounds_ok;
    g_app_test_result.rounds_total = BUS_TEST_ROUNDS;
    g_app_test_result.len_ok      = len_ok;
    g_app_test_result.len_count   = len_count;
    g_app_test_result.str_ok      = str_ok;
    g_app_test_result.str_total   = STRESS_TXN;
    g_app_test_result.fl_ok       = fl_ok;
    g_app_test_result.fl_total    = FLASH_ROUNDS;
    g_app_test_result.erase_count = erase_count;
    g_app_test_result.rec_ok      = rec_ok;
    g_app_test_result.ll_recover_count = g_ll_recover_count;
    g_app_test_result.ll_last_error    = g_ll_last_error;
    g_app_test_result.owner_null  = (owner_after == NULL) ? 1U : 0U;
    g_app_test_result.depth_zero  = (depth_after == 0U) ? 1U : 0U;
    g_app_test_result.pass        = pass ? 1U : 0U;
    g_app_test_result.elapsed_ms  = elapsed;
    /* 调试字段赋值（保持结构体末尾对齐） */
    /* rounds_fail_stage 已在第 2 部分失败时记录 */

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

    snprintf(line, sizeof(line), "FL:%u/%u EC:%lu",
             (unsigned)fl_ok, (unsigned)FLASH_ROUNDS, (unsigned long)erase_count);
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
