/**
 * @file    GC9A01.c
 * @brief   GC9A01 圆形 TFT 屏驱动（纯协议层，MCU 无关）
 * @details 驱动只依赖 GC9A01.h 中的 LCD_Port 回调表（片选/复位/DC/背光/延时/SPI），
 *          不包含任何 MCU、LL、HAL 头文件。引脚、外设与时钟配置全部在
 *          GC9A01_port.c 中完成，换平台时驱动零改动。
 *          适配说明：原工程引用的 MyApplication.h / LL_delay_ms / DMA 分支
 *          已全部移除，改为抽象端口回调。
 */
#include "gc9a01.h"
#include "lcdfont.h"

// 行缓冲区：240 像素 * 2 字节 = 480 字节，放在静态区防止栈溢出
static uint8_t lcd_line_buf[LCD_W * 2];

/**
 * @brief 发送单字节（不操作 CS，由调用者保证片选）
 */
static void LCD_Writ_Bus(LCD_Device *dev, uint8_t dat)
{
    dev->port->write(dev->port->ctx, &dat, 1);
}

/**
 * @brief 连续发送多字节（片选保持有效，提升填充/图片刷新速度）
 */
static void LCD_Writ_Bytes(LCD_Device *dev, const uint8_t *buf, uint32_t len)
{
    if (len > 0) {
        dev->port->write(dev->port->ctx, buf, (uint16_t)len);
    }
}

