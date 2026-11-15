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
 * @brief Stage 1.3 Task 4: SPI 双设备交替访问测试结果导出
 */
typedef struct {
    uint32_t magic;              /* 0x4455414C ('DUAL') */
    uint32_t rounds_ok;          /* 20 轮交替全成功轮数 */
    uint32_t rounds_total;       /* 20 */
    uint32_t id_ok_count;        /* ID 读取成功次数 */
    uint32_t dma256_ok_count;    /* 256B DMA 校验成功次数 */
    uint32_t dma480_ok_count;    /* 480B DMA 校验成功次数 */
    uint32_t tft_update_count;   /* TFT 局部更新成功次数 */
    uint32_t post_read_ok;       /* TFT 更新后重读 Flash 校验成功次数 */
    uint32_t owner_null;         /* 结束时 owner == NULL */
    uint32_t depth_zero;         /* 结束时 depth == 0 */
    uint32_t pass;               /* 1 = PASS */
    uint32_t elapsed_ms;
} app_dual_test_result_t;

volatile app_dual_test_result_t g_app_dual_result;

/* Stage 2.2-A-HIL: 板级硬件与 TFT 最小诊断变量导出 */
volatile bsp_diag_info_t  g_bsp_diag;
volatile tft_diag_result_t g_tft_diag;
volatile tft_diag_result_t g_tft_diag_stages[8];
volatile uint32_t         g_tft_diag_stage_count = 0;

void AppTFT_DiagInit(void)
{
    g_bsp_diag.magic         = 0x44494147U; /* 'DIAG' */
    g_bsp_diag.dual_spi_mode = CONFIG_DUAL_SPI;
#if defined(CONFIG_TFT_ONLY) && (CONFIG_TFT_ONLY != 0)
    g_bsp_diag.tft_only_mode = 1U;
#else
    g_bsp_diag.tft_only_mode = 0U;
#endif
#if defined(CONFIG_DUAL_SPI) && (CONFIG_DUAL_SPI != 0)
    g_bsp_diag.tft_dc_pin     = 4U;
    g_bsp_diag.tft_dc_port_id = 0U; /* GPIOA */
    g_bsp_diag.flash_bus_id   = 2U;
#else
    g_bsp_diag.tft_dc_pin     = 13U;
    g_bsp_diag.tft_dc_port_id = 1U; /* GPIOB */
    g_bsp_diag.flash_bus_id   = 1U;
#endif
    g_bsp_diag.tft_bus_id    = 1U;
    g_bsp_diag.tft_speed_hz  = 18000000U;
    g_bsp_diag.tft_use_dma   = 1U;
}

