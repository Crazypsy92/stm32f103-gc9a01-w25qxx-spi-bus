/**
 * @file app_test.c
 * @brief 应用层显示测试模块实现
 * @details GC9A01 上电自测：依次刷红/绿/蓝确认数据链路，再画色条、文字、
 *          边框与圆；随后由主循环调用 AppTFT_Task() 做心跳闪烁指示。
 */

#include "app_test.h"
#include "bsp_includes.h"

/** @brief 总线契约检查失败位图（bit0~bit9 对应 10 项检查，0 表示全部通过） */
static uint16_t s_ct_fail;

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

    static uint8_t tx[BUS_TEST_LEN];
    static uint8_t rx[BUS_TEST_LEN];
    static uint8_t tag_a, tag_b;                  /* 伪造的"设备上下文"标识 */
    static const spi_bus_config_t cfg_a = { 18000000U, SPI_BUS_MODE_0, 1 };
    static const spi_bus_config_t cfg_b = {  9000000U, SPI_BUS_MODE_0, 1 };

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

    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLACK);

    /* ---------------- 1. 契约检查（确定性，与时间无关） ---------------- */
    BSP_SPI1_PreemptProbe_Init();

    /* 未占用时不允许传输 */
    ct_total++;
    if (spi_bus_transfer(bus, tx, NULL, 4) != 0) ct_pass++; else s_ct_fail |= (1u << 0);

    /* 未占用时释放应失败 */
    ct_total++;
    if (spi_bus_release(bus, &tag_a) != 0) ct_pass++; else s_ct_fail |= (1u << 1);

    /* A 可以占用 */
    ct_total++;
    if (spi_bus_acquire(bus, &tag_a, &cfg_a) == 0) ct_pass++; else s_ct_fail |= (1u << 2);

    /* B 试图抢占 A 应被拒绝 */
    ct_total++;
    if (spi_bus_acquire(bus, &tag_b, &cfg_b) != 0) ct_pass++; else s_ct_fail |= (1u << 3);

    /* 主循环持总线时，手动注入一次高优先级中断：ISR 必须被拒绝 */
    BSP_SPI1_PreemptProbe_Inject();
    BSP_SPI1_PreemptProbe_Get(&probe);
    ct_total++;
    if (probe.probes == 1U && probe.saw_busy == 1U && probe.granted == 0U) ct_pass++; else s_ct_fail |= (1u << 4);

    /* 同一 owner 可重入，两次 release 才真正释放 */
    ct_total++;
    if (spi_bus_acquire(bus, &tag_a, &cfg_a) == 0) ct_pass++; else s_ct_fail |= (1u << 5);
    ct_total++;
    if (spi_bus_release(bus, &tag_a) == 0) ct_pass++; else s_ct_fail |= (1u << 6);
    ct_total++;
    if (spi_bus_release(bus, &tag_a) == 0) ct_pass++; else s_ct_fail |= (1u << 7);

    /* 多释放一次应失败 */
    ct_total++;
    if (spi_bus_release(bus, &tag_a) != 0) ct_pass++; else s_ct_fail |= (1u << 8);

    /* 完全释放后 owner 应为空 */
    ct_total++;
    if (spi_bus_owner(bus) == NULL) ct_pass++; else s_ct_fail |= (1u << 9);

    /* ---------------- 2. 抢占压力测试 ---------------- */
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
            break;
        }
        if (W25QXX_Read(&g_flash, rx, 0x0000U, BUS_TEST_LEN) != W25QXX_OK)
        {
            break;
        }
        if (memcmp(rx, tx, BUS_TEST_LEN) != 0)
        {
            break;
        }
        rounds_ok++;

        /* TFT 同时抢总线（DMA），与 Flash 交替 */
        LCD_Fill(&g_lcd, 10 + r * 22, 200, 10 + r * 22 + 16, 216, GREEN);
    }
    elapsed = GetTick_ms() - elapsed;

    BSP_SPI1_PreemptProbe_Stop();
    BSP_SPI1_PreemptProbe_Get(&probe);

    for (i = 0; i < BUS_TEST_LEN; i++)
    {
        sum += rx[i];
    }

    owner_after = spi_bus_owner(bus);
    depth_after = bus->depth;

    pass = (ct_pass == ct_total) &&
           (rounds_ok == BUS_TEST_ROUNDS) &&
           (probe.granted == 0U) &&
           (owner_after == NULL) &&
           (depth_after == 0U);

    /* ---------------- 3. 结果上屏 ---------------- */
    LCD_ShowString(&g_lcd, 12, 12, "BUS CONCURRENCY TEST", CYAN, BLACK, 16, 0);

    snprintf(line, sizeof(line), "CT:%u/%u G:%lu E:%04X",
             (unsigned)ct_pass, (unsigned)ct_total,
             (unsigned long)probe.granted, (unsigned)s_ct_fail);
    LCD_ShowString(&g_lcd, 12, 36, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "IRQ:%lu HIT:%lu",
             (unsigned long)probe.probes, (unsigned long)probe.saw_busy);
    LCD_ShowString(&g_lcd, 12, 60, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "W/R:%u/%u SUM:%04X",
             (unsigned)rounds_ok, (unsigned)BUS_TEST_ROUNDS, (unsigned)sum);
    LCD_ShowString(&g_lcd, 12, 84, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "OWNER:%s DEPTH:%u",
             (owner_after == NULL) ? "NULL" : "LEFT", (unsigned)depth_after);
    LCD_ShowString(&g_lcd, 12, 108, line,
                   (owner_after == NULL && depth_after == 0U) ? WHITE : RED,
                   BLACK, 16, 0);

    LCD_ShowString(&g_lcd, 12, 132, "TFT:DMA FLASH:DMA", LGRAY, BLACK, 16, 0);

    snprintf(line, sizeof(line), "%s %lums", pass ? "PASS" : "FAIL",
             (unsigned long)elapsed);
    LCD_ShowString(&g_lcd, 12, 156, line, pass ? GREEN : RED, BLACK, 16, 0);

    LCD_DrawRectangle(&g_lcd, 2, 2, 237, 237, WHITE);
}