// --------------------------------------------------
// GC9A01 初始化
// --------------------------------------------------
void GC9A01_Init(LCD_Device *dev, const LCD_Port *port)
{
    // 1. 绑定抽象端口（GPIO / SPI 已由 port 层初始化完成）
    dev->port  = port;
    dev->max_x = LCD_W;
    dev->max_y = LCD_H;

    // 2. 复位
    LCD_RST_CLR(dev);
    LCD_DELAY_MS(dev, 100);
    LCD_RST_SET(dev);
    LCD_DELAY_MS(dev, 100);

    LCD_BL_OFF(dev);
    LCD_DELAY_MS(dev, 100);

    // 5. GC9A01 初始化序列
    LCD_WR_REG(dev, 0xEF);
    LCD_WR_REG(dev, 0xEB);
    LCD_WR_DATA8(dev, 0x14);

    LCD_WR_REG(dev, 0xFE);
    LCD_WR_REG(dev, 0xEF);

    LCD_WR_REG(dev, 0xEB);
    LCD_WR_DATA8(dev, 0x14);

    LCD_WR_REG(dev, 0x84);
    LCD_WR_DATA8(dev, 0x40);

    LCD_WR_REG(dev, 0x85);
    LCD_WR_DATA8(dev, 0xFF);

    LCD_WR_REG(dev, 0x86);
    LCD_WR_DATA8(dev, 0xFF);

    LCD_WR_REG(dev, 0x87);
    LCD_WR_DATA8(dev, 0xFF);

    LCD_WR_REG(dev, 0x88);
    LCD_WR_DATA8(dev, 0x0A);

    LCD_WR_REG(dev, 0x89);
    LCD_WR_DATA8(dev, 0x21);

    LCD_WR_REG(dev, 0x8A);
    LCD_WR_DATA8(dev, 0x00);

    LCD_WR_REG(dev, 0x8B);
    LCD_WR_DATA8(dev, 0x80);

    LCD_WR_REG(dev, 0x8C);
    LCD_WR_DATA8(dev, 0x01);

    LCD_WR_REG(dev, 0x8D);
    LCD_WR_DATA8(dev, 0x01);

    LCD_WR_REG(dev, 0x8E);
    LCD_WR_DATA8(dev, 0xFF);

    LCD_WR_REG(dev, 0x8F);
    LCD_WR_DATA8(dev, 0xFF);

    LCD_WR_REG(dev, 0xB6);
    LCD_WR_DATA8(dev, 0x00);
    LCD_WR_DATA8(dev, 0x20);

    LCD_WR_REG(dev, 0x36);          // MADCTL：显示方向
#if (LCD_ROTATION == 0)
    LCD_WR_DATA8(dev, 0x08);        // 0°
#elif (LCD_ROTATION == 90)
    LCD_WR_DATA8(dev, 0x68);        // 90°
#elif (LCD_ROTATION == 180)
    LCD_WR_DATA8(dev, 0xC8);        // 180°
#elif (LCD_ROTATION == 270)
    LCD_WR_DATA8(dev, 0xA8);        // 270°
#else
#error "LCD_ROTATION 仅支持 0 / 90 / 180 / 270"
#endif

    LCD_WR_REG(dev, 0x3A);
    LCD_WR_DATA8(dev, 0x05);   // 16 位颜色模式

    LCD_WR_REG(dev, 0x90);
    LCD_WR_DATA8(dev, 0x08);
    LCD_WR_DATA8(dev, 0x08);
    LCD_WR_DATA8(dev, 0x08);
    LCD_WR_DATA8(dev, 0x08);

    LCD_WR_REG(dev, 0xBD);
    LCD_WR_DATA8(dev, 0x06);

    LCD_WR_REG(dev, 0xBC);
    LCD_WR_DATA8(dev, 0x00);

    LCD_WR_REG(dev, 0xFF);
    LCD_WR_DATA8(dev, 0x60);
    LCD_WR_DATA8(dev, 0x01);
    LCD_WR_DATA8(dev, 0x04);

    LCD_WR_REG(dev, 0xC3);
    LCD_WR_DATA8(dev, 0x13);
    LCD_WR_REG(dev, 0xC4);
    LCD_WR_DATA8(dev, 0x13);

    LCD_WR_REG(dev, 0xC9);
    LCD_WR_DATA8(dev, 0x22);

    LCD_WR_REG(dev, 0xBE);
    LCD_WR_DATA8(dev, 0x11);

    LCD_WR_REG(dev, 0xE1);
    LCD_WR_DATA8(dev, 0x10);
    LCD_WR_DATA8(dev, 0x0E);

    LCD_WR_REG(dev, 0xDF);
    LCD_WR_DATA8(dev, 0x21);
    LCD_WR_DATA8(dev, 0x0C);
    LCD_WR_DATA8(dev, 0x02);

    LCD_WR_REG(dev, 0xF0);
    LCD_WR_DATA8(dev, 0x45);
    LCD_WR_DATA8(dev, 0x09);
    LCD_WR_DATA8(dev, 0x08);
    LCD_WR_DATA8(dev, 0x08);
    LCD_WR_DATA8(dev, 0x26);
    LCD_WR_DATA8(dev, 0x2A);

    LCD_WR_REG(dev, 0xF1);
    LCD_WR_DATA8(dev, 0x43);
    LCD_WR_DATA8(dev, 0x70);
    LCD_WR_DATA8(dev, 0x72);
    LCD_WR_DATA8(dev, 0x36);
    LCD_WR_DATA8(dev, 0x37);
    LCD_WR_DATA8(dev, 0x6F);

    LCD_WR_REG(dev, 0xF2);
    LCD_WR_DATA8(dev, 0x45);
    LCD_WR_DATA8(dev, 0x09);
    LCD_WR_DATA8(dev, 0x08);
    LCD_WR_DATA8(dev, 0x08);
    LCD_WR_DATA8(dev, 0x26);
    LCD_WR_DATA8(dev, 0x2A);

    LCD_WR_REG(dev, 0xF3);
    LCD_WR_DATA8(dev, 0x43);
    LCD_WR_DATA8(dev, 0x70);
    LCD_WR_DATA8(dev, 0x72);
    LCD_WR_DATA8(dev, 0x36);
    LCD_WR_DATA8(dev, 0x37);
    LCD_WR_DATA8(dev, 0x6F);

    LCD_WR_REG(dev, 0xED);
    LCD_WR_DATA8(dev, 0x1B);
    LCD_WR_DATA8(dev, 0x0B);

    LCD_WR_REG(dev, 0xAE);
    LCD_WR_DATA8(dev, 0x77);

    LCD_WR_REG(dev, 0xCD);
    LCD_WR_DATA8(dev, 0x63);

    LCD_WR_REG(dev, 0x70);
    LCD_WR_DATA8(dev, 0x07);
    LCD_WR_DATA8(dev, 0x07);
    LCD_WR_DATA8(dev, 0x04);
    LCD_WR_DATA8(dev, 0x0E);
    LCD_WR_DATA8(dev, 0x0F);
    LCD_WR_DATA8(dev, 0x09);
    LCD_WR_DATA8(dev, 0x07);
    LCD_WR_DATA8(dev, 0x08);
    LCD_WR_DATA8(dev, 0x03);

    LCD_WR_REG(dev, 0xE8);
    LCD_WR_DATA8(dev, 0x34);

    LCD_WR_REG(dev, 0x62);
    LCD_WR_DATA8(dev, 0x18);
    LCD_WR_DATA8(dev, 0x0D);
    LCD_WR_DATA8(dev, 0x71);
    LCD_WR_DATA8(dev, 0xED);
    LCD_WR_DATA8(dev, 0x70);
    LCD_WR_DATA8(dev, 0x70);
    LCD_WR_DATA8(dev, 0x18);
    LCD_WR_DATA8(dev, 0x0F);
    LCD_WR_DATA8(dev, 0x71);
    LCD_WR_DATA8(dev, 0xEF);
    LCD_WR_DATA8(dev, 0x70);
    LCD_WR_DATA8(dev, 0x70);

    LCD_WR_REG(dev, 0x63);
    LCD_WR_DATA8(dev, 0x18);
    LCD_WR_DATA8(dev, 0x11);
    LCD_WR_DATA8(dev, 0x71);
    LCD_WR_DATA8(dev, 0xF1);
    LCD_WR_DATA8(dev, 0x70);
    LCD_WR_DATA8(dev, 0x70);
    LCD_WR_DATA8(dev, 0x18);
    LCD_WR_DATA8(dev, 0x13);
    LCD_WR_DATA8(dev, 0x71);
    LCD_WR_DATA8(dev, 0xF3);
    LCD_WR_DATA8(dev, 0x70);
    LCD_WR_DATA8(dev, 0x70);

    LCD_WR_REG(dev, 0x64);
    LCD_WR_DATA8(dev, 0x28);
    LCD_WR_DATA8(dev, 0x29);
    LCD_WR_DATA8(dev, 0xF1);
    LCD_WR_DATA8(dev, 0x01);
    LCD_WR_DATA8(dev, 0xF1);
    LCD_WR_DATA8(dev, 0x00);
    LCD_WR_DATA8(dev, 0x07);

    LCD_WR_REG(dev, 0x66);
    LCD_WR_DATA8(dev, 0x3C);
    LCD_WR_DATA8(dev, 0x00);
    LCD_WR_DATA8(dev, 0xCD);
    LCD_WR_DATA8(dev, 0x67);
    LCD_WR_DATA8(dev, 0x45);
    LCD_WR_DATA8(dev, 0x45);
    LCD_WR_DATA8(dev, 0x10);
    LCD_WR_DATA8(dev, 0x00);
    LCD_WR_DATA8(dev, 0x00);
    LCD_WR_DATA8(dev, 0x00);

    LCD_WR_REG(dev, 0x67);
    LCD_WR_DATA8(dev, 0x00);
    LCD_WR_DATA8(dev, 0x3C);
    LCD_WR_DATA8(dev, 0x00);
    LCD_WR_DATA8(dev, 0x00);
    LCD_WR_DATA8(dev, 0x00);
    LCD_WR_DATA8(dev, 0x01);
    LCD_WR_DATA8(dev, 0x54);
    LCD_WR_DATA8(dev, 0x10);
    LCD_WR_DATA8(dev, 0x32);
    LCD_WR_DATA8(dev, 0x98);

    LCD_WR_REG(dev, 0x74);
    LCD_WR_DATA8(dev, 0x10);
    LCD_WR_DATA8(dev, 0x85);
    LCD_WR_DATA8(dev, 0x80);
    LCD_WR_DATA8(dev, 0x00);
    LCD_WR_DATA8(dev, 0x00);
    LCD_WR_DATA8(dev, 0x4E);
    LCD_WR_DATA8(dev, 0x00);

    LCD_WR_REG(dev, 0x98);
    LCD_WR_DATA8(dev, 0x3E);
    LCD_WR_DATA8(dev, 0x07);

    LCD_WR_REG(dev, 0x35);
    LCD_WR_REG(dev, 0x21);

    LCD_WR_REG(dev, 0x11);   // 退出睡眠
    LCD_DELAY_MS(dev, 120);
    LCD_WR_REG(dev, 0x29);   // 开启显示
    LCD_DELAY_MS(dev, 20);

    LCD_Fill(dev, 0, 0, LCD_W, LCD_H, BLACK);
    LCD_BL_ON(dev);
}