void AppTFT_DiagRun(uint32_t clock_hz, uint8_t use_dma)
{
    AppTFT_DiagInit();
    g_bsp_diag.tft_speed_hz = clock_hz;
    g_bsp_diag.tft_use_dma  = use_dma;

    memset((void *)&g_tft_diag, 0, sizeof(g_tft_diag));
    g_tft_diag.magic    = 0x54465444U; /* 'TFTD' */
    g_tft_diag.clock_hz = clock_hz;
    g_tft_diag.use_dma  = use_dma;

    /* 初始化 GC9A01 硬件 */
    GC9A01_PortConfig cfg = {
        .bus          = NULL, /* Auto-bind SPI1 */
        .spi_clock_hz = clock_hz,
        .spi_mode     = SPI_BUS_MODE_0,
        .use_dma      = use_dma,
#if defined(CONFIG_DUAL_SPI) && (CONFIG_DUAL_SPI != 0)
        .dc_port      = GPIOA,
        .dc_pin       = LL_GPIO_PIN_4,
#else
        .dc_port      = GPIOB,
        .dc_pin       = LL_GPIO_PIN_13,
#endif
        .cs_port      = GPIOA,
        .cs_pin       = LL_GPIO_PIN_9,
        .rst_port     = GPIOA,
        .rst_pin      = LL_GPIO_PIN_10,
    };

    LCD_Result init_res = GC9A01_PortInitWithConfig(&g_lcd, &cfg);
    g_tft_diag.init_result = (uint32_t)init_res;

    if (init_res != LCD_OK) {
        g_tft_diag.spi_err_count++;
        g_tft_diag.test_complete = 1;
        return;
    }

    /* 依次填充黑、红、绿、蓝、白五种全屏纯色 (480B 行缓冲) */
    g_tft_diag.current_step = 1;
    g_tft_diag.fill_black_res = (uint32_t)LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLACK);
    if (g_tft_diag.fill_black_res == LCD_OK) g_tft_diag.fills_ok_count++;
    Delay_ms(800);

    g_tft_diag.current_step = 2;
    g_tft_diag.fill_red_res = (uint32_t)LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, RED);
    if (g_tft_diag.fill_red_res == LCD_OK) g_tft_diag.fills_ok_count++;
    Delay_ms(800);

    g_tft_diag.current_step = 3;
    g_tft_diag.fill_green_res = (uint32_t)LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, GREEN);
    if (g_tft_diag.fill_green_res == LCD_OK) g_tft_diag.fills_ok_count++;
    Delay_ms(800);

    g_tft_diag.current_step = 4;
    g_tft_diag.fill_blue_res = (uint32_t)LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLUE);
    if (g_tft_diag.fill_blue_res == LCD_OK) g_tft_diag.fills_ok_count++;
    Delay_ms(800);

    g_tft_diag.current_step = 5;
    g_tft_diag.fill_white_res = (uint32_t)LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, WHITE);
    if (g_tft_diag.fill_white_res == LCD_OK) g_tft_diag.fills_ok_count++;
    Delay_ms(800);

    /* 检查 DMA 错误标志（如果启用了 DMA） */
    if (use_dma) {
        g_tft_diag.dma_state = LL_DMA_IsActiveFlag_TE3(DMA1) ? 1U : 0U;
    } else {
        g_tft_diag.dma_state = 0U;
    }

    g_tft_diag.current_step = 6;
    g_tft_diag.test_complete = 1;
}

void AppTFT_DiagRunAll(void)
{
    static const struct {
        uint32_t clock_hz;
        uint8_t  use_dma;
    } tests[8] = {
        {  1125000U, 0U },
        {  4500000U, 0U },
        {  9000000U, 0U },
        { 18000000U, 0U },
        {  1125000U, 1U },
        {  4500000U, 1U },
        {  9000000U, 1U },
        { 18000000U, 1U },
    };

    g_tft_diag_stage_count = 0;
    for (uint32_t i = 0; i < 8; i++) {
        AppTFT_DiagRun(tests[i].clock_hz, tests[i].use_dma);
        g_tft_diag_stages[i] = g_tft_diag;
        g_tft_diag_stage_count = i + 1;
        Delay_ms(500);
    }
}

void AppTFT_DiagLoop(void)
{
    static uint32_t last_time = 0;
    static uint8_t color_idx = 0;
    static const uint16_t colors[5] = { BLACK, RED, GREEN, BLUE, WHITE };
    uint32_t now = GetTick_ms();
    if (now - last_time >= 1000) {
        last_time = now;
        color_idx = (color_idx + 1) % 5;
        LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, colors[color_idx]);
    }
}

/**
 * @brief GC9A01 全屏流式图案演示：240 行 × 240 像素 (115,200 字节)，
 *        使用 480B 小行缓冲逐行流式生成，验证 MCU 有限内存下的全屏刷屏能力。
 */
static LCD_Result AppTFT_StreamPattern(void)
{
    static uint8_t line_buf[LCD_W * 2];
    static const uint16_t color_table[8] = { RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, WHITE, DARKBLUE };
    LCD_Result res = LCD_Address_Set(&g_lcd, 0, 0, LCD_W - 1, LCD_H - 1);
    if (res != LCD_OK) {
        return res;
    }
    res = g_lcd.port->cs(g_lcd.port->ctx, 0);
    if (res != LCD_OK) {
        return res;
    }
    g_lcd.port->dc(g_lcd.port->ctx, 1);
    LCD_Result wr_res = LCD_OK;
    for (uint16_t y = 0; y < LCD_H; y++) {
        uint16_t c = color_table[(y / 30U) % 8U];
        for (uint16_t x = 0; x < LCD_W; x++) {
            line_buf[x * 2U]     = (uint8_t)(c >> 8);
            line_buf[x * 2U + 1] = (uint8_t)(c & 0xFF);
        }
        wr_res = g_lcd.port->write(g_lcd.port->ctx, line_buf, LCD_W * 2U);
        if (wr_res != LCD_OK) {
            break;
        }
    }
    LCD_Result rel_res = g_lcd.port->cs(g_lcd.port->ctx, 1);
    if (wr_res != LCD_OK) return wr_res;
    if (rel_res != LCD_OK) return rel_res;
    Delay_ms(400);
    return LCD_OK;
}

