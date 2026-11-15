/**
 * @file app_hil_test.h
 * @brief 目标硬件在环 (HIL) 自动化回归与诊断测试模块接口
 * @details 独立于正式应用固件。仅在开启 CONFIG_HIL_TESTS 或 CONFIG_TFT_ONLY 时编译。
 *          包含 GC9A01 上电自测、W25QXX 读写与数据完整性校验、SPI 契约与 TIM2 中断抢占压力测试。
 */

#ifndef __APP_HIL_TEST_H_
#define __APP_HIL_TEST_H_

#include <stdint.h>
#include "gc9a01.h"

/**
 * @brief 测试模式配置
 * @note  默认 0 (非破坏性安全模式)：上电仅识别 Flash、执行只读检验，绝不执行任何物理扇区擦除或写入。
 *        置 1 (破坏性测试模式)：显式启用，将在确认的安全测试扇区执行擦写读测试。
 */
#ifndef CONFIG_FLASH_DESTRUCTIVE_TEST
#define CONFIG_FLASH_DESTRUCTIVE_TEST 0
#endif

/* 破坏性测试时显式允许的安全测试扇区（禁止使用 0x000000，必须 4KB 对齐且严格 < capacity） */
#define FLASH_SAFE_TEST_SECTOR_ADDR   0x00100000U /* 1MB 偏移处测试专用扇区，远离 0x0000 */

/**
 * @brief 板级硬件与编译模式诊断信息（供 GDB 与自动化测试直接读取）
 */
typedef struct {
    uint32_t magic;              /* 0x44494147 ('DIAG') */
    uint32_t dual_spi_mode;      /* CONFIG_DUAL_SPI (0=SHARED_SPI1, 1=DUAL_SPI) */
    uint32_t tft_only_mode;      /* CONFIG_TFT_ONLY (0=Normal, 1=TFT_ONLY) */
    uint32_t tft_dc_pin;         /* 13 (PB13) 或 4 (PA4) */
    uint32_t tft_dc_port_id;     /* 1 (GPIOB) 或 0 (GPIOA) */
    uint32_t tft_bus_id;         /* 1 (SPI1) */
    uint32_t flash_bus_id;       /* 1 (SPI1) 或 2 (SPI2) */
    uint32_t tft_speed_hz;       /* 当前 TFT SPI 速率 */
    uint32_t tft_use_dma;        /* 0=轮询, 1=DMA */
} bsp_diag_info_t;

/**
 * @brief TFT 最小硬件诊断结果导出
 */
typedef struct {
    uint32_t magic;              /* 0x54465444 ('TFTD') */
    uint32_t init_result;        /* GC9A01_PortInit 结果 (LCD_Result) */
    uint32_t clock_hz;           /* SPI 速率 */
    uint32_t use_dma;            /* 0=轮询, 1=DMA */
    uint32_t fill_black_res;     /* 黑 (0x0000) 填充结果 */
    uint32_t fill_red_res;       /* 红 (0xF800) 填充结果 */
    uint32_t fill_green_res;     /* 绿 (0x07E0) 填充结果 */
    uint32_t fill_blue_res;      /* 蓝 (0x001F) 填充结果 */
    uint32_t fill_white_res;     /* 白 (0xFFFF) 填充结果 */
    uint32_t fills_ok_count;     /* LCD_OK 填充计数 (0..5) */
    uint32_t spi_err_count;      /* 期间 SPI 总线错误次数 */
    uint32_t dma_state;          /* DMA 通道状态标志 (0 表示无错误) */
    uint32_t current_step;       /* 当前测试步骤 */
    uint32_t test_complete;      /* 1 = 测试完成 */
} tft_diag_result_t;

/**
 * @brief AppBusTest 结果导出（供 SWD 读取做 HIL 验收）
 */
typedef struct {
    uint32_t magic;              /* 0x54534554 ('TEST') */
    uint32_t ct_pass, ct_total;
    uint32_t ct_fail;            /* s_ct_fail 位图 */
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
    uint32_t owner_null;         /* 结束时 owner == NULL */
    uint32_t depth_zero;         /* 结束时 depth == 0 */
    uint32_t pass;               /* 总体 PASS */
    uint32_t elapsed_ms;
    /* Stage 1.2 安全与诊断字段 */
    uint32_t test_mode;          /* 0: SAFE/READ-ONLY, 1: DESTRUCTIVE */
    uint32_t test_sector_addr;   /* 经确认的测试扇区物理地址 */
    uint32_t rounds_fail_stage;
    uint32_t rounds_fail_tx0;
    uint32_t rounds_fail_rx0;
    /* Stage 1.2-B: 数据一致性校验扩展统计 */
    uint32_t read_ok;            /* 读取接口成功次数 */
    uint32_t data_match_ok;      /* 数据重复读取一致次数 */
    uint32_t data_mismatch;      /* 数据不一致次数 */
    uint32_t first_mismatch_offset;/* 首个不一致字节偏移 */
    int32_t  last_flash_error;   /* 最后一次 Flash 错误码 */
    /* Stage 1.2-C 扩展字段 */
    uint32_t reference_valid;    /* 预读取基准有效性: bit0=ref_256, bit1=ref_buf, 0x03=全部有效 */
    uint32_t expected_256_valid; /* 预期恢复基准是否有效（写入失败时置 0） */
} app_test_result_t;

extern volatile bsp_diag_info_t g_bsp_diag;
extern volatile tft_diag_result_t g_tft_diag;
extern volatile tft_diag_result_t g_tft_diag_stages[8];
extern volatile uint32_t g_tft_diag_stage_count;
extern volatile app_test_result_t g_app_test_result;

/* HIL 测试启动与调度入口 */
void AppHIL_Init(void);
void AppHIL_Run(void);
void AppHIL_Task(void);

/* TFT 硬件诊断入口 */
void AppTFT_DiagInit(void);
void AppTFT_DiagRun(uint32_t clock_hz, uint8_t use_dma);
void AppTFT_DiagRunAll(void);
void AppTFT_DiagLoop(void);

/* 子测试接口 */
void AppTFT_Test(void);
void AppFlash_Test(void);
void AppBusTest(void);
void AppTFT_Task(void);

#endif /* __APP_HIL_TEST_H_ */
