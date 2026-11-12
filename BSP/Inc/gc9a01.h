/**
 * @file    GC9A01.h
 * @brief   GC9A01 圆形 TFT 屏驱动头文件（MCU 无关）
 * @details 纯协议层：不包含任何 MCU / LL / HAL 头文件，只依赖 LCD_Port 回调表。
 *          移植时在 GC9A01_port.c 中实现回调并填写板级引脚即可，本文件无需改动。
 */
#ifndef __GC9A01_H_
#define __GC9A01_H_

#include <stdint.h>
#include <stddef.h>

/* ==================== 屏幕参数 ==================== */

#define LCD_W 240
#define LCD_H 240

/* 显示方向：0 / 90 / 180 / 270（单位：度） */
#ifndef LCD_ROTATION
#define LCD_ROTATION 180
#endif

/* ==================== 抽象硬件接口（MCU 无关） ==================== */

/**
 * @brief LCD 底层操作回调表
 * @note  由 port 层实现并填充；驱动只通过回调访问引脚、延时与 SPI，不感知 MCU。
 *        level: 1=高电平/有效，0=低电平/无效。
 */
typedef struct {
    void (*cs)(void *ctx, uint8_t level);                    /* 片选控制 */
    void (*rst)(void *ctx, uint8_t level);                   /* 复位控制 */
    void (*dc)(void *ctx, uint8_t level);                    /* 数据/命令选择 */
    void (*bl)(void *ctx, uint8_t level);                    /* 背光控制，可为 NULL（不控制） */
    void (*delay_ms)(void *ctx, uint32_t ms);                /* 毫秒延时 */
    void (*write)(void *ctx, const uint8_t *buf, uint16_t len); /* SPI 连续发送 */
    void *ctx;                                               /* 传给上述回调的上下文 */
} LCD_Port;

/** @brief LCD 设备对象：只持有抽象端口与屏幕信息 */
typedef struct {
    const LCD_Port *port;    /* 绑定的底层端口（port 层提供，生命周期需覆盖设备） */
    uint16_t max_x;
    uint16_t max_y;
} LCD_Device;

/* ==================== 控制宏（基于抽象端口） ==================== */

#define LCD_CS_SET(dev)        ((dev)->port->cs((dev)->port->ctx, 1))
#define LCD_CS_CLR(dev)        ((dev)->port->cs((dev)->port->ctx, 0))
#define LCD_RST_SET(dev)       ((dev)->port->rst((dev)->port->ctx, 1))
#define LCD_RST_CLR(dev)       ((dev)->port->rst((dev)->port->ctx, 0))
#define LCD_DC_SET(dev)        ((dev)->port->dc((dev)->port->ctx, 1))
#define LCD_DC_CLR(dev)        ((dev)->port->dc((dev)->port->ctx, 0))
#define LCD_DELAY_MS(dev, ms)  ((dev)->port->delay_ms((dev)->port->ctx, (ms)))

/* 背光未接控制引脚（port->bl == NULL）时为空操作 */
#define LCD_BL_ON(dev)         do { if ((dev)->port->bl != NULL) (dev)->port->bl((dev)->port->ctx, 1); } while (0)
#define LCD_BL_OFF(dev)        do { if ((dev)->port->bl != NULL) (dev)->port->bl((dev)->port->ctx, 0); } while (0)

/* ==================== 颜色定义 (RGB565) ==================== */

#define WHITE         0xFFFF
#define BLACK         0x0000
#define BLUE          0x001F
#define BRED          0XF81F
#define GRED          0XFFE0
#define GBLUE         0X07FF
#define RED           0xF800
#define MAGENTA       0xF81F
#define GREEN         0x07E0
#define CYAN          0x7FFF
#define YELLOW        0xFFE0
#define BROWN         0XBC40   // 棕色
#define BRRED         0XFC07   // 棕红色
#define GRAY          0X8430   // 灰色
#define DARKBLUE      0X01CF   // 深蓝色
#define LIGHTBLUE     0X7D7C   // 浅蓝色
#define GRAYBLUE      0X5458   // 灰蓝色
#define LIGHTGREEN    0X841F   // 浅绿色
#define LGRAY         0XC618   // 浅灰色(PANNEL),窗体背景色
#define LGRAYBLUE     0XA651   // 浅灰蓝色(中间层颜色)
#define LBBLUE        0X2B12   // 浅棕蓝色(选择条目的反色)

/* ==================== 函数声明 ==================== */

/**
 * @brief 初始化 GC9A01（复位 + 面板初始化序列）
 * @param dev  设备句柄
 * @param port 已实现回调的端口（内部不负责其硬件初始化）
 * @note  调用前需保证 port 层已完成 GPIO / SPI 初始化
 */
void GC9A01_Init(LCD_Device *dev, const LCD_Port *port);

void LCD_WR_REG(LCD_Device *dev, uint8_t dat);
void LCD_WR_DATA8(LCD_Device *dev, uint8_t dat);
void LCD_WR_DATA(LCD_Device *dev, uint16_t dat);
void LCD_Address_Set(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

// 图形函数
void LCD_Fill(LCD_Device *dev, uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color);
void LCD_DrawPoint(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t color);
void LCD_DrawLine(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void LCD_DrawRectangle(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void Draw_Circle(LCD_Device *dev, uint16_t x0, uint16_t y0, uint8_t r, uint16_t color);

// 字符与数字显示（ASCII 字库 12/16/24/32 号）
void LCD_ShowChar(LCD_Device *dev, uint16_t x, uint16_t y, uint8_t num,
                  uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void LCD_ShowString(LCD_Device *dev, uint16_t x, uint16_t y, const char *p,
                    uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
uint32_t mypow(uint8_t m, uint8_t n);
void LCD_ShowIntNum(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t num, uint8_t len,
                    uint16_t fc, uint16_t bc, uint8_t sizey);
void LCD_ShowFloatNum1(LCD_Device *dev, uint16_t x, uint16_t y, float num, uint8_t len,
                       uint16_t fc, uint16_t bc, uint8_t sizey);

void LCD_ShowPicture(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t length, uint16_t width, const uint8_t pic[]);

#endif /* __GC9A01_H_ */
