/**
 * @file app_demo.h
 * @brief 交互式应用 Demo 模块接口
 * @details 专为 GC9A01 240x240 圆屏优化的单按键交互式多页面演示：
 *          1. DISPLAY DEMO   - 几何图形、文字与色彩渲染
 *          2. FLASH READ ONLY - W25Q128 JEDEC ID 识别与零擦写只读校验
 *          3. DUAL SPI STATUS - SPI1 (TFT) 与 SPI2 (Flash) 独立总线状态
 *          4. KEY1 INPUT TEST - 单按键短按/长按与引脚实时电平测试
 *          零动态内存分配，零大帧缓冲，严格保护外部 Flash。
 */

#ifndef __APP_DEMO_H_
#define __APP_DEMO_H_

#include <stdint.h>

/**
 * @brief Demo 页面枚举
 */
typedef enum {
    DEMO_PAGE_MENU = 0,     /**< 主菜单导航 */
    DEMO_PAGE_DISPLAY,      /**< 图形与色彩演示 */
    DEMO_PAGE_FLASH,        /**< 外部 Flash 只读验证 */
    DEMO_PAGE_DUAL_SPI,     /**< 双 SPI 总线状态监控 */
    DEMO_PAGE_KEY_TEST,     /**< KEY1 输入与交互测试 */
    DEMO_PAGE_MAX
} app_demo_page_t;

/**
 * @brief 初始化交互式 Demo 并绘制初始菜单
 */
void AppDemo_Init(void);

/**
 * @brief 交互式 Demo 状态机调度函数（由 main.c 主循环周期调用）
 */
void AppDemo_Run(void);

#endif /* __APP_DEMO_H_ */
