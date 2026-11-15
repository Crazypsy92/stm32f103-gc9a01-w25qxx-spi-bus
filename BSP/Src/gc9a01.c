/**
 * @file    gc9a01.c
 * @brief   GC9A01 圆形 TFT 屏驱动（纯协议层，MCU 无关）
 * @details 驱动只依赖 gc9a01.h 中的 LCD_Port 回调表（片选/复位/DC/背光/延时/SPI），
 *          不包含任何 MCU、LL、HAL 头文件。引脚、外设与时钟配置全部在
 *          gc9a01_port.c 中完成，换平台时驱动零改动。
 *          Stage 2.1: 修复长度截断（支持分块传输大长度/全屏115200字节）、
 *          统一错误传播 (LCD_Result)、片选失败处理与参数边界防御。
 */
#include "gc9a01.h"
#include "lcdfont.h"

/* 行缓冲区：240 像素 * 2 字节 = 480 字节，静态分配防止栈溢出，MCU 无需大堆栈 */
static uint8_t lcd_line_buf[LCD_W * 2];

/* ================================================================================== */
/* 内部辅助与错误记录                                                                  */
/* ================================================================================== */

static inline LCD_Result LCD_SetError(LCD_Device *dev, LCD_Result err)
{
    if (dev != NULL && err != LCD_OK) {
        if (dev->last_error == LCD_OK) {
            dev->last_error = err; /* 保留首个故障原因 */
        }
    }
    return err;
}

LCD_Result LCD_GetLastError(const LCD_Device *dev)
{
    if (dev == NULL) {
        return LCD_ERROR_PARAM;
    }
    return dev->last_error;
}

void LCD_ClearError(LCD_Device *dev)
{
    if (dev != NULL) {
        dev->last_error = LCD_OK;
    }
}

