/**
 * @file app_demo.c
 * @brief 交互式应用 Demo 实现文件
 * @details 专为 GC9A01 240x240 圆屏优化的单按键交互式多页面演示程序。
 *          通过 KEY1 (PB12) 短按切换/刷新，长按返回主菜单。
 *          不依赖全屏 FrameBuffer，无动态内存分配，对 W25Q128 严格零擦写。
 */

#include "app_demo.h"
#include "bsp_includes.h"
#include <stdio.h>
#include <string.h>

/* 当前页面与菜单项 */
static app_demo_page_t s_current_page = DEMO_PAGE_MENU;
static uint8_t         s_menu_selection = 0;
static uint8_t         s_display_pattern = 0;
static uint32_t        s_flash_read_count = 0;

/* 菜单项名称定义 */
static const char *s_menu_items[] = {
    "1. DISPLAY DEMO",
    "2. FLASH READ ONLY",
    "3. DUAL SPI STATUS",
    "4. KEY1 INPUT TEST"
};
#define MENU_ITEM_COUNT  (sizeof(s_menu_items) / sizeof(s_menu_items[0]))

/* 内部页面渲染函数 */
static void render_menu_page(void);
static void render_menu_selection_only(void);
static void render_display_demo_page(void);
static void render_flash_read_page(void);
static void render_dual_spi_page(void);
static void render_key_test_page(uint8_t full_redraw);

/**
 * @brief 绘制页面公共装饰与底部提示
 */
static void render_bottom_prompt(const char *action_text)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "[KEY1] Click:%s", action_text);
    LCD_ShowString(&g_lcd, (uint16_t)((240U - (uint16_t)strlen(buf) * 6U) / 2U), 194U, buf, GREEN, BLACK, 12U, 0U);
    LCD_ShowString(&g_lcd, 90U, 208U, "Hold:Back", GREEN, BLACK, 12U, 0U);
}

/**
 * @brief 渲染主菜单全屏
 */
static void render_menu_page(void)
{
    LCD_Fill(&g_lcd, 0U, 0U, LCD_W, LCD_H, BLACK);
    Draw_Circle(&g_lcd, 120U, 120U, 118U, DARKBLUE);

    LCD_ShowString(&g_lcd, 64U, 25U, "STM32F103 DEMO", CYAN, BLACK, 16U, 0U);
    LCD_ShowString(&g_lcd, 72U, 45U, "Dual SPI Driver", GRAY, BLACK, 12U, 0U);

    render_menu_selection_only();

    LCD_ShowString(&g_lcd, 66U, 192U, "[KEY1] Click:Next", GREEN, BLACK, 12U, 0U);
    LCD_ShowString(&g_lcd, 84U, 208U, "Hold:Enter", GREEN, BLACK, 12U, 0U);
}

/**
 * @brief 仅刷新菜单高亮项（避免全屏重绘闪烁）
 */
static void render_menu_selection_only(void)
{
    const uint16_t start_y = 74U;
    const uint16_t row_h   = 26U;

    for (uint8_t i = 0U; i < MENU_ITEM_COUNT; i++) {
        uint16_t y = start_y + (uint16_t)(i * row_h);
        if (i == s_menu_selection) {
            LCD_Fill(&g_lcd, 36U, y - 2U, 204U, y + 18U, DARKBLUE);
            LCD_ShowString(&g_lcd, 44U, y, s_menu_items[i], YELLOW, DARKBLUE, 16U, 0U);
        } else {
            LCD_Fill(&g_lcd, 36U, y - 2U, 204U, y + 18U, BLACK);
            LCD_ShowString(&g_lcd, 44U, y, s_menu_items[i], WHITE, BLACK, 16U, 0U);
        }
    }
}

/**
 * @brief 渲染 Page 1: DISPLAY DEMO
 */
