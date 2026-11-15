/**
 * @file key_port.h
 * @brief 单按键驱动接口头文件（KEY1 / PB12）
 * @details 基于 multi_button 状态机实现 KEY1 (PB12) 短按与长按检测。
 *          PB14 已完全释放给 SPI2_MISO，本工程无 KEY2。
 *          LED (PC13) 提供按键操作的瞬态状态指示。
 */

#ifndef __KEY_PORT_H_
#define __KEY_PORT_H_

#include <stdint.h>
#include "main.h"

/**
 * @brief 按键事件枚举
 */
typedef enum {
    KEY_EVENT_NONE = 0, /**< 无事件 */
    KEY_EVENT_SHORT,    /**< 短按单击 */
    KEY_EVENT_LONG,     /**< 长按触发 (~1000ms) */
} key_event_t;

/**
 * @brief 按键与 LED 初始化
 * @note  配置 PB12 上拉输入、PC13 推挽输出，并启动 multi_button
 */
void KEY_Init(void);

/**
 * @brief 按键心跳扫描任务
 * @details 主循环负责 LED 反馈；5ms 扫描节拍由 SysTick 中断保证
 */
void KEY_Task(void);

/** Called once per 1ms by SysTick_Handler. The real button scan runs every 5ms.
 * Callback only records events; no display or SPI operation in the IRQ. */
void KEY_OnSysTick1ms(void);

/**
 * @brief 读取并消费当前按键事件
 * @return 待处理的按键事件（调用后内部自动清除）
 */
key_event_t KEY_GetEvent(void);

/**
 * @brief 获取 PB12 当前实时物理电平
 * @retval 0 按下（低电平）; 1 释放（高电平）
 */
uint8_t KEY_GetRawLevel(void);

/**
 * @brief 获取累计短按次数
 */
uint32_t KEY_GetClickCount(void);

/**
 * @brief 获取累计长按次数
 */
uint32_t KEY_GetLongCount(void);

/**
 * @brief 控制板载 LED (PC13)
 * @param on 1=点亮 (低电平), 0=熄灭 (高电平)
 */
void KEY_SetLED(uint8_t on);

/**
 * @brief 翻转板载 LED (PC13)
 */
void KEY_ToggleLED(void);

#endif /* __KEY_PORT_H_ */
