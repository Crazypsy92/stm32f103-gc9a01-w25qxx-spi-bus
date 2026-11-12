/**
 * @file key_port.c
 * @brief 按键驱动实现文件（裸机事件驱动版）
 * @details 按键 GPIO 由驱动内部初始化；multi_button 状态机由主循环按
 *          TICKS_INTERVAL(5ms) 节拍调用 button_ticks() 驱动；
 *          事件通过回调记录，主循环再分发业务动作。
 *          无任务、无队列、无信号量、无阻塞延时。
 * @author 西伯利亚小菜狗
 * @version 3.0 (bare-metal)
 */

#include "key_port.h"

/* 按键对象 */
static Button KEY1;
static Button KEY2;

/* 本轮待处理的单击事件标志：bit1 对应 KEY_ID_1，bit2 对应 KEY_ID_2 */
#define KEY_PENDING_BIT(id)     ((uint8_t)(1U << (id)))
static uint8_t s_pending_clicks = 0;

/* 事件统计（供应用层显示验证，按 KEY_ID 索引） */
typedef struct {
    uint8_t clicks;     /* 单击次数 */
    uint8_t longs;      /* 长按开始次数 */
} KeyStats;
static KeyStats s_key_stats[3];

/* 内部函数声明 */
static void KEY_GPIO_Init(void);
static void key_dispatch_event(uint8_t id);

/**
 * @brief 按键与 LED 的 GPIO 初始化（驱动内部完成，不依赖 CubeMX）
 * @note  按键一端接 GND，GPIO 上拉输入，按下为低电平；LED 低电平点亮
 */
static void KEY_GPIO_Init(void)
{
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOB);
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOC);

    GPIO_InitStruct.Pin  = key1_Pin | key2_Pin;
    GPIO_InitStruct.Mode = LL_GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = LL_GPIO_PULL_UP;
    LL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin         = led_Pin;
    GPIO_InitStruct.Mode        = LL_GPIO_MODE_OUTPUT;
    GPIO_InitStruct.Speed       = LL_GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.OutputType  = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStruct.Pull        = LL_GPIO_PULL_UP;
    LL_GPIO_Init(led_GPIO_Port, &GPIO_InitStruct);
    LL_GPIO_SetOutputPin(led_GPIO_Port, led_Pin);   /* 初始熄灭 */
}

/**
 * @brief 读取引脚电平
 */
uint8_t Read_Pin_Level(uint8_t button_id)
{
    switch (button_id)
    {
        case KEY_ID_1:
            return LL_GPIO_IsInputPinSet(key1_GPIO_Port, key1_Pin);
        case KEY_ID_2:
            return LL_GPIO_IsInputPinSet(key2_GPIO_Port, key2_Pin);
        default:
            return 1;   /* 默认返回释放电平 */
    }
}

/**
 * @brief 统一的按键回调函数
 * @note  运行在 button_ticks() 调用者（主循环）上下文，不是硬件中断；
 *        只记录事件，不发送队列、不阻塞
 */
void Key_Common_Callback(Button *btn)
{
    uint8_t id;
    ButtonEvent event;

    if (btn == NULL) return;

    id    = btn->button_id;
    event = button_get_event(btn);
    if (id != KEY_ID_1 && id != KEY_ID_2) return;

    switch (event)
    {
        case BTN_SINGLE_CLICK:
            s_key_stats[id].clicks++;
            s_pending_clicks |= KEY_PENDING_BIT(id);
            break;
        case BTN_LONG_PRESS_START:
        case BTN_LONG_PRESS_HOLD:
            if (event == BTN_LONG_PRESS_START) s_key_stats[id].longs++;
            break;
        default:
            /* 短按按下 / 释放 / 连击等事件状态机内部已处理，此处不额外记录 */
            break;
    }
}

/**
 * @brief 按键业务分发：KEY1 单击点亮 LED，KEY2 单击熄灭 LED
 */
static void key_dispatch_event(uint8_t id)
{
    switch (id)
    {
        case KEY_ID_1:
            LL_GPIO_ResetOutputPin(led_GPIO_Port, led_Pin);
            break;
        case KEY_ID_2:
            LL_GPIO_SetOutputPin(led_GPIO_Port, led_Pin);
            break;
        default:
            break;
    }
}

/**
 * @brief 处理本轮扫描产生的单击事件
 * @details 由主循环在 button_ticks() 返回后调用，先取走并清空待处理标志，
 *          再按固定顺序分发：先 KEY1，后 KEY2
 */
void Key_ProcessEvents(void)
{
    uint8_t pending = s_pending_clicks;

    s_pending_clicks = 0;

    if (pending & KEY_PENDING_BIT(KEY_ID_1))
    {
        key_dispatch_event(KEY_ID_1);
    }
    if (pending & KEY_PENDING_BIT(KEY_ID_2))
    {
        key_dispatch_event(KEY_ID_2);
    }
}

uint8_t Key_GetClickCount(uint8_t id)
{
    return (id == KEY_ID_1 || id == KEY_ID_2) ? s_key_stats[id].clicks : 0;
}

uint8_t Key_GetLongCount(uint8_t id)
{
    return (id == KEY_ID_1 || id == KEY_ID_2) ? s_key_stats[id].longs : 0;
}

/**
 * @brief 按键初始化
 */
void KEY_Init(void)
{
    KEY_GPIO_Init();

    /* 初始化 multi_button 对象：handle, 读引脚函数, 有效电平(0=低), ID */
    button_init(&KEY1, Read_Pin_Level, 0, KEY_ID_1);
    button_init(&KEY2, Read_Pin_Level, 0, KEY_ID_2);

    /* 绑定需要向上层传递的事件：单击、长按开始 */
    button_attach(&KEY1, BTN_SINGLE_CLICK,     Key_Common_Callback);
    button_attach(&KEY1, BTN_LONG_PRESS_START, Key_Common_Callback);
    button_attach(&KEY2, BTN_SINGLE_CLICK,     Key_Common_Callback);
    button_attach(&KEY2, BTN_LONG_PRESS_START, Key_Common_Callback);

    button_start(&KEY1);
    button_start(&KEY2);
}