static void render_display_demo_page(void)
{
    LCD_Fill(&g_lcd, 0U, 0U, LCD_W, LCD_H, BLACK);
    Draw_Circle(&g_lcd, 120U, 120U, 118U, DARKBLUE);

    LCD_ShowString(&g_lcd, 56U, 24U, "1. DISPLAY DEMO", YELLOW, BLACK, 16U, 0U);

    if (s_display_pattern == 0U) {
        /* 图案 1: 几何图形与对角线 */
        LCD_ShowString(&g_lcd, 66U, 44U, "Geometry & Shapes", GRAY, BLACK, 12U, 0U);

        /* 矩形与对角交叉线 */
        LCD_DrawRectangle(&g_lcd, 40U, 62U, 110U, 112U, CYAN);
        LCD_DrawLine(&g_lcd, 40U, 62U, 110U, 112U, YELLOW);
        LCD_DrawLine(&g_lcd, 40U, 112U, 110U, 62U, YELLOW);

        /* 同心圆 */
        Draw_Circle(&g_lcd, 165U, 87U, 24U, GREEN);
        Draw_Circle(&g_lcd, 165U, 87U, 16U, RED);
        Draw_Circle(&g_lcd, 165U, 87U, 8U, WHITE);

        /* 局部色彩填充块（验证行缓冲 DMA 性能） */
        LCD_Fill(&g_lcd, 40U, 124U, 200U, 148U, BLUE);
        LCD_ShowString(&g_lcd, 54U, 130U, "RGB565 Line Buffer", WHITE, BLUE, 12U, 0U);

        LCD_ShowString(&g_lcd, 52U, 164U, "Render: Shapes [1/3]", WHITE, BLACK, 12U, 0U);
    } else if (s_display_pattern == 1U) {
        /* 图案 2: 六阶色条 */
        LCD_ShowString(&g_lcd, 64U, 44U, "RGB565 Color Bars", GRAY, BLACK, 12U, 0U);

        const uint16_t bar_colors[6] = {RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA};
        const char *bar_names[6] = {"RED", "GREEN", "BLUE", "YELLOW", "CYAN", "MAGENTA"};
        for (uint8_t i = 0U; i < 6U; i++) {
            uint16_t y = 62U + (uint16_t)(i * 16U);
            LCD_Fill(&g_lcd, 40U, y, 200U, y + 12U, bar_colors[i]);
            uint16_t text_col = (i == 3U || i == 4U) ? BLACK : WHITE;
            LCD_ShowString(&g_lcd, 96U, y, bar_names[i], text_col, bar_colors[i], 12U, 0U);
        }

        LCD_ShowString(&g_lcd, 52U, 166U, "Render: Bars   [2/3]", WHITE, BLACK, 12U, 0U);
    } else {
        /* 图案 3: 同心靶心与坐标十字 */
        LCD_ShowString(&g_lcd, 68U, 44U, "Radial Target", GRAY, BLACK, 12U, 0U);

        const uint16_t target_colors[5] = {CYAN, GREEN, YELLOW, RED, MAGENTA};
        for (uint8_t r = 0U; r < 5U; r++) {
            Draw_Circle(&g_lcd, 120U, 110U, (uint8_t)(10U + r * 10U), target_colors[r]);
        }
        LCD_DrawLine(&g_lcd, 60U, 110U, 180U, 110U, WHITE);
        LCD_DrawLine(&g_lcd, 120U, 50U, 120U, 170U, WHITE);

        LCD_ShowString(&g_lcd, 52U, 166U, "Render: Target [3/3]", WHITE, BLACK, 12U, 0U);
    }

    render_bottom_prompt("Mode");
}

/**
 * @brief 渲染 Page 2: FLASH READ ONLY
 * @note  绝不执行擦除或写入，仅执行读 JEDEC ID 与安全数据校验
 */