static inline LCD_Result LCD_CS_LOW(LCD_Device *dev)
{
    if (dev == NULL || dev->port == NULL || dev->port->cs == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    LCD_Result res = dev->port->cs(dev->port->ctx, 0U);
    if (res != LCD_OK) {
        return LCD_SetError(dev, res);
    }
    return LCD_OK;
}

static inline LCD_Result LCD_CS_HIGH(LCD_Device *dev)
{
    if (dev == NULL || dev->port == NULL || dev->port->cs == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    LCD_Result res = dev->port->cs(dev->port->ctx, 1U);
    if (res != LCD_OK) {
        return LCD_SetError(dev, res);
    }
    return LCD_OK;
}

/**
 * @brief 发送单字节（不单独操作 CS，由外层保证片选）
 */
static LCD_Result LCD_Writ_Bus(LCD_Device *dev, uint8_t dat)
{
    if (dev == NULL || dev->port == NULL || dev->port->write == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    LCD_Result res = dev->port->write(dev->port->ctx, &dat, 1U);
    if (res != LCD_OK) {
        return LCD_SetError(dev, res);
    }
    return LCD_OK;
}

/**
 * @brief 连续发送多字节（支持任意长度，根据 Port 限制自动分块传输）
 * @note  分块期间 CS 和 DC 保持当前状态，任何分块失败中止后续传输并返回错误。
 */
#ifdef HOST_TEST
LCD_Result LCD_Writ_Bytes(LCD_Device *dev, const uint8_t *buf, uint32_t len)
#else
static LCD_Result LCD_Writ_Bytes(LCD_Device *dev, const uint8_t *buf, uint32_t len)
#endif
{
    if (dev == NULL || dev->port == NULL || dev->port->write == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    if (len == 0U) {
        return LCD_OK;
    }
    if (buf == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    /* Port 层未配置分块上限时默认以 65535 字节分块（满足 16 位传输限制） */
    uint16_t chunk_limit = (dev->port->max_chunk_len > 0U) ? dev->port->max_chunk_len : 65535U;

    while (len > 0U) {
        uint16_t chunk = (len > chunk_limit) ? chunk_limit : (uint16_t)len;
        LCD_Result res = dev->port->write(dev->port->ctx, buf, chunk);
        if (res != LCD_OK) {
            return LCD_SetError(dev, res);
        }
        buf += chunk;
        len -= chunk;
    }
    return LCD_OK;
}

/* ================================================================================== */
/* 基础写操作                                                                         */
/* ================================================================================== */

LCD_Result LCD_WR_REG(LCD_Device *dev, uint8_t dat)
{
    if (dev == NULL || dev->port == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    /* CS 申请失败不得继续发送屏幕命令 */
    LCD_Result cs_res = LCD_CS_LOW(dev);
    if (cs_res != LCD_OK) {
        return cs_res;
    }

    LCD_DC_CLR(dev);  /* 命令模式 */
    LCD_Result wr_res = LCD_Writ_Bus(dev, dat);
    LCD_DC_SET(dev);  /* 恢复数据模式 */

    /* CS 释放失败不得误报成功；但若前面传输已失败，保留首次故障原因 */
    LCD_Result rel_res = LCD_CS_HIGH(dev);
    if (wr_res != LCD_OK) {
        return wr_res;
    }
    if (rel_res != LCD_OK) {
        return rel_res;
    }
    return LCD_OK;
}

LCD_Result LCD_WR_DATA8(LCD_Device *dev, uint8_t dat)
{
    if (dev == NULL || dev->port == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    LCD_Result cs_res = LCD_CS_LOW(dev);
    if (cs_res != LCD_OK) {
        return cs_res;
    }

    LCD_DC_SET(dev);
    LCD_Result wr_res = LCD_Writ_Bus(dev, dat);
    LCD_Result rel_res = LCD_CS_HIGH(dev);

    if (wr_res != LCD_OK) {
        return wr_res;
    }
    if (rel_res != LCD_OK) {
        return rel_res;
    }
    return LCD_OK;
}

LCD_Result LCD_WR_DATA(LCD_Device *dev, uint16_t dat)
{
    if (dev == NULL || dev->port == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    LCD_Result cs_res = LCD_CS_LOW(dev);
    if (cs_res != LCD_OK) {
        return cs_res;
    }

    LCD_DC_SET(dev);
    uint8_t buf[2];
    buf[0] = (uint8_t)(dat >> 8);
    buf[1] = (uint8_t)(dat & 0xFF);
    LCD_Result wr_res = LCD_Writ_Bytes(dev, buf, 2U);
    LCD_Result rel_res = LCD_CS_HIGH(dev);

    if (wr_res != LCD_OK) {
        return wr_res;
    }
    if (rel_res != LCD_OK) {
        return rel_res;
    }
    return LCD_OK;
}

LCD_Result LCD_Address_Set(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    if (dev == NULL || dev->port == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    if (x1 > x2 || y1 > y2 || x2 >= dev->max_x || y2 >= dev->max_y) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    LCD_Result res;
    res = LCD_WR_REG(dev, 0x2A);    /* 列地址设置 */
    if (res != LCD_OK) return res;
    res = LCD_WR_DATA(dev, x1);
    if (res != LCD_OK) return res;
    res = LCD_WR_DATA(dev, x2);
    if (res != LCD_OK) return res;

    res = LCD_WR_REG(dev, 0x2B);    /* 行地址设置 */
    if (res != LCD_OK) return res;
    res = LCD_WR_DATA(dev, y1);
    if (res != LCD_OK) return res;
    res = LCD_WR_DATA(dev, y2);
    if (res != LCD_OK) return res;

    res = LCD_WR_REG(dev, 0x2C);    /* 存储器写 */
    return res;
}

/* ================================================================================== */
/* GC9A01 初始化                                                                      */
/* ================================================================================== */

#define LCD_INIT_REG(cmd) do { \
    LCD_Result _r = LCD_WR_REG(dev, (cmd)); \
    if (_r != LCD_OK) return _r; \
} while (0)

#define LCD_INIT_DATA8(val) do { \
    LCD_Result _r = LCD_WR_DATA8(dev, (val)); \
    if (_r != LCD_OK) return _r; \
} while (0)

LCD_Result GC9A01_Init(LCD_Device *dev, const LCD_Port *port)
{
    if (dev == NULL || port == NULL || port->cs == NULL ||
        port->rst == NULL || port->dc == NULL ||
        port->delay_ms == NULL || port->write == NULL) {
        return (dev != NULL) ? LCD_SetError(dev, LCD_ERROR_PARAM) : LCD_ERROR_PARAM;
    }

    /* 1. 绑定抽象端口与屏幕参数 */
    dev->port       = port;
    dev->max_x      = LCD_W;
    dev->max_y      = LCD_H;
    dev->last_error = LCD_OK;

    /* 2. 复位时序 */
    LCD_RST_CLR(dev);
    LCD_DELAY_MS(dev, 100);
    LCD_RST_SET(dev);
    LCD_DELAY_MS(dev, 100);

    LCD_BL_OFF(dev);
    LCD_DELAY_MS(dev, 100);

    /* 3. GC9A01 初始化序列（失败立即返回） */
    LCD_INIT_REG(0xEF);
    LCD_INIT_REG(0xEB);
    LCD_INIT_DATA8(0x14);

    LCD_INIT_REG(0xFE);
    LCD_INIT_REG(0xEF);

    LCD_INIT_REG(0xEB);
    LCD_INIT_DATA8(0x14);

    LCD_INIT_REG(0x84);
    LCD_INIT_DATA8(0x40);

    LCD_INIT_REG(0x85);
    LCD_INIT_DATA8(0xFF);

    LCD_INIT_REG(0x86);
    LCD_INIT_DATA8(0xFF);

    LCD_INIT_REG(0x87);
    LCD_INIT_DATA8(0xFF);

    LCD_INIT_REG(0x88);
    LCD_INIT_DATA8(0x0A);

    LCD_INIT_REG(0x89);
    LCD_INIT_DATA8(0x21);

    LCD_INIT_REG(0x8A);
    LCD_INIT_DATA8(0x00);

    LCD_INIT_REG(0x8B);
    LCD_INIT_DATA8(0x80);

    LCD_INIT_REG(0x8C);
    LCD_INIT_DATA8(0x01);

    LCD_INIT_REG(0x8D);
    LCD_INIT_DATA8(0x01);

    LCD_INIT_REG(0x8E);
    LCD_INIT_DATA8(0xFF);

    LCD_INIT_REG(0x8F);
    LCD_INIT_DATA8(0xFF);

    LCD_INIT_REG(0xB6);
    LCD_INIT_DATA8(0x00);
    LCD_INIT_DATA8(0x20);

    LCD_INIT_REG(0x36);          /* MADCTL：显示方向 */
#if (LCD_ROTATION == 0)
    LCD_INIT_DATA8(0x08);        /* 0° */
#elif (LCD_ROTATION == 90)
    LCD_INIT_DATA8(0x68);        /* 90° */
#elif (LCD_ROTATION == 180)
    LCD_INIT_DATA8(0xC8);        /* 180° */
#elif (LCD_ROTATION == 270)
    LCD_INIT_DATA8(0xA8);        /* 270° */
#else
#error "LCD_ROTATION 仅支持 0 / 90 / 180 / 270"
#endif

    LCD_INIT_REG(0x3A);
    LCD_INIT_DATA8(0x05);        /* 16 位 RGB565 颜色模式 */

    LCD_INIT_REG(0x90);
    LCD_INIT_DATA8(0x08);
    LCD_INIT_DATA8(0x08);
    LCD_INIT_DATA8(0x08);
    LCD_INIT_DATA8(0x08);

    LCD_INIT_REG(0xBD);
    LCD_INIT_DATA8(0x06);

    LCD_INIT_REG(0xBC);
    LCD_INIT_DATA8(0x00);

    LCD_INIT_REG(0xFF);
    LCD_INIT_DATA8(0x60);
    LCD_INIT_DATA8(0x01);
    LCD_INIT_DATA8(0x04);

    LCD_INIT_REG(0xC3);
    LCD_INIT_DATA8(0x13);
    LCD_INIT_REG(0xC4);
    LCD_INIT_DATA8(0x13);

    LCD_INIT_REG(0xC9);
    LCD_INIT_DATA8(0x22);

    LCD_INIT_REG(0xBE);
    LCD_INIT_DATA8(0x11);

    LCD_INIT_REG(0xE1);
    LCD_INIT_DATA8(0x10);
    LCD_INIT_DATA8(0x0E);

    LCD_INIT_REG(0xDF);
    LCD_INIT_DATA8(0x21);
    LCD_INIT_DATA8(0x0C);
    LCD_INIT_DATA8(0x02);

    LCD_INIT_REG(0xF0);
    LCD_INIT_DATA8(0x45);
    LCD_INIT_DATA8(0x09);
    LCD_INIT_DATA8(0x08);
    LCD_INIT_DATA8(0x08);
    LCD_INIT_DATA8(0x26);
    LCD_INIT_DATA8(0x2A);

    LCD_INIT_REG(0xF1);
    LCD_INIT_DATA8(0x43);
    LCD_INIT_DATA8(0x70);
    LCD_INIT_DATA8(0x72);
    LCD_INIT_DATA8(0x36);
    LCD_INIT_DATA8(0x37);
    LCD_INIT_DATA8(0x6F);

    LCD_INIT_REG(0xF2);
    LCD_INIT_DATA8(0x45);
    LCD_INIT_DATA8(0x09);
    LCD_INIT_DATA8(0x08);
    LCD_INIT_DATA8(0x08);
    LCD_INIT_DATA8(0x26);
    LCD_INIT_DATA8(0x2A);

    LCD_INIT_REG(0xF3);
    LCD_INIT_DATA8(0x43);
    LCD_INIT_DATA8(0x70);
    LCD_INIT_DATA8(0x72);
    LCD_INIT_DATA8(0x36);
    LCD_INIT_DATA8(0x37);
    LCD_INIT_DATA8(0x6F);

    LCD_INIT_REG(0xED);
    LCD_INIT_DATA8(0x1B);
    LCD_INIT_DATA8(0x0B);

    LCD_INIT_REG(0xAE);
    LCD_INIT_DATA8(0x77);

    LCD_INIT_REG(0xCD);
    LCD_INIT_DATA8(0x63);

    LCD_INIT_REG(0x70);
    LCD_INIT_DATA8(0x07);
    LCD_INIT_DATA8(0x07);
    LCD_INIT_DATA8(0x04);
    LCD_INIT_DATA8(0x0E);
    LCD_INIT_DATA8(0x0F);
    LCD_INIT_DATA8(0x09);
    LCD_INIT_DATA8(0x07);
    LCD_INIT_DATA8(0x08);
    LCD_INIT_DATA8(0x03);

    LCD_INIT_REG(0xE8);
    LCD_INIT_DATA8(0x34);

    LCD_INIT_REG(0x62);
    LCD_INIT_DATA8(0x18);
    LCD_INIT_DATA8(0x0D);
    LCD_INIT_DATA8(0x71);
    LCD_INIT_DATA8(0xED);
    LCD_INIT_DATA8(0x70);
    LCD_INIT_DATA8(0x70);
    LCD_INIT_DATA8(0x18);
    LCD_INIT_DATA8(0x0F);
    LCD_INIT_DATA8(0x71);
    LCD_INIT_DATA8(0xEF);
    LCD_INIT_DATA8(0x70);
    LCD_INIT_DATA8(0x70);

    LCD_INIT_REG(0x63);
    LCD_INIT_DATA8(0x18);
    LCD_INIT_DATA8(0x11);
    LCD_INIT_DATA8(0x71);
    LCD_INIT_DATA8(0xF1);
    LCD_INIT_DATA8(0x70);
    LCD_INIT_DATA8(0x70);
    LCD_INIT_DATA8(0x18);
    LCD_INIT_DATA8(0x13);
    LCD_INIT_DATA8(0x71);
    LCD_INIT_DATA8(0xF3);
    LCD_INIT_DATA8(0x70);
    LCD_INIT_DATA8(0x70);

    LCD_INIT_REG(0x64);
    LCD_INIT_DATA8(0x28);
    LCD_INIT_DATA8(0x29);
    LCD_INIT_DATA8(0xF1);
    LCD_INIT_DATA8(0x01);
    LCD_INIT_DATA8(0xF1);
    LCD_INIT_DATA8(0x00);
    LCD_INIT_DATA8(0x07);

    LCD_INIT_REG(0x66);
    LCD_INIT_DATA8(0x3C);
    LCD_INIT_DATA8(0x00);
    LCD_INIT_DATA8(0xCD);
    LCD_INIT_DATA8(0x67);
    LCD_INIT_DATA8(0x45);
    LCD_INIT_DATA8(0x45);
    LCD_INIT_DATA8(0x10);
    LCD_INIT_DATA8(0x00);
    LCD_INIT_DATA8(0x00);
    LCD_INIT_DATA8(0x00);

    LCD_INIT_REG(0x67);
    LCD_INIT_DATA8(0x00);
    LCD_INIT_DATA8(0x3C);
    LCD_INIT_DATA8(0x00);
    LCD_INIT_DATA8(0x00);
    LCD_INIT_DATA8(0x00);
    LCD_INIT_DATA8(0x01);
    LCD_INIT_DATA8(0x54);
    LCD_INIT_DATA8(0x10);
    LCD_INIT_DATA8(0x32);
    LCD_INIT_DATA8(0x98);

    LCD_INIT_REG(0x74);
    LCD_INIT_DATA8(0x10);
    LCD_INIT_DATA8(0x85);
    LCD_INIT_DATA8(0x80);
    LCD_INIT_DATA8(0x00);
    LCD_INIT_DATA8(0x00);
    LCD_INIT_DATA8(0x4E);
    LCD_INIT_DATA8(0x00);

    LCD_INIT_REG(0x98);
    LCD_INIT_DATA8(0x3E);
    LCD_INIT_DATA8(0x07);

    LCD_INIT_REG(0x35);
    LCD_INIT_REG(0x21);

    LCD_INIT_REG(0x11);   /* 退出睡眠 */
    LCD_DELAY_MS(dev, 120);
    LCD_INIT_REG(0x29);   /* 开启显示 */
    LCD_DELAY_MS(dev, 20);

    /* 清屏 */
    LCD_Result fill_res = LCD_Fill(dev, 0, 0, LCD_W, LCD_H, BLACK);
    if (fill_res != LCD_OK) {
        return fill_res;
    }

    LCD_BL_ON(dev);
    return LCD_OK;
}

/* ================================================================================== */
/* 图形绘制函数                                                                        */
/* ================================================================================== */

LCD_Result LCD_Fill(LCD_Device *dev, uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color)
{
    if (dev == NULL || dev->port == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    /* 填充区域防御：检查零宽、零高及越界坐标 */
    if (xsta >= xend || ysta >= yend) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    if (xend > dev->max_x || yend > dev->max_y) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    LCD_Result res = LCD_Address_Set(dev, xsta, ysta, xend - 1U, yend - 1U);
    if (res != LCD_OK) {
        return res;
    }

    /* 预填充行缓冲 */
    for (uint32_t i = 0; i < sizeof(lcd_line_buf); i += 2U) {
        lcd_line_buf[i]     = (uint8_t)(color >> 8);
        lcd_line_buf[i + 1] = (uint8_t)(color & 0xFF);
    }

    uint32_t remain = (uint32_t)(xend - xsta) * (yend - ysta);   /* 剩余像素数 */

    LCD_Result cs_res = LCD_CS_LOW(dev);
    if (cs_res != LCD_OK) {
        return cs_res;
    }
    LCD_DC_SET(dev);

    LCD_Result wr_res = LCD_OK;
    while (remain > 0U) {
        uint32_t pixels = (remain > LCD_W) ? LCD_W : remain;
        wr_res = LCD_Writ_Bytes(dev, lcd_line_buf, pixels * 2U);
        if (wr_res != LCD_OK) {
            break;
        }
        remain -= pixels;
    }

    LCD_Result rel_res = LCD_CS_HIGH(dev);
    if (wr_res != LCD_OK) return wr_res;
    if (rel_res != LCD_OK) return rel_res;
    return LCD_OK;
}

LCD_Result LCD_DrawPoint(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t color)
{
    if (dev == NULL || dev->port == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    if (x >= dev->max_x || y >= dev->max_y) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    LCD_Result res = LCD_Address_Set(dev, x, y, x, y);
    if (res != LCD_OK) return res;
    return LCD_WR_DATA(dev, color);
}

LCD_Result LCD_DrawLine(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    if (dev == NULL || dev->port == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    uint16_t t;
    int xerr = 0, yerr = 0, delta_x, delta_y, distance;
    int incx, incy, uRow, uCol;
    delta_x = (int)x2 - (int)x1;
    delta_y = (int)y2 - (int)y1;
    uRow = x1;
    uCol = y1;

    if (delta_x > 0) incx = 1;
    else if (delta_x == 0) incx = 0;
    else { incx = -1; delta_x = -delta_x; }

    if (delta_y > 0) incy = 1;
    else if (delta_y == 0) incy = 0;
    else { incy = -1; delta_y = -delta_y; }

    if (delta_x > delta_y) distance = delta_x;
    else distance = delta_y;

    for (t = 0; t <= distance + 1; t++) {
        if (uRow >= 0 && uCol >= 0 && (uint16_t)uRow < dev->max_x && (uint16_t)uCol < dev->max_y) {
            LCD_Result res = LCD_DrawPoint(dev, (uint16_t)uRow, (uint16_t)uCol, color);
            if (res != LCD_OK) {
                return res;
            }
        }
        xerr += delta_x;
        yerr += delta_y;
        if (xerr > distance) {
            xerr -= distance;
            uRow += incx;
        }
        if (yerr > distance) {
            yerr -= distance;
            uCol += incy;
        }
    }
    return LCD_OK;
}

LCD_Result LCD_DrawRectangle(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    if (dev == NULL || dev->port == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    if (x1 > x2 || y1 > y2 || x2 >= dev->max_x || y2 >= dev->max_y) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    LCD_Result res;
    res = LCD_DrawLine(dev, x1, y1, x2, y1, color);
    if (res != LCD_OK) return res;
    res = LCD_DrawLine(dev, x1, y1, x1, y2, color);
    if (res != LCD_OK) return res;
    res = LCD_DrawLine(dev, x1, y2, x2, y2, color);
    if (res != LCD_OK) return res;
    res = LCD_DrawLine(dev, x2, y1, x2, y2, color);
    if (res != LCD_OK) return res;
    return LCD_OK;
}

LCD_Result Draw_Circle(LCD_Device *dev, uint16_t x0, uint16_t y0, uint8_t r, uint16_t color)
{
    if (dev == NULL || dev->port == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    int a = 0, b = r;
    int di = 3 - (r << 1);
    LCD_Result res;

    while (a <= b) {
        if (x0 >= b && y0 >= a) {
            res = LCD_DrawPoint(dev, x0 - b, y0 - a, color);
            if (res != LCD_OK) return res;
        }
        res = LCD_DrawPoint(dev, x0 + b, y0 - a, color);
        if (res != LCD_OK) return res;
        if (x0 >= a && y0 >= b) {
            res = LCD_DrawPoint(dev, x0 - a, y0 + b, color);
            if (res != LCD_OK) return res;
        }
        if (x0 >= a && y0 >= b) {
            res = LCD_DrawPoint(dev, x0 - a, y0 - b, color);
            if (res != LCD_OK) return res;
        }
        res = LCD_DrawPoint(dev, x0 + b, y0 + a, color);
        if (res != LCD_OK) return res;
        if (x0 >= b) {
            res = LCD_DrawPoint(dev, x0 + a, y0 - b, color);
            if (res != LCD_OK) return res;
        }
        res = LCD_DrawPoint(dev, x0 + a, y0 + b, color);
        if (res != LCD_OK) return res;
        if (x0 >= b) {
            res = LCD_DrawPoint(dev, x0 - b, y0 + a, color);
            if (res != LCD_OK) return res;
        }
        a++;
        if (di < 0) {
            di += 4 * a + 6;
        } else {
            di += 10 + 4 * (a - b);
            b--;
        }
    }
    return LCD_OK;
}

/* ================================================================================== */
/* 字符与数字显示                                                                      */
/* ================================================================================== */

LCD_Result LCD_ShowChar(LCD_Device *dev, uint16_t x, uint16_t y, uint8_t num,
                        uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode)
{
    if (dev == NULL || dev->port == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    /* 1. 字号安全检查：仅支持 12, 16, 24, 32 */
    if (sizey != 12U && sizey != 16U && sizey != 24U && sizey != 32U) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    /* 2. ASCII 范围检查：仅支持 ' ' (32) ~ '~' (126) */
    if (num < ' ' || num > '~') {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    uint8_t sizex = sizey / 2U;

    /* 3. 屏幕边界防御：防止字符坐标或矩形越界 */
    if (x >= dev->max_x || y >= dev->max_y ||
        ((uint32_t)x + sizex) > dev->max_x ||
        ((uint32_t)y + sizey) > dev->max_y) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    uint8_t font_index = num - ' ';
    uint16_t typeface_num = (sizex / 8U + ((sizex % 8U) ? 1U : 0U)) * sizey;

    if (!mode) {
        /* 非叠加模式：设置矩形窗口，连续写入前景色/背景色 */
        LCD_Result addr_res = LCD_Address_Set(dev, x, y, x + sizex - 1U, y + sizey - 1U);
        if (addr_res != LCD_OK) return addr_res;

        LCD_Result cs_res = LCD_CS_LOW(dev);
        if (cs_res != LCD_OK) return cs_res;
        LCD_DC_SET(dev);

        LCD_Result wr_res = LCD_OK;
        uint8_t m = 0;

        for (uint16_t i = 0; i < typeface_num; i++) {
            uint8_t temp;
            if (sizey == 12U)      temp = ascii_1206[font_index][i];
            else if (sizey == 16U) temp = ascii_1608[font_index][i];
            else if (sizey == 24U) temp = ascii_2412[font_index][i];
            else                   temp = ascii_3216[font_index][i];

            for (uint8_t t = 0; t < 8U; t++) {
                uint16_t color = (temp & (0x01U << t)) ? fc : bc;
                uint8_t pix[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
                wr_res = LCD_Writ_Bytes(dev, pix, 2U);
                if (wr_res != LCD_OK) {
                    break;
                }
                m++;
                if (m % sizex == 0U) {
                    m = 0U;
                    break;
                }
            }
            if (wr_res != LCD_OK) break;
        }

        LCD_Result rel_res = LCD_CS_HIGH(dev);
        if (wr_res != LCD_OK) return wr_res;
        if (rel_res != LCD_OK) return rel_res;
    } else {
        /* 叠加模式：仅绘制前景色有效点，不破坏背景 */
        uint16_t x0 = x;
        for (uint16_t i = 0; i < typeface_num; i++) {
            uint8_t temp;
            if (sizey == 12U)      temp = ascii_1206[font_index][i];
            else if (sizey == 16U) temp = ascii_1608[font_index][i];
            else if (sizey == 24U) temp = ascii_2412[font_index][i];
            else                   temp = ascii_3216[font_index][i];

            for (uint8_t t = 0; t < 8U; t++) {
                if (temp & (0x01U << t)) {
                    LCD_Result p_res = LCD_DrawPoint(dev, x, y, fc);
                    if (p_res != LCD_OK) return p_res;
                }
                x++;
                if ((x - x0) == sizex) {
                    x = x0;
                    y++;
                    break;
                }
            }
        }
    }
    return LCD_OK;
}

LCD_Result LCD_ShowString(LCD_Device *dev, uint16_t x, uint16_t y, const char *p,
                          uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode)
{
    if (dev == NULL || p == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    if (sizey != 12U && sizey != 16U && sizey != 24U && sizey != 32U) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    uint8_t sizex = sizey / 2U;

    while (*p != '\0') {
        /* 超出屏幕边界，安全裁剪停止绘制 */
        if (((uint32_t)x + sizex) > dev->max_x || ((uint32_t)y + sizey) > dev->max_y) {
            break;
        }
        LCD_Result res = LCD_ShowChar(dev, x, y, (uint8_t)*p, fc, bc, sizey, mode);
        if (res != LCD_OK) {
            return res; /* 失败立即停止并返回错误 */
        }
        x += sizex;
        p++;
    }
    return LCD_OK;
}

uint32_t mypow(uint8_t m, uint8_t n)
{
    uint32_t result = 1U;
    while (n--) result *= m;
    return result;
}

LCD_Result LCD_ShowIntNum(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t num, uint8_t len,
                          uint16_t fc, uint16_t bc, uint8_t sizey)
{
    if (dev == NULL || len == 0U || len > 10U) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    if (sizey != 12U && sizey != 16U && sizey != 24U && sizey != 32U) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    uint8_t sizex = sizey / 2U;
    uint8_t enshow = 0U;

    for (uint8_t t = 0U; t < len; t++) {
        if (((uint32_t)x + (uint32_t)t * sizex + sizex) > dev->max_x) {
            break; /* 字符越界安全截断 */
        }
        uint8_t temp = (num / mypow(10U, len - t - 1U)) % 10U;
        if (enshow == 0U && t < (len - 1U)) {
            if (temp == 0U) {
                LCD_Result r = LCD_ShowChar(dev, x + t * sizex, y, ' ', fc, bc, sizey, 0U);
                if (r != LCD_OK) return r;
                continue;
            } else {
                enshow = 1U;
            }
        }
        LCD_Result r = LCD_ShowChar(dev, x + t * sizex, y, temp + '0', fc, bc, sizey, 0U);
        if (r != LCD_OK) return r;
    }
    return LCD_OK;
}

LCD_Result LCD_ShowFloatNum1(LCD_Device *dev, uint16_t x, uint16_t y, float num, uint8_t len,
                             uint16_t fc, uint16_t bc, uint8_t sizey)
{
    if (dev == NULL || len == 0U) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    if (sizey != 12U && sizey != 16U && sizey != 24U && sizey != 32U) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    uint8_t sizex = sizey / 2U;
    LCD_Result res;

    if (num < 0.0f) {
        if (((uint32_t)x + sizex) <= dev->max_x) {
            res = LCD_ShowChar(dev, x, y, '-', fc, bc, sizey, 0U);
            if (res != LCD_OK) return res;
        }
        x += sizex;
        num = -num;
    }

    int32_t int_part = (int32_t)num;
    uint16_t dec = (uint16_t)((num - (float)int_part) * 10.0f + 0.5f);
    if (dec > 9U) {
        int_part++;
        dec = 0U;
    }

    res = LCD_ShowIntNum(dev, x, y, (uint16_t)int_part, len, fc, bc, sizey);
    if (res != LCD_OK) return res;
    x += len * sizex;

    if (((uint32_t)x + sizex) <= dev->max_x) {
        res = LCD_ShowChar(dev, x, y, '.', fc, bc, sizey, 0U);
        if (res != LCD_OK) return res;
    }
    if (((uint32_t)x + 2U * sizex) <= dev->max_x) {
        res = LCD_ShowChar(dev, x + sizex, y, (uint8_t)(dec + '0'), fc, bc, sizey, 0U);
        if (res != LCD_OK) return res;
    }
    return LCD_OK;
}

/* ================================================================================== */
/* 图片显示（支持分块大长度）                                                         */
/* ================================================================================== */

LCD_Result LCD_ShowPicture(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t length, uint16_t width, const uint8_t pic[])
{
    if (dev == NULL || dev->port == NULL || pic == NULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    if (length == 0U || width == 0U) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    /* 坐标与尺寸边界防御 */
    if (((uint32_t)x + length) > dev->max_x || ((uint32_t)y + width) > dev->max_y) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }

    /* 乘法溢出防御：length * width * 2 是否超出 32 位表示范围 */
    uint64_t total_bytes_64 = (uint64_t)length * (uint64_t)width * 2ULL;
    if (total_bytes_64 > 0xFFFFFFFFULL) {
        return LCD_SetError(dev, LCD_ERROR_PARAM);
    }
    uint32_t total_bytes = (uint32_t)total_bytes_64;

    LCD_Result addr_res = LCD_Address_Set(dev, x, y, x + length - 1U, y + width - 1U);
    if (addr_res != LCD_OK) {
        return addr_res;
    }

    LCD_Result cs_res = LCD_CS_LOW(dev);
    if (cs_res != LCD_OK) {
        return cs_res;
    }
    LCD_DC_SET(dev);

    /* 连续发送图片像素数据（由 LCD_Writ_Bytes 内部根据 Port 分块上限自动分段） */
    LCD_Result wr_res = LCD_Writ_Bytes(dev, pic, total_bytes);
    LCD_Result rel_res = LCD_CS_HIGH(dev);

    if (wr_res != LCD_OK) return wr_res;
    if (rel_res != LCD_OK) return rel_res;
    return LCD_OK;
}
