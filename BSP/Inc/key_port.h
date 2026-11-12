/**
 * @file key_port.h
 * @brief 按键驱动接口头文件（裸机版）
 * @details 定义按键硬件接口及函数声明，基于 multi_button 状态机实现。
 *          事件通过回调向上层传递，不依赖 RTOS 任务、队列或信号量。
 * @author 西伯利亚小菜狗
 * @version 3.0 (bare-metal)
 */

#ifndef __KEY_PORT_H_
#define __KEY_PORT_H_

#include "main.h"
#include <stdint.h>
#include "multi_button.h"

/** @defgroup KEY_CONFIG 按键硬件配置 @{ */
#define KEY_ID_1    1   /**< key1: PB12 */
#define KEY_ID_2    2   /**< key2: PB14 */
/** @} */

/**
 * @brief 按键初始化
 * @details 驱动内部完成按键与 LED 的 GPIO 初始化（不依赖 CubeMX），
 *          初始化 multi_button 对象并绑定事件回调。
 *          需在 Timebase_Init() 之后调用。
 */
void KEY_Init(void);

/**
 * @brief 读取按键引脚电平（供 multi_button 调用）
 * @param[in] button_id 按键 ID（KEY_ID_x）
 * @retval 0 按下（低电平）; 1 释放（高电平）
 */
uint8_t Read_Pin_Level(uint8_t button_id);

/**
 * @brief 按键事件统一回调函数
 * @details 在主循环调用 button_ticks() 的上下文中执行（非中断上下文），
 *          只做极短的事件记录，不阻塞、不使用队列
 */
void Key_Common_Callback(Button *btn);

/**
 * @brief 处理本轮扫描产生的按键业务动作
 * @details 由主循环在 button_ticks() 返回后调用：
 *          KEY1 单击点亮 LED，KEY2 单击熄灭 LED。
 */
void Key_ProcessEvents(void);

/** @brief 获取指定按键的单击累计次数（供应用层/显示验证使用） */
uint8_t Key_GetClickCount(uint8_t id);

/** @brief 获取指定按键的长按触发累计次数（供应用层/显示验证使用） */
uint8_t Key_GetLongCount(uint8_t id);

#endif /* __KEY_PORT_H_ */
