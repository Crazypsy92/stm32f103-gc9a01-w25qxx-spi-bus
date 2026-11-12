/**
 * @file    delay.c
 * @brief   统一时间基准实现
 * @details 1ms 心跳：SysTick 已在 SystemClock_Config() 中由 LL_Init1msTick() 配好，
 *          这里只打开中断，SysTick_Handler 中递增毫秒计数。
 *          微秒延时：DWT->CYCCNT 周期计数，与 1ms 心跳互不干扰。
 * @author  西伯利亚小菜狗
 * @version 3.2 (bare-metal)
 */

#include "delay.h"

/* ================================================================================== */
/* DWT 周期计数器                                                     */
/* ================================================================================== */

#define DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004U)   /**< 周期计数寄存器 */
#define DWT_CTRL    (*(volatile uint32_t *)0xE0001000U)   /**< DWT 控制寄存器 */
#define DEMCR       (*(volatile uint32_t *)0xE000EDFCU)   /**< 调试异常评测控制寄存器 */

extern uint32_t SystemCoreClock;

/* ================================================================================== */
/* 公共变量                                                           */
/* ================================================================================== */

/** @brief 毫秒计数，由 SysTick_Handler 中断递增 */
volatile uint32_t g_sys_tick = 0U;

/* ================================================================================== */
/* 公共函数实现                                                       */
/* ================================================================================== */

/**
 * @brief  初始化时间基准
 * @note   SysTick 1ms 已由 SystemClock_Config() 里的 LL_Init1msTick() 配置并使能，
 *         这里只补开中断，不再重复配置 SysTick
 */
void Timebase_Init(void)
{
    DEMCR      |= (1U << 24);   /* TRCENA: 使能 DWT */
    DWT_CYCCNT  = 0U;
    DWT_CTRL   |= 1U;           /* CYCCNTENA: 启动周期计数 */

    LL_SYSTICK_EnableIT();      /* 打开 SysTick 1ms 中断 */
}

/**
 * @brief  微秒级阻塞延时（DWT）
 * @param  us 延时的微秒数
 */
void Delay_us(uint32_t us)
{
    uint32_t start = DWT_CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000U);

    /* 无符号减法天然处理 32 位计数回绕 */
    while ((DWT_CYCCNT - start) < ticks) { }
}

/**
 * @brief  获取系统运行时间
 * @return 上电以来的毫秒数
 */
uint32_t GetTick_ms(void)
{
    return g_sys_tick;
}

/**
 * @brief  毫秒级阻塞延时（SysTick 心跳）
 * @param  ms 延时的毫秒数
 */
void Delay_ms(uint32_t ms)
{
    uint32_t start = GetTick_ms();
    while ((GetTick_ms() - start) < ms) { }
}

/* ================================================================================== */
/* printf 重定向                                                      */
/* ================================================================================== */

#if ENABLE_PRINTF_REDIRECT

#include "stm32f1xx_ll_usart.h"

/**
 * @brief  重定向 fputc 到调试串口
 * @param  ch 要输出的字符
 * @param  f  文件指针（标准库使用，忽略）
 * @return 输出的字符
 */
int fputc(int ch, FILE *f)
{
    (void)f;  /* 避免未使用参数警告 */

    while (!LL_USART_IsActiveFlag_TXE(huart_debug)) { }

    LL_USART_TransmitData8(huart_debug, (uint8_t)ch);
    return ch;
}

#endif /* ENABLE_PRINTF_REDIRECT */
