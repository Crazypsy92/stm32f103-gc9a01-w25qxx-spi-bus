/**
 * @file app_test.h
 * @brief 应用层显示测试模块接口
 * @details GC9A01 上电自测 + 周期心跳任务，测试逻辑集中在本模块，
 *          main.c 只负责底层初始化和周期调度。
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