// --------------------------------------------------
// 基础写操作
// --------------------------------------------------
void LCD_WR_DATA8(LCD_Device *dev, uint8_t dat)
{
    LCD_CS_CLR(dev);
    LCD_DC_SET(dev);
    LCD_Writ_Bus(dev, dat);
    LCD_CS_SET(dev);
}

void LCD_WR_DATA(LCD_Device *dev, uint16_t dat)
{
    LCD_CS_CLR(dev);
    LCD_DC_SET(dev);
    LCD_Writ_Bus(dev, dat >> 8);
    LCD_Writ_Bus(dev, dat & 0xFF);
    LCD_CS_SET(dev);
}

void LCD_WR_REG(LCD_Device *dev, uint8_t dat)
{
    LCD_CS_CLR(dev);
    LCD_DC_CLR(dev);          // 写命令
    LCD_Writ_Bus(dev, dat);
    LCD_DC_SET(dev);          // 后续为数据
    LCD_CS_SET(dev);
}

void LCD_Address_Set(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    LCD_WR_REG(dev, 0x2A);    // 列地址设置
    LCD_WR_DATA(dev, x1);
    LCD_WR_DATA(dev, x2);
    LCD_WR_REG(dev, 0x2B);    // 行地址设置
    LCD_WR_DATA(dev, y1);
    LCD_WR_DATA(dev, y2);
    LCD_WR_REG(dev, 0x2C);    // 储存器写
}