static void render_flash_read_page(void)
{
    LCD_Fill(&g_lcd, 0U, 0U, LCD_W, LCD_H, BLACK);
    Draw_Circle(&g_lcd, 120U, 120U, 118U, DARKBLUE);

    LCD_ShowString(&g_lcd, 52U, 24U, "2. FLASH READ ONLY", YELLOW, BLACK, 16U, 0U);
    LCD_ShowString(&g_lcd, 54U, 44U, "Strictly Zero Erase/Write", GRAY, BLACK, 12U, 0U);

    s_flash_read_count++;

    /* 1. 读取 JEDEC ID */
    uint16_t flash_id = W25QXX_ReadID(&g_flash);
    uint32_t capacity = g_flash.capacity;

    /* 2. 双重只读校验：连续两次读取 32 字节并对比 */
    uint8_t buf1[32] = {0};
    uint8_t buf2[32] = {0};
    const uint32_t safe_addr = 0x00100000U;

    W25QXX_Result r1 = W25QXX_Read(&g_flash, buf1, safe_addr, sizeof(buf1));
    W25QXX_Result r2 = W25QXX_Read(&g_flash, buf2, safe_addr, sizeof(buf2));

    uint8_t match_ok = (r1 == W25QXX_OK && r2 == W25QXX_OK && memcmp(buf1, buf2, sizeof(buf1)) == 0);

    /* 页面数据展示 */
    char line[40];

    /* JEDEC ID */
    snprintf(line, sizeof(line), "JEDEC ID: 0x%04X (%s)", flash_id, (flash_id == 0xEF17U) ? "OK" : "ERR");
    LCD_ShowString(&g_lcd, 40U, 66U, line, (flash_id == 0xEF17U) ? GREEN : RED, BLACK, 12U, 0U);

    /* 容量 */
    snprintf(line, sizeof(line), "Capacity: %lu MB (128M)", (unsigned long)(capacity / (1024U * 1024U)));
    LCD_ShowString(&g_lcd, 40U, 84U, line, WHITE, BLACK, 12U, 0U);

    /* 地址与采样字节 */
    snprintf(line, sizeof(line), "Addr: 0x%08lX", (unsigned long)safe_addr);
    LCD_ShowString(&g_lcd, 40U, 102U, line, WHITE, BLACK, 12U, 0U);

    snprintf(line, sizeof(line), "Data: %02X %02X %02X %02X...",
             buf1[0], buf1[1], buf1[2], buf1[3]);
    LCD_ShowString(&g_lcd, 40U, 120U, line, CYAN, BLACK, 12U, 0U);

    /* 比对与状态 */
    snprintf(line, sizeof(line), "Verify: 32B Match [%s]", match_ok ? "PASS" : "FAIL");
    LCD_ShowString(&g_lcd, 40U, 138U, line, match_ok ? GREEN : RED, BLACK, 12U, 0U);

    snprintf(line, sizeof(line), "Erase Count: 0 (SAFE)");
    LCD_ShowString(&g_lcd, 40U, 156U, line, YELLOW, BLACK, 12U, 0U);

    snprintf(line, sizeof(line), "Read Rounds: %lu", (unsigned long)s_flash_read_count);
    LCD_ShowString(&g_lcd, 40U, 172U, line, LGRAY, BLACK, 12U, 0U);

    render_bottom_prompt("Read");
}

/**
 * @brief 渲染 Page 3: DUAL SPI STATUS
 */
