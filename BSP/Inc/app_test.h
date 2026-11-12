/**
 * @file app_test.h
 * @brief 应用层显示测试模块接口
 * @details GC9A01 上电自测 + 周期心跳任务，测试逻辑集中在本模块，
 *          main.c 只负责底层初始化和周期调度。
 */

#ifndef __APP_TEST_H_
#define __APP_TEST_H_

/**
 * @brief GC9A01 上电显示自测（颜色/色条/文字/边框/圆），阻塞约 1s
 * @note  需在 GC9A01_PortInit() 之后调用
 */
void AppTFT_Test(void);

/**
 * @brief W25QXX 共享总线测试：写-读-校验并把结果显示在 TFT 上
 * @note  需在 W25QXX_PortInit() 之后调用；与 TFT 交替访问同一 SPI1，
 *        用于验证总线占用/释放（仲裁）与两种传输方式共存
 */
void AppFlash_Test(void);

/**
 * @brief 总线并发测试：接口契约检查 + 高优先级中断抢占压力测试
 * @note  需在 W25QXX_PortInit() 之后调用；结束后屏幕显示通过项数与抢占统计，
 *        owner/depth 必须归零，否则判 FAIL
 */
void AppBusTest(void);

/**
 * @brief GC9A01 周期任务：心跳闪烁指示，非阻塞
 */
void AppTFT_Task(void);

#endif /* __APP_TEST_H_ */