// --------------------------------------------------
// 图形函数
// --------------------------------------------------
void LCD_Fill(LCD_Device *dev, uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color)
{
    uint32_t remain;
    uint32_t i;

    if (xsta >= xend || ysta >= yend) return;

    LCD_Address_Set(dev, xsta, ysta, xend - 1, yend - 1);

    // 预填充行缓冲
    for (i = 0; i < sizeof(lcd_line_buf); i += 2) {
        lcd_line_buf[i]     = color >> 8;
        lcd_line_buf[i + 1] = color & 0xFF;
    }

    remain = (uint32_t)(xend - xsta) * (yend - ysta);   // 剩余像素数

    LCD_CS_CLR(dev);
    LCD_DC_SET(dev);
    while (remain > 0) {
        uint32_t pixels = (remain > LCD_W) ? LCD_W : remain;
        LCD_Writ_Bytes(dev, lcd_line_buf, pixels * 2);
        remain -= pixels;
    }
    LCD_CS_SET(dev);
}

void LCD_DrawPoint(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= dev->max_x || y >= dev->max_y) return;
    LCD_Address_Set(dev, x, y, x, y);
    LCD_WR_DATA(dev, color);
}

void LCD_DrawLine(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    uint16_t t;
    int xerr = 0, yerr = 0, delta_x, delta_y, distance;
    int incx, incy, uRow, uCol;

    delta_x = x2 - x1;
    delta_y = y2 - y1;
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

    for (t = 0; t < distance + 1; t++) {
        LCD_DrawPoint(dev, uRow, uCol, color);
        xerr += delta_x;
        yerr += delta_y;
        if (xerr > distance) { xerr -= distance; uRow += incx; }
        if (yerr > distance) { yerr -= distance; uCol += incy; }
    }
}

void LCD_DrawRectangle(LCD_Device *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    LCD_DrawLine(dev, x1, y1, x2, y1, color);
    LCD_DrawLine(dev, x1, y1, x1, y2, color);
    LCD_DrawLine(dev, x1, y2, x2, y2, color);
    LCD_DrawLine(dev, x2, y1, x2, y2, color);
}

void Draw_Circle(LCD_Device *dev, uint16_t x0, uint16_t y0, uint8_t r, uint16_t color)
{
    int a, b;
    a = 0;
    b = r;
    while (a <= b) {
        LCD_DrawPoint(dev, x0 - b, y0 - a, color);
        LCD_DrawPoint(dev, x0 + b, y0 - a, color);
        LCD_DrawPoint(dev, x0 - a, y0 + b, color);
        LCD_DrawPoint(dev, x0 - a, y0 - b, color);
        LCD_DrawPoint(dev, x0 + b, y0 + a, color);
        LCD_DrawPoint(dev, x0 + a, y0 - b, color);
        LCD_DrawPoint(dev, x0 + a, y0 + b, color);
        LCD_DrawPoint(dev, x0 - b, y0 + a, color);
        a++;
        if ((a * a + b * b) > (r * r)) {
            b--;
        }
    }
}