static void render_dual_spi_page(void)
{
    LCD_Fill(&g_lcd, 0U, 0U, LCD_W, LCD_H, BLACK);
    Draw_Circle(&g_lcd, 120U, 120U, 118U, DARKBLUE);

    LCD_ShowString(&g_lcd, 50U, 22U, "3. DUAL SPI STATUS", YELLOW, BLACK, 16U, 0U);
    LCD_ShowString(&g_lcd, 64U, 40U, "Bus Diagnostics", GRAY, BLACK, 12U, 0U);

    spi_bus_t *bus1 = BSP_SPI_GetBus(BSP_SPI_BUS_1);
    spi_bus_t *bus2 = BSP_SPI_GetBus(BSP_SPI_BUS_2);

    char line[40];

    /* SPI1 区块 (TFT GC9A01) */
    LCD_ShowString(&g_lcd, 36U, 58U, "SPI1: GC9A01 (18MHz Mode0)", CYAN, BLACK, 12U, 0U);
    LCD_ShowString(&g_lcd, 36U, 72U, "Pins: PA5/7 RST:10 DC:PA4", WHITE, BLACK, 12U, 0U);
    LCD_ShowString(&g_lcd, 36U, 86U, "DMA : DMA1 Ch3(TX)/Ch2(RX)", WHITE, BLACK, 12U, 0U);

    snprintf(line, sizeof(line), "Stat: %s  Depth:%u",
             (bus1 && spi_bus_owner(bus1) != NULL) ? "BUSY" : "IDLE",
             bus1 ? spi_bus_depth(bus1) : 0U);
    LCD_ShowString(&g_lcd, 36U, 100U, line, GREEN, BLACK, 12U, 0U);

    /* 中间分割线 */
    LCD_DrawLine(&g_lcd, 36U, 114U, 204U, 114U, GRAY);

    /* SPI2 区块 (W25Q128 Flash) */
    LCD_ShowString(&g_lcd, 36U, 120U, "SPI2: W25Q128 (9MHz Mode0)", CYAN, BLACK, 12U, 0U);
    LCD_ShowString(&g_lcd, 36U, 134U, "Pins: PB13/14/15 CS:PA8", WHITE, BLACK, 12U, 0U);
    LCD_ShowString(&g_lcd, 36U, 148U, "DMA : DMA1 Ch5(TX)/Ch4(RX)", WHITE, BLACK, 12U, 0U);

    snprintf(line, sizeof(line), "Stat: %s  Depth:%u",
             (bus2 && spi_bus_owner(bus2) != NULL) ? "BUSY" : "IDLE",
             bus2 ? spi_bus_depth(bus2) : 0U);
    LCD_ShowString(&g_lcd, 36U, 162U, line, GREEN, BLACK, 12U, 0U);

    render_bottom_prompt("Refresh");
}

/**
 * @brief 渲染 Page 4: KEY1 INPUT TEST
 */
static void render_key_test_page(uint8_t full_redraw)
{
    static uint8_t prev_level = 0xFFU;
    static uint32_t prev_clicks = UINT32_MAX, prev_longs = UINT32_MAX;
    if (full_redraw) {
        LCD_Fill(&g_lcd, 0U, 0U, LCD_W, LCD_H, BLACK);
        Draw_Circle(&g_lcd, 120U, 120U, 118U, DARKBLUE);
        LCD_ShowString(&g_lcd, 52U, 24U, "4. KEY1 INPUT TEST", YELLOW, BLACK, 16U, 0U);
        LCD_ShowString(&g_lcd, 64U, 44U, "PB12 GPIO & Debounce", GRAY, BLACK, 12U, 0U);
        LCD_ShowString(&g_lcd, 44U, 66U, "Pin : PB12 (Pull-Up)", WHITE, BLACK, 12U, 0U);
        LCD_ShowString(&g_lcd, 44U, 82U, "LED : PC13 (Active Low)", WHITE, BLACK, 12U, 0U);
        render_bottom_prompt("Count");
        prev_level = 0xFFU;
        prev_clicks = UINT32_MAX;
        prev_longs = UINT32_MAX;
    }

    uint8_t level = KEY_GetRawLevel();
    uint32_t clicks = KEY_GetClickCount(), longs = KEY_GetLongCount();
    char line[36];
    if (level != prev_level) {
        LCD_Fill(&g_lcd, 44U, 102U, 204U, 115U, BLACK);
        snprintf(line, sizeof(line), "Level: %s", level ? "RELEASED" : "PRESSED");
        LCD_ShowString(&g_lcd, 44U, 102U, line, level ? CYAN : YELLOW, BLACK, 12U, 0U);
        LCD_Fill(&g_lcd, 110U, 158U, 130U, 178U, level ? BLACK : GREEN);
        if (level) Draw_Circle(&g_lcd, 120U, 168U, 9U, GRAY);
        prev_level = level;
    }
    if (clicks != prev_clicks) {
        LCD_Fill(&g_lcd, 44U, 120U, 204U, 133U, BLACK);
        snprintf(line, sizeof(line), "Clicks : %lu", (unsigned long)clicks);
        LCD_ShowString(&g_lcd, 44U, 120U, line, GREEN, BLACK, 12U, 0U);
        prev_clicks = clicks;
    }
    if (longs != prev_longs) {
        LCD_Fill(&g_lcd, 44U, 138U, 204U, 151U, BLACK);
        snprintf(line, sizeof(line), "Longs  : %lu", (unsigned long)longs);
        LCD_ShowString(&g_lcd, 44U, 138U, line, LGRAY, BLACK, 12U, 0U);
        prev_longs = longs;
    }
}

