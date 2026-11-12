/**
 * @file    delay.h
 * @brief   统一时间基准 (Timebase) 头文件
 * @details SysTick 提供 1ms 心跳（毫秒计数），DWT 周期计数器提供微秒级延时。
 *          驱动层统一使用 Delay_us / Delay_ms / GetTick_ms，
 *          不再在各驱动中各自实现延时。
 * @author  西伯利亚小菜狗
 * @version 3.2 (bare-metal)
 */
#ifndef __DELAY_H_
#define __DELAY_H_

#include "main.h"
#include <stdint.h>

/* ================================================================================== */
/* 配置选项                                                           */
/* ================================================================================== */

/**
 * @brief  使能 printf 串口重定向
 * @note   1: 启用（需同时定义 huart_debug 并完成 USART 初始化）
 *         0: 禁用（默认，工程当前未接入串口）
 */
#define ENABLE_PRINTF_REDIRECT   0

/**
 * @brief  调试串口外设
 * @note   需要串口输出时取消下一行注释，并自行完成 USART 时钟/引脚/参数初始化
 */
/* #define huart_debug USART2 */

/* ================================================================================== */
/* 公共变量                                                           */
/* ================================================================================== */

/** @brief 毫秒计数，由 SysTick_Handler 中断每 1ms 递增一次 */
extern volatile uint32_t g_sys_tick;

/* ================================================================================== */
/* 公共函数                                                           */
/* ================================================================================== */

/**
 * @brief  初始化时间基准（DWT 微秒计时 + 打开 SysTick 中断）
 * @note   SysTick 的 1ms 配置已由 SystemClock_Config() 中的 LL_Init1msTick() 完成，
 *         本函数只开中断，不重复配置 SysTick
 * @note   必须在 SystemClock_Config() 之后、使用任何延时接口之前调用一次
 */
void Timebase_Init(void);

/**
 * @brief  微秒级阻塞延时（基于 DWT 硬件周期计数，不依赖 1ms 心跳）
 * @param  us 延时的微秒数
 * @note   精度为一个 CPU 周期，供软件 IIC 等时序使用
 */
void Delay_us(uint32_t us);

/**
 * @brief  毫秒级阻塞延时（基于 SysTick 毫秒心跳计数）
 * @param  ms 延时的毫秒数
 * @note   延时期间 CPU 空转，不会进入低功耗模式
 */
void Delay_ms(uint32_t ms);

/**
 * @brief  获取系统运行时间
 * @return 上电以来的毫秒数
 */
uint32_t GetTick_ms(void);

/* 兼容旧代码中的 delay_us()/delay_ms() 写法 */
#define delay_us(us)    Delay_us(us)
#define delay_ms(ms)    Delay_ms(ms)

/* ================================================================================== */
/* printf 重定向                                                      */
/* ================================================================================== */

#if ENABLE_PRINTF_REDIRECT

#include <stdio.h>

/**
 * @brief  重定向 fputc 到调试串口
 * @param  ch 要输出的字符
 * @param  f  文件指针（标准库使用，忽略）
 * @return 输出的字符
 * @note   需确保 huart_debug 已正确初始化
 */
int fputc(int ch, FILE *f);

#endif /* ENABLE_PRINTF_REDIRECT */

#endif /* __DELAY_H_ */