// --------------------------------------------------
// 字符与数字显示
// --------------------------------------------------
void LCD_ShowChar(LCD_Device *dev, uint16_t x, uint16_t y, uint8_t num,
                  uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode)
{
    uint8_t temp, sizex, t, m = 0;
    uint16_t i, TypefaceNum;
    uint16_t x0 = x;

    sizex = sizey / 2;
    TypefaceNum = (sizex / 8 + ((sizex % 8) ? 1 : 0)) * sizey;
    num = num - ' ';

    LCD_Address_Set(dev, x, y, x + sizex - 1, y + sizey - 1);
    LCD_CS_CLR(dev);
    LCD_DC_SET(dev);

    for (i = 0; i < TypefaceNum; i++) {
        if (sizey == 12)      temp = ascii_1206[num][i];
        else if (sizey == 16) temp = ascii_1608[num][i];
        else if (sizey == 24) temp = ascii_2412[num][i];
        else if (sizey == 32) temp = ascii_3216[num][i];
        else { LCD_CS_SET(dev); return; }

        for (t = 0; t < 8; t++) {
            if (!mode) {   // 非叠加模式
                if (temp & (0x01 << t)) {
                    LCD_Writ_Bus(dev, fc >> 8);
                    LCD_Writ_Bus(dev, fc & 0xFF);
                } else {
                    LCD_Writ_Bus(dev, bc >> 8);
                    LCD_Writ_Bus(dev, bc & 0xFF);
                }
                m++;
                if (m % sizex == 0) {
                    m = 0;
                    break;
                }
            } else {       // 叠加模式
                if (temp & (0x01 << t)) {
                    LCD_DrawPoint(dev, x, y, fc);
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
    LCD_CS_SET(dev);
}

void LCD_ShowString(LCD_Device *dev, uint16_t x, uint16_t y, const char *p,
                    uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode)
{
    while (*p != '\0') {
        LCD_ShowChar(dev, x, y, (uint8_t)*p, fc, bc, sizey, mode);
        x += sizey / 2;
        p++;
    }
}

uint32_t mypow(uint8_t m, uint8_t n)
{
    uint32_t result = 1;
    while (n--) result *= m;
    return result;
}

void LCD_ShowIntNum(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t num, uint8_t len,
                    uint16_t fc, uint16_t bc, uint8_t sizey)
{
    uint8_t t, temp;
    uint8_t enshow = 0;
    uint8_t sizex = sizey / 2;

    for (t = 0; t < len; t++) {
        temp = (num / mypow(10, len - t - 1)) % 10;
        if (enshow == 0 && t < (len - 1)) {
            if (temp == 0) {
                LCD_ShowChar(dev, x + t * sizex, y, ' ', fc, bc, sizey, 0);
                continue;
            } else {
                enshow = 1;
            }
        }
        LCD_ShowChar(dev, x + t * sizex, y, temp + '0', fc, bc, sizey, 0);
    }
}

void LCD_ShowFloatNum1(LCD_Device *dev, uint16_t x, uint16_t y, float num, uint8_t len,
                       uint16_t fc, uint16_t bc, uint8_t sizey)
{
    int32_t int_part;
    uint16_t dec;
    uint8_t sizex = sizey / 2;

    if (num < 0) {
        LCD_ShowChar(dev, x, y, '-', fc, bc, sizey, 0);
        x += sizex;
        num = -num;
    }

    int_part = (int32_t)num;
    dec = (uint16_t)((num - int_part) * 10.0f + 0.5f);
    if (dec > 9) {
        int_part++;
        dec = 0;
    }

    LCD_ShowIntNum(dev, x, y, (uint16_t)int_part, len, fc, bc, sizey);
    x += len * sizex;
    LCD_ShowChar(dev, x, y, '.', fc, bc, sizey, 0);
    LCD_ShowChar(dev, x + sizex, y, (uint8_t)(dec + '0'), fc, bc, sizey, 0);
}

// --------------------------------------------------
// 图片显示
// --------------------------------------------------
void LCD_ShowPicture(LCD_Device *dev, uint16_t x, uint16_t y, uint16_t length, uint16_t width, const uint8_t pic[])
{
    uint32_t total_bytes = (uint32_t)length * width * 2;

    LCD_Address_Set(dev, x, y, x + length - 1, y + width - 1);

    LCD_CS_CLR(dev);
    LCD_DC_SET(dev);
    LCD_Writ_Bytes(dev, pic, total_bytes);
    LCD_CS_SET(dev);
}
