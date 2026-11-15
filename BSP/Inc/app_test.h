/**
 * @file app_test.h
 * @brief 应用层显示测试模块接口
 * @details GC9A01 上电自测 + 周期心跳任务，测试逻辑集中在本模块，
 *          main.c 只负责底层初始化和周期调度。
 *          Stage 2.2-A-HIL: 新增 TFT_ONLY 最小硬件诊断与板级诊断导出结构。
 */

#ifndef __APP_TEST_H_
#define __APP_TEST_H_

#include <stdint.h>

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

extern volatile bsp_diag_info_t g_bsp_diag;
extern volatile tft_diag_result_t g_tft_diag;
extern volatile tft_diag_result_t g_tft_diag_stages[8];
extern volatile uint32_t g_tft_diag_stage_count;

void AppTFT_DiagInit(void);
void AppTFT_DiagRun(uint32_t clock_hz, uint8_t use_dma);
void AppTFT_DiagRunAll(void);
void AppTFT_DiagLoop(void);

/**
 * @brief GC9A01 上电显示自测（颜色/色条/文字/边框/圆），阻塞约 1s
 * @note  需在 GC9A01_PortInit() 之后调用
 */
void AppTFT_Test(void);

/**
 * @brief W25QXX 共享总线测试：识别与读校验并把结果显示在 TFT 上
 * @note  默认安全模式下仅执行非破坏性只读测试与 TFT DMA 交替访问
 */
void AppFlash_Test(void);

/**
 * @brief 总线并发测试：接口契约检查 + 高优先级中断抢占压力测试
 * @note  默认安全模式下不执行物理擦除与写入，全量契约与抢占保护仍然有效
 */
void AppBusTest(void);

/**
 * @brief GC9A01 周期任务：心跳闪烁指示，非阻塞
 */
void AppTFT_Task(void);

#endif /* __APP_TEST_H_ */