/**
 * @brief GC9A01 上电显示自测
 */
void AppTFT_Test(void)
{
    /* 1. 全屏流式 8 色条测试图案 (115,200 字节，480B 行缓冲) */
    AppTFT_StreamPattern();

    /* 2. 圆形屏引导页：不占用额外帧缓冲，RGB 色块同时检验颜色通路。 */
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
    Delay_ms(1000);
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
 * @brief 双设备共享总线测试：TFT 与 W25QXX 交替占用 SPI1 (Stage 1.3 Task 4: 20 轮交替访问)
 * @note  默认安全模式：仅执行非破坏性只读测试，完整核对重复读取数据一致性，绝对不擦除/改写 Flash 数据！
 */
void AppFlash_Test(void)
{
    #define DUAL_TEST_ROUNDS 20U
    #define DUAL_TEST_256    256U
    #define DUAL_TEST_480    480U

    static uint8_t ref_256[DUAL_TEST_256];
    static uint8_t ref_480[DUAL_TEST_480];
    static uint8_t rx_256[DUAL_TEST_256];
    static uint8_t rx_480[DUAL_TEST_480];
    char line[40];
    uint32_t elapsed;
    uint8_t r;
    uint8_t rounds_ok = 0;
    uint32_t id_ok_count = 0;
    uint32_t dma256_ok_count = 0;
    uint32_t dma480_ok_count = 0;
    uint32_t tft_update_count = 0;
    uint32_t post_read_ok = 0;

    LCD_Fill(&g_lcd, 0, 0, LCD_W, LCD_H, BLACK);
    Draw_Circle(&g_lcd, 120, 120, 113, DARKBLUE);
    LCD_ShowString(&g_lcd, 64, 25, "DUAL BUS TEST", CYAN, BLACK, 16, 0);
    LCD_ShowString(&g_lcd, 68, 50, "TFT + W25Q128", WHITE, BLACK, 16, 0);

    elapsed = GetTick_ms();

    /* 1. 预读取 256B 与 480B 基准数据 */
    if (W25QXX_Read(&g_flash, ref_256, FLASH_SAFE_TEST_SECTOR_ADDR, DUAL_TEST_256) == W25QXX_OK &&
        W25QXX_Read(&g_flash, ref_480, FLASH_SAFE_TEST_SECTOR_ADDR, DUAL_TEST_480) == W25QXX_OK)
    {
        for (r = 0; r < DUAL_TEST_ROUNDS; r++)
        {
            /* 步骤 1: 读 ID */
            uint16_t id = W25QXX_ReadID(&g_flash);
            if (id == W25Q128) {
                id_ok_count++;
            } else {
                break;
            }

            /* 步骤 2: DMA 读取 256B 并校验 */
            memset(rx_256, 0, DUAL_TEST_256);
            if (W25QXX_Read(&g_flash, rx_256, FLASH_SAFE_TEST_SECTOR_ADDR, DUAL_TEST_256) == W25QXX_OK &&
                memcmp(rx_256, ref_256, DUAL_TEST_256) == 0) {
                dma256_ok_count++;
            } else {
                break;
            }

            /* 步骤 3: DMA 读取 480B 并校验 */
            memset(rx_480, 0, DUAL_TEST_480);
            if (W25QXX_Read(&g_flash, rx_480, FLASH_SAFE_TEST_SECTOR_ADDR, DUAL_TEST_480) == W25QXX_OK &&
                memcmp(rx_480, ref_480, DUAL_TEST_480) == 0) {
                dma480_ok_count++;
            } else {
                break;
            }

            /* 步骤 4: TFT 更新局部图形块（交替占用 SPI1） */
            LCD_Result fill_res = LCD_Fill(&g_lcd, 24 + (r % 10) * 19, 140 + (r / 10) * 16,
                                           24 + (r % 10) * 19 + 15, 140 + (r / 10) * 16 + 12, GREEN);
            if (fill_res == LCD_OK) {
                tft_update_count++;
            } else {
                break;
            }

            /* 步骤 5: 再次读取 Flash 256B 并校验 */
            memset(rx_256, 0, DUAL_TEST_256);
            if (W25QXX_Read(&g_flash, rx_256, FLASH_SAFE_TEST_SECTOR_ADDR, DUAL_TEST_256) == W25QXX_OK &&
                memcmp(rx_256, ref_256, DUAL_TEST_256) == 0) {
                post_read_ok++;
            } else {
                break;
            }

            rounds_ok++;
        }
    }

    elapsed = GetTick_ms() - elapsed;

    spi_bus_t *bus1 = BSP_SPI1_GetBus();
    spi_bus_t *bus2 = BSP_SPI_IsBusActive(BSP_SPI_BUS_2) ? BSP_SPI2_GetBus() : NULL;
    uint8_t owner_ok = (spi_bus_owner(bus1) == NULL) && (bus2 == NULL || spi_bus_owner(bus2) == NULL);
    uint8_t depth_ok = (bus1->depth == 0U) && (bus2 == NULL || bus2->depth == 0U);
    uint8_t pass = (rounds_ok == DUAL_TEST_ROUNDS) &&
                   (id_ok_count == DUAL_TEST_ROUNDS) &&
                   (dma256_ok_count == DUAL_TEST_ROUNDS) &&
                   (dma480_ok_count == DUAL_TEST_ROUNDS) &&
                   (tft_update_count == DUAL_TEST_ROUNDS) &&
                   (post_read_ok == DUAL_TEST_ROUNDS) &&
                   owner_ok && depth_ok;

    g_app_dual_result.magic            = 0x4455414CU; /* 'DUAL' */
    g_app_dual_result.rounds_ok        = rounds_ok;
    g_app_dual_result.rounds_total     = DUAL_TEST_ROUNDS;
    g_app_dual_result.id_ok_count      = id_ok_count;
    g_app_dual_result.dma256_ok_count  = dma256_ok_count;
    g_app_dual_result.dma480_ok_count  = dma480_ok_count;
    g_app_dual_result.tft_update_count = tft_update_count;
    g_app_dual_result.post_read_ok     = post_read_ok;
    g_app_dual_result.owner_null       = owner_ok ? 1U : 0U;
    g_app_dual_result.depth_zero       = depth_ok ? 1U : 0U;
    g_app_dual_result.pass             = pass ? 1U : 0U;
    g_app_dual_result.elapsed_ms       = elapsed;

    snprintf(line, sizeof(line), "ROUNDS:%u/%u", (unsigned)rounds_ok, (unsigned)DUAL_TEST_ROUNDS);
    LCD_ShowString(&g_lcd, 68, 75, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "ID:%lu DMA:%lu", (unsigned long)id_ok_count, (unsigned long)dma480_ok_count);
    LCD_ShowString(&g_lcd, 64, 98, line, WHITE, BLACK, 16, 0);

    snprintf(line, sizeof(line), "%s %lums", pass ? "DUAL PASS" : "DUAL FAIL", (unsigned long)elapsed);
    LCD_ShowString(&g_lcd, 72, 180, line, pass ? GREEN : RED, BLACK, 16, 0);

    Delay_ms(800);
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

    spi_bus_t *bus1 = BSP_SPI1_GetBus();
    spi_bus_t *bus2 = BSP_SPI_IsBusActive(BSP_SPI_BUS_2) ? BSP_SPI2_GetBus() : NULL;
    owner_after = spi_bus_owner(bus1);
    if (owner_after == NULL && bus2 != NULL) {
        owner_after = spi_bus_owner(bus2);
    }
    depth_after = bus1->depth;
    if (depth_after == 0U && bus2 != NULL) {
        depth_after = bus2->depth;
    }

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
