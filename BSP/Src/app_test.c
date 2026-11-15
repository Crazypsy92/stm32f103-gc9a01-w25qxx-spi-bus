/**
 * @file app_test.c
 * @brief 应用层显示与总线测试实现
 * @details GC9A01 上电自测、W25QXX 读写校验、总线接口契约检查与抢占压力测试。
 *          Stage 1.2-B: 增强非破坏性读取数据完整性校验，区分通信成功与数据一致性统计。
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
    /* Stage 1.2-B: 数据一致性校验扩展统计 */
    uint32_t read_ok;               /* 读取接口成功次数 */
    uint32_t data_match_ok;        /* 数据重复读取一致次数 */
    uint32_t data_mismatch;        /* 数据不一致次数 */
    uint32_t first_mismatch_offset;/* 首个不一致字节偏移 */
    int32_t  last_flash_error;     /* 最后一次 Flash 错误码 */
    /* Stage 1.2-C 扩展字段 */
    uint32_t reference_valid;       /* 预读取基准有效性: bit0=ref_256, bit1=ref_buf, 0x03=全部有效 */
    uint32_t expected_256_valid;    /* 预期恢复基准是否有效（写入失败时置 0） */
} app_test_result_t;

volatile app_test_result_t g_app_test_result;

/**
 * @brief GC9A01 上电显示自测
 */
void AppTFT_Test(void)
{
    /* 圆形屏引导页：不占用额外帧缓冲，RGB 色块同时检验颜色通路。 */
    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLACK);
    Draw_Circle(&g_lcd, 120, 120, 113, DARKBLUE);
    LCD_ShowString(&g_lcd, 68, 34, "SPI1 LAB", CYAN, BLACK, 24, 0);
    LCD_ShowString(&g_lcd, 60, 84, "STM32F103C8", WHITE, BLACK, 16, 0);
    LCD_ShowString(&g_lcd, 52, 109, "GC9A01 W25Q128", LGRAY, BLACK, 16, 0);
    LCD_ShowString(&g_lcd, 70, 136, "DMA + FLASH", WHITE, BLACK, 16, 0);
    LCD_Fill(&g_lcd, 75, 166, 105, 173, RED);
    LCD_Fill(&g_lcd, 105, 166, 135, 173, GREEN);
    LCD_Fill(&g_lcd, 135, 166, 165, 173, BLUE);
    LCD_ShowString(&g_lcd, 80, 187, "SELF TEST", GREEN, BLACK, 16, 0);
    Delay_ms(350);
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
 * @note  默认安全模式：仅执行非破坏性只读测试，完整核对重复读取数据一致性，绝对不擦除/改写 Flash 数据！
 */
