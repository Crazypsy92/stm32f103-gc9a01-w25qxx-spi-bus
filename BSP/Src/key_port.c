/**
 * @file key_port.c
 * @brief 单按键驱动实现文件（KEY1 / PB12）
 * @details 基于 multi_button 状态机驱动，主循环通过 KEY_Task() 5ms 定时节拍扫描。
 *          PB14 完全由 SPI2 占用，杜绝按键冲突。
 *          按键触发时提供 PC13 LED 状态翻转反馈。
 */

#include "key_port.h"
#include "multi_button.h"
#include "delay.h"

/* 按键对象 */
static Button s_key1;

/* 待处理事件标志与统计 */
static volatile key_event_t s_pending_event = KEY_EVENT_NONE;
static volatile uint32_t   s_click_count    = 0;
static volatile uint32_t   s_long_count     = 0;

/**
 * @brief 读取 KEY1 引脚电平（供 multi_button 调用）
 */
static uint8_t key1_read_pin(uint8_t button_id)
{
    (void)button_id;
    return (uint8_t)LL_GPIO_IsInputPinSet(key1_GPIO_Port, key1_Pin);
}

/**
 * @brief 按键事件回调
 */
static void key1_event_callback(Button *btn)
{
    if (btn == NULL) {
        return;
    }

    ButtonEvent ev = button_get_event(btn);
    switch (ev)
    {
        case BTN_SINGLE_CLICK:
            s_pending_event = KEY_EVENT_SHORT;
            s_click_count++;
            KEY_ToggleLED();
            break;

        case BTN_LONG_PRESS_START:
            s_pending_event = KEY_EVENT_LONG;
            s_long_count++;
            KEY_ToggleLED();
            break;

        default:
            break;
    }
}

void KEY_Init(void)
{
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOB);
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOC);

    /* KEY1: PB12 上拉输入（按键按下为低电平） */
    GPIO_InitStruct.Pin  = key1_Pin;
    GPIO_InitStruct.Mode = LL_GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = LL_GPIO_PULL_UP;
    LL_GPIO_Init(key1_GPIO_Port, &GPIO_InitStruct);

    /* LED: PC13 推挽输出（低电平点亮，初始熄灭） */
    GPIO_InitStruct.Pin        = led_Pin;
    GPIO_InitStruct.Mode       = LL_GPIO_MODE_OUTPUT;
    GPIO_InitStruct.Speed      = LL_GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStruct.Pull       = LL_GPIO_PULL_UP;
    LL_GPIO_Init(led_GPIO_Port, &GPIO_InitStruct);
    LL_GPIO_SetOutputPin(led_GPIO_Port, led_Pin);

    /* 初始化 multi_button：有效电平 0 (按下低电平)，ID 为 1 */
    button_init(&s_key1, key1_read_pin, 0, 1);
    button_attach(&s_key1, BTN_SINGLE_CLICK,     key1_event_callback);
    button_attach(&s_key1, BTN_LONG_PRESS_START, key1_event_callback);
    button_start(&s_key1);
}

void KEY_Task(void)
{
    static uint32_t s_last_tick = 0;
    uint32_t now = GetTick_ms();

    if ((uint32_t)(now - s_last_tick) >= TICKS_INTERVAL) {
        s_last_tick = now;
        button_ticks();
    }
}

key_event_t KEY_GetEvent(void)
{
    key_event_t ev = s_pending_event;
    s_pending_event = KEY_EVENT_NONE;
    return ev;
}

uint8_t KEY_GetRawLevel(void)
{
    return (uint8_t)LL_GPIO_IsInputPinSet(key1_GPIO_Port, key1_Pin);
}

uint32_t KEY_GetClickCount(void)
{
    return s_click_count;
}

uint32_t KEY_GetLongCount(void)
{
    return s_long_count;
}

void KEY_SetLED(uint8_t on)
{
    if (on) {
        LL_GPIO_ResetOutputPin(led_GPIO_Port, led_Pin); /* 低电平点亮 */
    } else {
        LL_GPIO_SetOutputPin(led_GPIO_Port, led_Pin);   /* 高电平熄灭 */
    }
}

void KEY_ToggleLED(void)
{
    LL_GPIO_TogglePin(led_GPIO_Port, led_Pin);
}
