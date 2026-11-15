/**
 * @file    gc9a01.h
 * @brief   GC9A01 圆形 TFT 屏驱动头文件（纯协议层，MCU 无关）
 * @details 纯协议层：不包含任何 MCU / LL / HAL 头文件，只依赖 LCD_Port 回调表。
 *          移植时在 gc9a01_port.c 中实现回调并填写板级引脚即可，本文件无需改动。
 *          Stage 2.1: 规范化返回状态码 (LCD_Result)，增加分块传输与坐标/参数边界防御。
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

/* ==================== 统一状态码 ==================== */

typedef enum {
    LCD_OK            =  0,   /* 操作成功 */
    LCD_ERROR_PARAM   = -1,   /* 参数非法（NULL 指针/越界/非法字号/溢出） */
    LCD_ERROR_BUS     = -2,   /* 总线错误（底层 SPI 错误/CS 失败/未持有总线） */
    LCD_ERROR_TIMEOUT = -3    /* 通信超时 */
} LCD_Result;

/* ==================== 抽象硬件接口（MCU 无关） ==================== */

/**
 * @brief LCD 底层操作回调表
 * @note  由 port 层实现并填充；驱动只通过回调访问引脚、延时与 SPI，不感知 MCU。
 *        level: 1=高电平/释放，0=低电平/有效。
 */
typedef struct {
    LCD_Result (*cs)(void *ctx, uint8_t level);                    /* 片选控制：0=拉低(占用总线)，1=拉高(释放总线) */
    void (*rst)(void *ctx, uint8_t level);                         /* 复位控制 */
    void (*dc)(void *ctx, uint8_t level);                          /* 数据/命令选择: 0=CMD, 1=DATA */
    void (*bl)(void *ctx, uint8_t level);                          /* 背光控制，可为 NULL（不控制） */
    void (*delay_ms)(void *ctx, uint32_t ms);                      /* 毫秒延时 */
    LCD_Result (*write)(void *ctx, const uint8_t *buf, uint16_t len); /* SPI 连续发送 */
    uint16_t max_chunk_len;                                        /* 单次传输上限 (0 或 65535 表示 65535) */
    void *ctx;                                                     /* 传给上述回调的上下文 */
} LCD_Port;

/** @brief LCD 设备对象：持有抽象端口、屏幕信息与最后错误码 */
typedef struct {
    const LCD_Port *port;       /* 绑定的底层端口（port 层提供，生命周期需覆盖设备） */
    uint16_t max_x;             /* 屏幕最大宽度 (默认 LCD_W) */
    uint16_t max_y;             /* 屏幕最大高度 (默认 LCD_H) */
    LCD_Result last_error;      /* 最后一次错误记录 */
} LCD_Device;

/* ==================== 控制宏（基于抽象端口） ==================== */

#define LCD_RST_SET(dev)       do { if ((dev) && (dev)->port && (dev)->port->rst) (dev)->port->rst((dev)->port->ctx, 1); } while (0)
#define LCD_RST_CLR(dev)       do { if ((dev) && (dev)->port && (dev)->port->rst) (dev)->port->rst((dev)->port->ctx, 0); } while (0)
#define LCD_DC_SET(dev)        do { if ((dev) && (dev)->port && (dev)->port->dc)  (dev)->port->dc((dev)->port->ctx, 1); } while (0)
#define LCD_DC_CLR(dev)        do { if ((dev) && (dev)->port && (dev)->port->dc)  (dev)->port->dc((dev)->port->ctx, 0); } while (0)
#define LCD_DELAY_MS(dev, ms)  do { if ((dev) && (dev)->port && (dev)->port->delay_ms) (dev)->port->delay_ms((dev)->port->ctx, (ms)); } while (0)

/* 背光未接控制引脚（port->bl == NULL）时为空操作 */
#define LCD_BL_ON(dev)         do { if ((dev) && (dev)->port && (dev)->port->bl != NULL) (dev)->port->bl((dev)->port->ctx, 1); } while (0)
#define LCD_BL_OFF(dev)        do { if ((dev) && (dev)->port && (dev)->port->bl != NULL) (dev)->port->bl((dev)->port->ctx, 0); } while (0)

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
 * @return LCD_OK 成功；其他为失败错误码
 */
LCD_Result GC9A01_Init(LCD_Device *dev, const LCD_Port *port);

/** @brief 查询最后一次错误码 */
LCD_Result LCD_GetLastError(const LCD_Device *dev);

/** @brief 清除错误状态 */
void LCD_ClearError(LCD_Device *dev);

/* 基础寄存器与数据写操作 */
LCD_Result LCD_WR_REG(LCD_Device *dev, uint8_t dat);
LCD_Result LCD_WR_DATA8(LCD_Device *dev, uint8_t dat);
LCD_Result LCD_WR_DATA(LCD_Device *dev, uint16_t dat);
LCD_Result LCD_Address_Set(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

/* 图形函数 */
LCD_Result LCD_Fill(LCD_Device *dev, uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color);
LCD_Result LCD_DrawPoint(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t color);
LCD_Result LCD_DrawLine(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
LCD_Result LCD_DrawRectangle(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
LCD_Result Draw_Circle(LCD_Device *dev, uint16_t x0, uint16_t y0, uint8_t r, uint16_t color);

/* 字符与数字显示（ASCII 字库 12/16/24/32 号） */
LCD_Result LCD_ShowChar(LCD_Device *dev, uint16_t x, uint16_t y, uint8_t num,
                        uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
LCD_Result LCD_ShowString(LCD_Device *dev, uint16_t x, uint16_t y, const char *p,
                          uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
uint32_t mypow(uint8_t m, uint8_t n);
LCD_Result LCD_ShowIntNum(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t num, uint8_t len,
                          uint16_t fc, uint16_t bc, uint8_t sizey);
LCD_Result LCD_ShowFloatNum1(LCD_Device *dev, uint16_t x, uint16_t y, float num, uint8_t len,
                             uint16_t fc, uint16_t bc, uint8_t sizey);

/* 图片显示（支持分块大长度） */
LCD_Result LCD_ShowPicture(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t length, uint16_t width, const uint8_t pic[]);

#ifdef HOST_TEST
/** @brief 内部长数据分块传输辅助接口（仅在单元测试环境下公开用于 65,536 B 等精确边界验证） */
LCD_Result LCD_Writ_Bytes(LCD_Device *dev, const uint8_t *buf, uint32_t len);
#endif

#endif /* __GC9A01_H_ */