void AppFlash_Test(void)
{
    #define FLASH_TEST_LEN    256U
    #define FLASH_TEST_ROUNDS 3U

#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
    static uint8_t tx[FLASH_TEST_LEN];
#endif
    static uint8_t rx[FLASH_TEST_LEN];
    static uint8_t ref[FLASH_TEST_LEN];
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
    /* ---- 默认安全模式：基准读取 + 重复读取数据一致性比对，零擦除零写入 ---- */
    /* 1. 先读取 256 字节作为基准参考 */
    if (W25QXX_Read(&g_flash, ref, FLASH_SAFE_TEST_SECTOR_ADDR, FLASH_TEST_LEN) == W25QXX_OK)
    {
        for (r = 0; r < FLASH_TEST_ROUNDS; r++)
        {
            memset(rx, 0, sizeof(rx));
            if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, FLASH_TEST_LEN) != W25QXX_OK)
            {
                break;
            }
            /* 完整对比缓冲区数据一致性 */
            if (memcmp(rx, ref, FLASH_TEST_LEN) != 0)
            {
                break;
            }
            rounds_ok++;

            /* TFT 与 Flash 共用 SPI1，交替占用总线刷块（验证总线仲裁） */
            LCD_Fill(&g_lcd, 16 + r * 72, 172, 16 + r * 72 + 56, 216, GREEN);
        }
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

    snprintf(line, sizeof(line), "%uB x%u  MATCH:%u/%u",
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
 * @note  Stage 1.2-B: 增强数据校验，独立统计通信成功与数据一致性指标
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
    static uint8_t ref_256[BUS_TEST_LEN];
    static uint8_t ref_buf[BUS_TEST_BUF];
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

    /* Stage 1.2-B 扩展统计变量 */
    uint32_t read_ok = 0;
    uint32_t data_match_ok = 0;
    uint32_t data_mismatch = 0;
    uint32_t first_mismatch_offset = 0xFFFFFFFFU;
    int32_t  last_flash_error = 0;

    /* Stage 1.2-C 基准有效性与动态预期数据维护 */
    uint32_t reference_valid = 0U;
    static uint8_t expected_256[BUS_TEST_LEN];
    uint8_t expected_256_valid = 0U;

    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLACK);

    /* ---------------- 0. 预读取基准数据（用于安全模式与恢复检验的基准校验） ---------------- */
    W25QXX_Result r256_res = W25QXX_Read(&g_flash, ref_256, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_LEN);
    if (r256_res == W25QXX_OK) {
        reference_valid |= 0x01U;
        memcpy(expected_256, ref_256, BUS_TEST_LEN);
        expected_256_valid = 1U;
    } else {
        last_flash_error = (int32_t)r256_res;
    }

    W25QXX_Result rbuf_res = W25QXX_Read(&g_flash, ref_buf, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_BUF);
    if (rbuf_res == W25QXX_OK) {
        reference_valid |= 0x02U;
    } else {
        last_flash_error = (int32_t)rbuf_res;
    }

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
            expected_256_valid = 0U;
            break;
        }
        erase_count++;
        if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_LEN) != W25QXX_OK) {
            g_app_test_result.rounds_fail_stage = 2;
            expected_256_valid = 0U;
            break;
        }
        if (memcmp(rx, tx, BUS_TEST_LEN) != 0) {
            g_app_test_result.rounds_fail_stage = 3;
            expected_256_valid = 0U;
            break;
        }
        /* 破坏性模式：成功写入并校验后，更新最新有效基准 */
        memcpy(expected_256, tx, BUS_TEST_LEN);
        expected_256_valid = 1U;
#else
        /* 安全模式：若基准无效，禁止进行未初始化内存比对，直接标记失败 */
        if ((reference_valid & 0x01U) == 0U) {
            g_app_test_result.rounds_fail_stage = 1;
            break;
        }
        /* 纯非破坏性读取，并与基准数据逐字节比对，在 20kHz 中断压力下验证总线仲裁与数据稳定性 */
        if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_LEN) != W25QXX_OK) {
            g_app_test_result.rounds_fail_stage = 2;
            break;
        }
        if (memcmp(rx, ref_256, BUS_TEST_LEN) != 0) {
            g_app_test_result.rounds_fail_stage = 3;
            break;
        }
#endif
        rounds_ok++;
        LCD_Fill(&g_lcd, 10 + r * 22, 200, 10 + r * 22 + 16, 216, GREEN);
    }
    BSP_SPI1_PreemptProbe_Stop();
    BSP_SPI1_PreemptProbe_Get(&probe);

    /* ---------------- 3. 传输长度矩阵（1~480 B，覆盖轮询与 DMA 切换，完整核对数据） ---------------- */
    {
#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
        for (i = 0; i < BUS_TEST_BUF; i++) {
            tx[i] = (uint8_t)(i ^ 0xA5U);
        }
        if (W25QXX_Write(&g_flash, tx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_BUF) == W25QXX_OK) {
            erase_count++;
            memcpy(expected_256, tx, BUS_TEST_LEN);
            expected_256_valid = 1U;
            for (uint8_t li = 0; li < len_count; li++) {
                uint16_t L = len_list[li];
                memset(rx, 0, L);
                if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, L) == W25QXX_OK &&
                    memcmp(rx, tx, L) == 0) {
                    len_ok++;
                }
            }
        } else {
            expected_256_valid = 0U;
        }
#else
        /* 安全模式：仅当 480B 基准有效时才核对数据 */
        if ((reference_valid & 0x02U) != 0U) {
            for (uint8_t li = 0; li < len_count; li++) {
                uint16_t L = len_list[li];
                memset(rx, 0, L);
                if (W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, L) == W25QXX_OK &&
                    memcmp(rx, ref_buf, L) == 0) {
                    len_ok++;
                }
            }
        }