void AppDemo_Init(void)
{
    s_current_page     = DEMO_PAGE_MENU;
    s_menu_selection   = 0U;
    s_display_pattern  = 0U;
    s_flash_read_count = 0U;

    render_menu_page();
}

void AppDemo_Run(void)
{
    /* 1. 扫描按键事件 */
    KEY_Task();
    key_event_t ev = KEY_GetEvent();

    /* 2. 在 KEY TEST 页面中周期性刷新实时电平 */
    if (s_current_page == DEMO_PAGE_KEY_TEST) {
        static uint32_t s_last_key_refresh = 0;
        uint32_t now = GetTick_ms();
        if ((uint32_t)(now - s_last_key_refresh) >= 100U) {
            s_last_key_refresh = now;
            render_key_test_page(0U);
        }
    }

    /* 3. 无按键事件直接返回 */
    if (ev == KEY_EVENT_NONE) {
        return;
    }

    /* 4. 主菜单页面状态处理 */
    if (s_current_page == DEMO_PAGE_MENU) {
        if (ev == KEY_EVENT_SHORT) {
            /* 短按：切换下一菜单项 */
            s_menu_selection = (uint8_t)((s_menu_selection + 1U) % MENU_ITEM_COUNT);
            render_menu_selection_only();
        } else if (ev == KEY_EVENT_LONG) {
            /* 长按：进入当前选中项页面 */
            s_current_page = (app_demo_page_t)(s_menu_selection + 1U);
            switch (s_current_page) {
                case DEMO_PAGE_DISPLAY:
                    s_display_pattern = 0U;
                    render_display_demo_page();
                    break;
                case DEMO_PAGE_FLASH:
                    render_flash_read_page();
                    break;
                case DEMO_PAGE_DUAL_SPI:
                    render_dual_spi_page();
                    break;
                case DEMO_PAGE_KEY_TEST:
                    render_key_test_page(1U);
                    break;
                default:
                    s_current_page = DEMO_PAGE_MENU;
                    render_menu_page();
                    break;
            }
        }
    } else {
        /* 5. 功能子页面处理 */
        if (ev == KEY_EVENT_LONG) {
            /* 任何子页面长按均返回主菜单 */
            s_current_page = DEMO_PAGE_MENU;
            render_menu_page();
        } else if (ev == KEY_EVENT_SHORT) {
            /* 各子页面短按动作 */
            switch (s_current_page) {
                case DEMO_PAGE_DISPLAY:
                    s_display_pattern = (uint8_t)((s_display_pattern + 1U) % 3U);
                    render_display_demo_page();
                    break;
                case DEMO_PAGE_FLASH:
                    render_flash_read_page();
                    break;
                case DEMO_PAGE_DUAL_SPI:
                    render_dual_spi_page();
                    break;
                case DEMO_PAGE_KEY_TEST:
                    render_key_test_page(0U);
                    break;
                default:
                    break;
            }
        }
    }
}