#endif
    }

    /* ---------------- 4. 1000 次 SPI 事务压力（独立统计通信成功与数据一致性） ---------------- */
    {
        uint16_t k;
        uint8_t ref_byte = 0U;
#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
        uint8_t valid_for_stress = expected_256_valid;
        if (valid_for_stress != 0U) {
            ref_byte = expected_256[0];
        }
#else
        uint8_t valid_for_stress = ((reference_valid & 0x02U) != 0U) ? 1U : 0U;
        if (valid_for_stress != 0U) {
            ref_byte = ref_buf[0];
        }
#endif

        for (k = 0; k < STRESS_TXN; k++) {
            uint8_t b = 0U;
            W25QXX_Result res = W25QXX_Read(&g_flash, &b, FLASH_SAFE_TEST_SECTOR_ADDR, 1U);
            if (res == W25QXX_OK) {
                read_ok++;
                if (valid_for_stress != 0U && b == ref_byte) {
                    data_match_ok++;
                    str_ok++;
                } else {
                    data_mismatch++;
                    if (first_mismatch_offset == 0xFFFFFFFFU) {
                        first_mismatch_offset = 0;
                    }
                }
            } else {
                last_flash_error = (int32_t)res;
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
            if (wr != W25QXX_OK) {
                g_app_test_result.fl_fail_round = k;
                g_app_test_result.fl_last_result = (uint32_t)wr;
                expected_256_valid = 0U;
                break;
            }
            erase_count++;
            rr = W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_LEN);
            if (rr != W25QXX_OK) {
                g_app_test_result.fl_fail_round = k;
                g_app_test_result.fl_last_result = (uint32_t)rr;
                expected_256_valid = 0U;
                break;
            }
            if (memcmp(rx, tx, BUS_TEST_LEN) != 0) {
                g_app_test_result.fl_fail_round = k;
                expected_256_valid = 0U;
                break;
            }
            fl_ok++;
            memcpy(expected_256, tx, BUS_TEST_LEN);
            expected_256_valid = 1U;
        }
    }
#else
    fl_ok = 0;
#endif

    /* ---------------- 6. 统一异常恢复测试（调用 BSP_SPI1_Recover 并验证通信正常） ---------------- */
    {
        if (BSP_SPI1_Recover() == 0) {
            memset(rx, 0, BUS_TEST_LEN);
            if (expected_256_valid != 0U &&
                W25QXX_Read(&g_flash, rx, FLASH_SAFE_TEST_SECTOR_ADDR, BUS_TEST_LEN) == W25QXX_OK &&
                memcmp(rx, expected_256, BUS_TEST_LEN) == 0) {
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

    /* 校验已知设备 ID: W25Q128 (0xEF17) */
    uint8_t id_match = (g_flash.id == W25Q128 && g_flash.capacity == 16U * 1024U * 1024U) ? 1U : 0U;

#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
    pass = (ct_pass == ct_total) &&
           (reference_valid == 0x03U) &&
           (rounds_ok == BUS_TEST_ROUNDS) &&
           (len_ok == len_count) &&
           (str_ok == STRESS_TXN) &&
           (data_match_ok == STRESS_TXN) &&
           (data_mismatch == 0U) &&
           (fl_ok == FLASH_ROUNDS) &&
           (rec_ok == 1U) &&
           (id_match == 1U) &&
           (probe.granted == 0U) &&
           (owner_after == NULL) &&
           (depth_after == 0U);
#else
    pass = (ct_pass == ct_total) &&
           (reference_valid == 0x03U) &&
           (rounds_ok == BUS_TEST_ROUNDS) &&
           (len_ok == len_count) &&
           (str_ok == STRESS_TXN) &&
           (data_match_ok == STRESS_TXN) &&
           (data_mismatch == 0U) &&
           (rec_ok == 1U) &&
           (id_match == 1U) &&
           (probe.granted == 0U) &&
           (owner_after == NULL) &&
           (depth_after == 0U) &&
           (erase_count == 0U);
#endif

    /* ---------------- 7. 导出结构体与上屏 ---------------- */
    g_app_test_result.magic                 = 0x54534554U;   /* 'TEST' */
    g_app_test_result.ct_pass               = ct_pass;
    g_app_test_result.ct_total              = ct_total;
    g_app_test_result.ct_fail               = s_ct_fail;
    g_app_test_result.rounds_ok             = rounds_ok;
    g_app_test_result.rounds_total          = BUS_TEST_ROUNDS;
    g_app_test_result.len_ok                = len_ok;
    g_app_test_result.len_count             = len_count;
    g_app_test_result.str_ok                = str_ok;
    g_app_test_result.str_total             = STRESS_TXN;
    g_app_test_result.fl_ok                 = fl_ok;
    g_app_test_result.fl_total              = (CONFIG_FLASH_DESTRUCTIVE_TEST != 0) ? FLASH_ROUNDS : 0U;
    g_app_test_result.erase_count           = erase_count;
    g_app_test_result.rec_ok                = rec_ok;
    g_app_test_result.ll_recover_count      = g_ll_recover_count;
    g_app_test_result.ll_last_error         = g_ll_last_error;
    g_app_test_result.owner_null            = (owner_after == NULL) ? 1U : 0U;
    g_app_test_result.depth_zero            = (depth_after == 0U) ? 1U : 0U;
    g_app_test_result.pass                  = pass ? 1U : 0U;
    g_app_test_result.elapsed_ms            = elapsed;
    g_app_test_result.test_mode             = CONFIG_FLASH_DESTRUCTIVE_TEST;
    g_app_test_result.test_sector_addr      = FLASH_SAFE_TEST_SECTOR_ADDR;
    /* Stage 1.2-B 扩展字段 */
    g_app_test_result.read_ok               = read_ok;
    g_app_test_result.data_match_ok         = data_match_ok;
    g_app_test_result.data_mismatch         = data_mismatch;
    g_app_test_result.first_mismatch_offset = first_mismatch_offset;
    g_app_test_result.last_flash_error      = last_flash_error;
    /* Stage 1.2-C 扩展字段 */
    g_app_test_result.reference_valid       = reference_valid;
    g_app_test_result.expected_256_valid    = expected_256_valid;

    /* 240x240 圆屏摘要：关键指标放中心，避免文字被四角遮挡。 */
    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLACK);
    Draw_Circle(&g_lcd, 120, 120, 113, DARKBLUE);
    LCD_ShowString(&g_lcd, 72, 22, "SPI1 HEALTH", CYAN, BLACK, 16, 0);
    LCD_ShowString(&g_lcd, 68, 48, "W25Q128 16M", LGRAY, BLACK, 16, 0);

    snprintf(line, sizeof(line), "CT:%u/%u IRQ:%s",
             (unsigned)ct_pass, (unsigned)ct_total,
             (probe.granted == 0U) ? "OK" : "ERR");
    LCD_ShowString(&g_lcd, 52, 72, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "LEN:%u/%u", (unsigned)len_ok, (unsigned)len_count);
    LCD_ShowString(&g_lcd, 72, 94, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "RD:%lu/%lu", (unsigned long)read_ok, (unsigned long)STRESS_TXN);
    LCD_ShowString(&g_lcd, 64, 114, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "EQ:%lu/%lu", (unsigned long)data_match_ok, (unsigned long)STRESS_TXN);
    LCD_ShowString(&g_lcd, 64, 134, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "REC:%s REF:%s",
             rec_ok ? "OK" : "ERR", (reference_valid == 0x03U) ? "OK" : "ERR");
    LCD_ShowString(&g_lcd, 64, 154, line, (rec_ok && reference_valid == 0x03U) ? GREEN : RED, BLACK, 16, 0);

#if (CONFIG_FLASH_DESTRUCTIVE_TEST != 0)
    snprintf(line, sizeof(line), "ERASE:%lu TEST", (unsigned long)erase_count);
#else
    snprintf(line, sizeof(line), "SAFE ERASE:0");
#endif
    LCD_ShowString(&g_lcd, 68, 178, line, YELLOW, BLACK, 16, 0);
    LCD_ShowString(&g_lcd, 96, 199, pass ? "PASS" : "FAIL",
                   pass ? GREEN : RED, BLACK, 24, 0);
}
