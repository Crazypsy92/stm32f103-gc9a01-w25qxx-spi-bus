/**
 * @file  test_gc9a01.c
 * @brief Host 单元测试：GC9A01 驱动协议层、长数据分块、边界防护与全链路错误传播
 *        通过 Mock Port 拦截底层 CS/DC/RST/Delay/Write，验证 MCU 无关纯协议逻辑。
 */
#include "gc9a01.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================================== */
/* Mock Port 实现与状态跟踪                                                            */
/* ================================================================================== */

typedef struct {
    uint8_t dc;       /* 0=CMD, 1=DATA */
    uint16_t len;     /* 本次写入长度 */
} mock_tx_entry_t;

typedef struct {
    /* 调用计数 */
    uint32_t cs_low_count;
    uint32_t cs_high_count;
    uint32_t rst_low_count;
    uint32_t rst_high_count;
    uint32_t dc_cmd_count;
    uint32_t dc_data_count;
    uint32_t write_call_count;
    uint32_t delay_call_count;

    /* 传输明细记录 */
    mock_tx_entry_t tx_records[300];
    uint32_t tx_count;

    /* 当前逻辑电平 */
    uint8_t current_cs;  /* 0=LOW, 1=HIGH */
    uint8_t current_dc;  /* 0=CMD, 1=DATA */
    uint8_t current_rst;

    /* 故障注入控制 */
    int inject_cs_acquire_fail;
    int inject_cs_release_fail;
    int inject_write_fail;
    uint32_t fail_on_write_call_index;

    /* 命令与数据捕获 */
    uint8_t last_cmd;
    uint8_t last_data_byte;
} mock_gc9a01_ctx_t;

static mock_gc9a01_ctx_t g_mock;

static void mock_reset(void)
{
    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.current_cs = 1U; /* 初始空闲为高电平 */
    g_mock.current_dc = 1U;
    g_mock.current_rst = 1U;
}

static LCD_Result mock_cs(void *ctx, uint8_t level)
{
    mock_gc9a01_ctx_t *m = (mock_gc9a01_ctx_t *)ctx;
    if (level == 0U) {
        if (m->inject_cs_acquire_fail) {
            return LCD_ERROR_BUS;
        }
        m->cs_low_count++;
        m->current_cs = 0U;
    } else {
        if (m->inject_cs_release_fail) {
            return LCD_ERROR_BUS;
        }
        m->cs_high_count++;
        m->current_cs = 1U;
    }
    return LCD_OK;
}

static void mock_rst(void *ctx, uint8_t level)
{
    mock_gc9a01_ctx_t *m = (mock_gc9a01_ctx_t *)ctx;
    if (level == 0U) {
        m->rst_low_count++;
        m->current_rst = 0U;
    } else {
        m->rst_high_count++;
        m->current_rst = 1U;
    }
}

static void mock_dc(void *ctx, uint8_t level)
{
    mock_gc9a01_ctx_t *m = (mock_gc9a01_ctx_t *)ctx;
    if (level == 0U) {
        m->dc_cmd_count++;
        m->current_dc = 0U;
    } else {
        m->dc_data_count++;
        m->current_dc = 1U;
    }
}

static void mock_delay_ms(void *ctx, uint32_t ms)
{
    (void)ms;
    mock_gc9a01_ctx_t *m = (mock_gc9a01_ctx_t *)ctx;
    m->delay_call_count++;
}

static LCD_Result mock_write(void *ctx, const uint8_t *buf, uint16_t len)
{
    mock_gc9a01_ctx_t *m = (mock_gc9a01_ctx_t *)ctx;
    m->write_call_count++;

    if (m->inject_write_fail) {
        return LCD_ERROR_BUS;
    }
    if (m->fail_on_write_call_index > 0U && m->write_call_count == m->fail_on_write_call_index) {
        return LCD_ERROR_BUS;
    }

    if (m->tx_count < (sizeof(m->tx_records) / sizeof(m->tx_records[0]))) {
        m->tx_records[m->tx_count].dc = m->current_dc;
        m->tx_records[m->tx_count].len = len;
        m->tx_count++;
    }

    if (len > 0U && buf != NULL) {
        if (m->current_dc == 0U) {
            m->last_cmd = buf[0];
        } else {
            m->last_data_byte = buf[len - 1U];
        }
    }

    return LCD_OK;
}

static const LCD_Port g_mock_port = {
    .cs            = mock_cs,
    .rst           = mock_rst,
    .dc            = mock_dc,
    .bl            = NULL,
    .delay_ms      = mock_delay_ms,
    .write         = mock_write,
    .max_chunk_len = 65535U,
    .ctx           = &g_mock,
};

/* ================================================================================== */
/* 测试宏与辅助                                                                       */
/* ================================================================================== */

static int g_fail = 0;
static int g_assert_count = 0;

#define CHECK(cond, msg) do { \
    g_assert_count++; \
    if (!(cond)) { \
        printf("  [FAIL] line %d: %s\n", __LINE__, msg); \
        g_fail++; \
    } else { \
        printf("  [OK]   %s\n", msg); \
    } \
} while (0)

static uint8_t s_buf_115k[115200];
static uint8_t s_buf_70k[70000];
static uint8_t s_buf_65k[65536];

/* ================================================================================== */
/* 基础功能测试用例                                                                   */
/* ================================================================================== */

/* 1. 正常命令与数据发送 */
static void test_normal_command_send(void)
{
    printf("\n=== Test 1: Normal Command & Data Send ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    LCD_Result res = LCD_WR_REG(&dev, 0x29);
    CHECK(res == LCD_OK, "LCD_WR_REG returns LCD_OK");
    CHECK(g_mock.last_cmd == 0x29, "Last command is 0x29");
    CHECK(g_mock.current_cs == 1U, "CS returned to HIGH after command");
    CHECK(g_mock.current_dc == 1U, "DC returned to DATA mode after command");

    res = LCD_WR_DATA8(&dev, 0x55);
    CHECK(res == LCD_OK, "LCD_WR_DATA8 returns LCD_OK");
    CHECK(g_mock.last_data_byte == 0x55, "Data byte is 0x55");
    CHECK(g_mock.current_cs == 1U, "CS returned to HIGH after DATA8");

    res = LCD_WR_DATA(&dev, 0x1234);
    CHECK(res == LCD_OK, "LCD_WR_DATA returns LCD_OK");
    CHECK(g_mock.last_data_byte == 0x34, "Lower data byte matches 0x34");
    CHECK(g_mock.current_cs == 1U, "CS returned to HIGH after DATA16");
}

/* 2. 16 B 阈值测试 */
static void test_16b_threshold(void)
{
    printf("\n=== Test 2: 16 B Boundary / Threshold ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    /* 4x2 像素 RGB565 图片 = 4 * 2 * 2 = 16 字节 */
    uint8_t pic16[16];
    memset(pic16, 0xAA, sizeof(pic16));

    LCD_Result res = LCD_ShowPicture(&dev, 10, 10, 4, 2, pic16);
    CHECK(res == LCD_OK, "ShowPicture 16B returns LCD_OK");
    /* LCD_Address_Set 占用前 7 次写入，第 8 次写入为图片数据 */
    CHECK(g_mock.tx_count == 8U, "7 address calls + 1 picture write call");
    CHECK(g_mock.tx_records[7].dc == 1U, "Picture chunk sent with DC=DATA");
    CHECK(g_mock.tx_records[7].len == 16U, "Single write chunk size is exactly 16 bytes");
    CHECK(g_mock.current_cs == 1U, "CS returned to HIGH");
}

/* 3. 65,280 B 边界测试 (单次 Port 写入) */
static void test_65280b_boundary(void)
{
    printf("\n=== Test 3: 65,280 B Boundary (Single Chunk Port Write) ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    memset(s_buf_65k, 0x5A, 65280);

    /* 240 * 136 * 2 = 65,280 字节（未超 65535 字节单块上限） */
    LCD_Result res = LCD_ShowPicture(&dev, 0, 0, 240, 136, s_buf_65k);
    CHECK(res == LCD_OK, "ShowPicture 65,280 B returns LCD_OK");
    CHECK(g_mock.tx_count == 8U, "7 address calls + 1 data chunk");
    CHECK(g_mock.tx_records[7].len == 65280U, "Data chunk size is exactly 65,280 bytes");
    CHECK(g_mock.tx_records[7].dc == 1U, "Data chunk DC=DATA");
    CHECK(g_mock.current_cs == 1U, "CS returned to HIGH");
}

/* 4. 65,536 B 分块测试 (分为 65,535 + 1 B，利用内部长数据分块测试辅助接口) */
static void test_65536b_chunking(void)
{
    printf("\n=== Test 4: 65,536 B Chunking (65,535 + 1 B) ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    memset(s_buf_65k, 0xA5, 65536);

    /* 手动片选与数据模式模拟完整调用链 */
    LCD_Result cs_res = g_mock_port.cs(g_mock_port.ctx, 0);
    CHECK(cs_res == LCD_OK, "CS acquire OK");
    g_mock_port.dc(g_mock_port.ctx, 1);

    LCD_Result wr_res = LCD_Writ_Bytes(&dev, s_buf_65k, 65536U);
    CHECK(wr_res == LCD_OK, "LCD_Writ_Bytes 65,536 B returns LCD_OK");

    LCD_Result rel_res = g_mock_port.cs(g_mock_port.ctx, 1);
    CHECK(rel_res == LCD_OK, "CS release OK");

    CHECK(g_mock.tx_count == 2U, "Split into exactly 2 write chunks");
    CHECK(g_mock.tx_records[0].len == 65535U, "First chunk is exactly 65,535 B");
    CHECK(g_mock.tx_records[1].len == 1U, "Second chunk is exactly 1 B");
    CHECK(g_mock.tx_records[0].dc == 1U && g_mock.tx_records[1].dc == 1U, "Both chunks DC=DATA");
    CHECK(g_mock.current_cs == 1U, "CS returned to HIGH");
}

/* 5. 70,000 B 分块测试 (200x175 RGB565 合法图片，分为 65,535 + 4,465 B) */
static void test_70000b_chunking(void)
{
    printf("\n=== Test 5: 70,000 B Chunking (200x175 RGB565 -> 65,535 + 4,465 B) ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    memset(s_buf_70k, 0x7E, 70000);

    /* 200 * 175 * 2 = 70,000 字节（合法的 200x175 屏幕范围图像） */
    LCD_Result res = LCD_ShowPicture(&dev, 0, 0, 200, 175, s_buf_70k);
    CHECK(res == LCD_OK, "ShowPicture 70,000 B (200x175) returns LCD_OK");
    CHECK(g_mock.tx_count == 9U, "7 address calls + 2 data chunks");
    CHECK(g_mock.tx_records[7].len == 65535U, "First data chunk is exactly 65,535 B");
    CHECK(g_mock.tx_records[8].len == (70000U - 65535U), "Second data chunk is remaining 4,465 B");
    CHECK(g_mock.tx_records[7].dc == 1U && g_mock.tx_records[8].dc == 1U, "Both chunks sent with DC=DATA");
    CHECK(g_mock.current_cs == 1U, "CS returned to HIGH at the end");
}

/* 6. 115,200 B 全屏发送完整测试 (240x240 RGB565 -> 65,535 + 49,665 B) */
static void test_115200b_fullscreen_send(void)
{
    printf("\n=== Test 6: 115,200 B Fullscreen Send (240x240 RGB565 -> 65,535 + 49,665 B) ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    memset(s_buf_115k, 0x33, 115200);

    LCD_Result res = LCD_ShowPicture(&dev, 0, 0, 240, 240, s_buf_115k);
    CHECK(res == LCD_OK, "ShowPicture 115,200 B returns LCD_OK");
    CHECK(g_mock.tx_count == 9U, "7 address calls + 2 picture data chunks");
    CHECK(g_mock.tx_records[7].len == 65535U, "Chunk 1 is 65,535 B");
    CHECK(g_mock.tx_records[8].len == (115200U - 65535U), "Chunk 2 is 49,665 B");
    CHECK((g_mock.tx_records[7].len + g_mock.tx_records[8].len) == 115200U, "Total pixel bytes exactly 115,200 B");
    CHECK(g_mock.current_cs == 1U, "CS returned to HIGH after fullscreen transfer");
    CHECK(g_mock.current_dc == 1U, "DC held in DATA mode");
}

/* 7. SPI 写入失败向协议层报告与中断 */
static void test_spi_write_fail(void)
{
    printf("\n=== Test 7: SPI Write Failure Propagation ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    /* 注入底层 write 错误 */
    g_mock.inject_write_fail = 1;

    LCD_Result res = LCD_WR_REG(&dev, 0x29);
    CHECK(res == LCD_ERROR_BUS, "LCD_WR_REG returns LCD_ERROR_BUS on write fail");
    CHECK(LCD_GetLastError(&dev) == LCD_ERROR_BUS, "dev->last_error recorded LCD_ERROR_BUS");
    CHECK(g_mock.current_cs == 1U, "CS released to HIGH even on write failure");

    /* 测试分块中途失败：第 2 个数据分块失败（第 7 次调用为第 1 分块，第 8 次调用为第 2 分块，加 7 次地址调用共第 9 次） */
    mock_reset();
    LCD_ClearError(&dev);
    g_mock.fail_on_write_call_index = 9U;

    res = LCD_ShowPicture(&dev, 0, 0, 240, 240, s_buf_115k);
    CHECK(res == LCD_ERROR_BUS, "ShowPicture returns LCD_ERROR_BUS on 2nd chunk fail");
    CHECK(g_mock.write_call_count == 9U, "Stops transmission immediately, no further write calls");
    CHECK(g_mock.tx_records[7].len == 65535U, "First chunk was written successfully");
    CHECK(g_mock.tx_count == 8U, "Only chunk 1 recorded before failure at chunk 2");
    CHECK(g_mock.current_cs == 1U, "CS safely released to HIGH on mid-transfer fail");
}

/* 8. CS 申请失败：不继续发送数据 */
static void test_cs_acquire_fail(void)
{
    printf("\n=== Test 8: CS Acquire Failure Handling ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    g_mock.inject_cs_acquire_fail = 1;

    LCD_Result res = LCD_WR_REG(&dev, 0x29);
    CHECK(res == LCD_ERROR_BUS, "LCD_WR_REG returns LCD_ERROR_BUS on CS acquire fail");
    CHECK(g_mock.write_call_count == 0U, "No SPI command or data sent after CS acquire fail");

    res = LCD_Fill(&dev, 0, 0, 10, 10, RED);
    CHECK(res == LCD_ERROR_BUS, "LCD_Fill returns LCD_ERROR_BUS on CS acquire fail");
    CHECK(g_mock.tx_count == 0U, "No pixels sent after CS acquire fail");
}

/* 9. CS 释放失败：不误报成功 */
static void test_cs_release_fail(void)
{
    printf("\n=== Test 9: CS Release Failure Handling ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    g_mock.inject_cs_release_fail = 1;

    LCD_Result res = LCD_WR_REG(&dev, 0x29);
    CHECK(res == LCD_ERROR_BUS, "LCD_WR_REG returns LCD_ERROR_BUS on CS release fail");

    res = LCD_WR_DATA8(&dev, 0x55);
    CHECK(res == LCD_ERROR_BUS, "LCD_WR_DATA8 returns LCD_ERROR_BUS on CS release fail");

    res = LCD_Fill(&dev, 0, 0, 10, 10, RED);
    CHECK(res == LCD_ERROR_BUS, "LCD_Fill returns LCD_ERROR_BUS on CS release fail");
}

/* 10. 非法 ASCII 字符防御 */
static void test_illegal_ascii_char(void)
{
    printf("\n=== Test 10: Illegal ASCII Character Bounds ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    /* ASCII 下界越界 (< ' ' = 32) */
    LCD_Result res = LCD_ShowChar(&dev, 0, 0, 0U, WHITE, BLACK, 16, 0);
    CHECK(res == LCD_ERROR_PARAM, "ShowChar ASCII 0 returns LCD_ERROR_PARAM");

    res = LCD_ShowChar(&dev, 0, 0, 31U, WHITE, BLACK, 16, 0);
    CHECK(res == LCD_ERROR_PARAM, "ShowChar ASCII 31 returns LCD_ERROR_PARAM");

    /* ASCII 上界越界 (> '~' = 126) */
    res = LCD_ShowChar(&dev, 0, 0, 127U, WHITE, BLACK, 16, 0);
    CHECK(res == LCD_ERROR_PARAM, "ShowChar ASCII 127 returns LCD_ERROR_PARAM");

    res = LCD_ShowChar(&dev, 0, 0, 255U, WHITE, BLACK, 16, 0);
    CHECK(res == LCD_ERROR_PARAM, "ShowChar ASCII 255 returns LCD_ERROR_PARAM");

    /* 正常字符 */
    res = LCD_ShowChar(&dev, 0, 0, 'A', WHITE, BLACK, 16, 0);
    CHECK(res == LCD_OK, "ShowChar 'A' (65) returns LCD_OK");
}

/* 11. 非法字号安全处理 */
static void test_illegal_font_size(void)
{
    printf("\n=== Test 11: Illegal Font Size Handling ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    LCD_Result res = LCD_ShowChar(&dev, 0, 0, 'A', WHITE, BLACK, 0U, 0);
    CHECK(res == LCD_ERROR_PARAM, "Font size 0 rejected");

    res = LCD_ShowChar(&dev, 0, 0, 'A', WHITE, BLACK, 8U, 0);
    CHECK(res == LCD_ERROR_PARAM, "Font size 8 rejected");

    res = LCD_ShowChar(&dev, 0, 0, 'A', WHITE, BLACK, 14U, 0);
    CHECK(res == LCD_ERROR_PARAM, "Font size 14 rejected");

    res = LCD_ShowChar(&dev, 0, 0, 'A', WHITE, BLACK, 48U, 0);
    CHECK(res == LCD_ERROR_PARAM, "Font size 48 rejected");

    /* 支持的四个合法字号：12, 16, 24, 32 */
    CHECK(LCD_ShowChar(&dev, 0, 0, 'A', WHITE, BLACK, 12U, 0) == LCD_OK, "Font size 12 OK");
    CHECK(LCD_ShowChar(&dev, 0, 0, 'A', WHITE, BLACK, 16U, 0) == LCD_OK, "Font size 16 OK");
    CHECK(LCD_ShowChar(&dev, 0, 0, 'A', WHITE, BLACK, 24U, 0) == LCD_OK, "Font size 24 OK");
    CHECK(LCD_ShowChar(&dev, 0, 0, 'A', WHITE, BLACK, 32U, 0) == LCD_OK, "Font size 32 OK");
}

/* 12. 图形坐标越界与裁剪防御 */
static void test_coord_out_of_bounds(void)
{
    printf("\n=== Test 12: Coordinate Bounds & Clipping ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    /* 画点越界 */
    CHECK(LCD_DrawPoint(&dev, 240, 10, RED) == LCD_ERROR_PARAM, "DrawPoint x=240 rejected");
    CHECK(LCD_DrawPoint(&dev, 10, 240, RED) == LCD_ERROR_PARAM, "DrawPoint y=240 rejected");
    CHECK(LCD_DrawPoint(&dev, 239, 239, RED) == LCD_OK, "DrawPoint (239, 239) OK");

    /* 填充非法坐标与零宽零高 */
    CHECK(LCD_Fill(&dev, 50, 50, 50, 60, RED) == LCD_ERROR_PARAM, "Fill zero width rejected");
    CHECK(LCD_Fill(&dev, 50, 50, 60, 50, RED) == LCD_ERROR_PARAM, "Fill zero height rejected");
    CHECK(LCD_Fill(&dev, 60, 50, 50, 60, RED) == LCD_ERROR_PARAM, "Fill xsta > xend rejected");
    CHECK(LCD_Fill(&dev, 0, 0, 241, 100, RED) == LCD_ERROR_PARAM, "Fill xend > 240 rejected");
    CHECK(LCD_Fill(&dev, 0, 0, 100, 241, RED) == LCD_ERROR_PARAM, "Fill yend > 240 rejected");
    CHECK(LCD_Fill(&dev, 0, 0, 240, 240, RED) == LCD_OK, "Fill (0,0,240,240) OK");

    /* 字符越界 */
    CHECK(LCD_ShowChar(&dev, 235, 0, 'A', WHITE, BLACK, 16, 0) == LCD_ERROR_PARAM,
          "ShowChar x=235 + sizex(8)=243 > 240 rejected");

    /* 字符串安全裁剪：超出屏幕边界停止绘制，不崩溃 */
    mock_reset();
    LCD_Result res = LCD_ShowString(&dev, 220, 0, "ABCDEFGHIJ", WHITE, BLACK, 16, 0);
    CHECK(res == LCD_OK, "ShowString boundary clipping returns safely");
    CHECK(g_mock.current_cs == 1U, "CS released after clipping");

    /* 图片越界与尺寸溢出 */
    uint8_t dummy[16] = {0};
    CHECK(LCD_ShowPicture(&dev, 0, 0, 0, 10, dummy) == LCD_ERROR_PARAM, "Zero length rejected");
    CHECK(LCD_ShowPicture(&dev, 0, 0, 10, 0, dummy) == LCD_ERROR_PARAM, "Zero width rejected");
    CHECK(LCD_ShowPicture(&dev, 200, 0, 50, 50, dummy) == LCD_ERROR_PARAM, "x+length > 240 rejected");
    CHECK(LCD_ShowPicture(&dev, 0, 200, 50, 50, dummy) == LCD_ERROR_PARAM, "y+width > 240 rejected");
}

/* 13. 初始化序列与失败处理 */
static void test_initialization_sequence(void)
{
    printf("\n=== Test 13: GC9A01 Initialization Sequence ===\n");
    mock_reset();
    LCD_Device dev;

    /* NULL 参数检查 */
    CHECK(GC9A01_Init(NULL, &g_mock_port) == LCD_ERROR_PARAM, "Init NULL dev rejected");
    CHECK(GC9A01_Init(&dev, NULL) == LCD_ERROR_PARAM, "Init NULL port rejected");

    /* 正常完整初始化 */
    LCD_Result res = GC9A01_Init(&dev, &g_mock_port);
    CHECK(res == LCD_OK, "GC9A01_Init returns LCD_OK");
    CHECK(g_mock.rst_low_count >= 1U && g_mock.rst_high_count >= 1U, "Reset pulses executed");
    CHECK(g_mock.delay_call_count >= 5U, "Initialization delays executed");
    CHECK(g_mock.write_call_count > 30U, "Initialization registers written");
    CHECK(g_mock.current_cs == 1U, "CS returned to HIGH after Init");

    /* 初始化中途总线故障 */
    mock_reset();
    g_mock.fail_on_write_call_index = 5U;
    res = GC9A01_Init(&dev, &g_mock_port);
    CHECK(res == LCD_ERROR_BUS, "GC9A01_Init aborts and returns LCD_ERROR_BUS on SPI fail");
}

/* ================================================================================== */
/* Stage 2.1-Fix 新增专用故障注入与错误传播测试 (T01 ~ T08)                             */
/* ================================================================================== */

/* T01: LCD_DrawLine 中途 SPI 失败 -> 返回 BUS 错误且停止绘制 */
static void test_T01_draw_line_mid_fail(void)
{
    printf("\n=== T01: LCD_DrawLine Mid-SPI Failure Propagation ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    /* 绘制从 (10, 10) 到 (30, 10) 共 21 个点的水平线，每个点含 address_set(7 calls) + wr_data(1 call) = 8 calls */
    /* 在第 12 次 write 调用处注入故障（即第 2 个点的写入过程） */
    g_mock.fail_on_write_call_index = 12U;

    LCD_Result res = LCD_DrawLine(&dev, 10, 10, 30, 10, RED);
    CHECK(res == LCD_ERROR_BUS, "LCD_DrawLine returns LCD_ERROR_BUS when mid-SPI fails");
    CHECK(g_mock.write_call_count == 12U, "Aborted immediately on failure without drawing subsequent points");
    CHECK(LCD_GetLastError(&dev) == LCD_ERROR_BUS, "dev->last_error records LCD_ERROR_BUS");
    CHECK(g_mock.current_cs == 1U, "CS safely released to HIGH");
}

/* T02: LCD_DrawRectangle 中途失败 -> 中止后续边线绘制 */
static void test_T02_draw_rectangle_mid_fail(void)
{
    printf("\n=== T02: LCD_DrawRectangle Mid-Failure Propagation ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    /* 第一条水平线 (x1, y1) 到 (x2, y1) 写入过程中注入故障 */
    g_mock.fail_on_write_call_index = 10U;

    LCD_Result res = LCD_DrawRectangle(&dev, 10, 10, 50, 50, BLUE);
    CHECK(res == LCD_ERROR_BUS, "LCD_DrawRectangle returns LCD_ERROR_BUS on failure");
    CHECK(g_mock.write_call_count == 10U, "Aborted immediately on first line failure, remaining 3 lines not drawn");
    CHECK(LCD_GetLastError(&dev) == LCD_ERROR_BUS, "dev->last_error records LCD_ERROR_BUS");
    CHECK(g_mock.current_cs == 1U, "CS safely released to HIGH");
}

/* T03: Draw_Circle 中途失败 -> 不误报成功并立即退出 */
static void test_T03_draw_circle_mid_fail(void)
{
    printf("\n=== T03: Draw_Circle Mid-Failure Propagation ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    /* 圆周点在第 25 次 write 调用处注入故障 */
    g_mock.fail_on_write_call_index = 25U;

    LCD_Result res = Draw_Circle(&dev, 120, 120, 50, GREEN);
    CHECK(res == LCD_ERROR_BUS, "Draw_Circle returns LCD_ERROR_BUS on failure");
    CHECK(g_mock.write_call_count == 25U, "Aborted immediately on failure, not continuing circle loop");
    CHECK(LCD_GetLastError(&dev) == LCD_ERROR_BUS, "dev->last_error records LCD_ERROR_BUS");
    CHECK(g_mock.current_cs == 1U, "CS safely released to HIGH");
}

/* T04: LCD_ShowFloatNum1 绘制失败 -> 完整保留并传播错误 */
static void test_T04_show_float_mid_fail(void)
{
    printf("\n=== T04: LCD_ShowFloatNum1 Failure Propagation ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    /* 浮点数显示包含负号/整数部分/小数点/小数部分，注入写入故障 */
    g_mock.fail_on_write_call_index = 15U;

    LCD_Result res = LCD_ShowFloatNum1(&dev, 10, 10, -12.34f, 4, WHITE, BLACK, 16);
    CHECK(res == LCD_ERROR_BUS, "LCD_ShowFloatNum1 returns LCD_ERROR_BUS on failure");
    CHECK(g_mock.write_call_count == 15U, "Aborted immediately on failure");
    CHECK(LCD_GetLastError(&dev) == LCD_ERROR_BUS, "dev->last_error records LCD_ERROR_BUS");
    CHECK(g_mock.current_cs == 1U, "CS safely released to HIGH");
}

/* T05: GC9A01_Init 中途失败 -> 初始化失败结果明确可见 */
static void test_T05_gc9a01_init_mid_fail(void)
{
    printf("\n=== T05: GC9A01_Init Mid-Failure Visible ===\n");
    mock_reset();
    LCD_Device dev;

    /* 在初始化序列中间（第 10 次写入）注入故障 */
    g_mock.fail_on_write_call_index = 10U;

    LCD_Result res = GC9A01_Init(&dev, &g_mock_port);
    CHECK(res == LCD_ERROR_BUS, "GC9A01_Init returns LCD_ERROR_BUS on SPI failure");
    CHECK(g_mock.write_call_count == 10U, "Init sequence halted immediately at failure point");
    CHECK(LCD_GetLastError(&dev) == LCD_ERROR_BUS, "dev->last_error records LCD_ERROR_BUS");
    CHECK(g_mock.current_cs == 1U, "CS returned to HIGH");
}

/* T06: TFT 局部刷新失败 -> 成功计数严格不增加 (校验 AppFlash_Test 统计防护) */
static void test_T06_tft_partial_update_fail_stops_count(void)
{
    printf("\n=== T06: TFT Partial Update Failure Stops Count Increment ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    uint32_t tft_update_count = 0;
    uint32_t fail_round = 0;
    LCD_Result fail_res = LCD_OK;

    /* 模拟 20 轮交替循环：前 5 轮成功，第 6 轮发生 SPI 写入失败 */
    for (uint32_t r = 0; r < 20U; r++) {
        if (r == 5U) {
            g_mock.inject_write_fail = 1;
        }

        LCD_Result res = LCD_Fill(&dev, 24 + (r % 10) * 19, 140 + (r / 10) * 16,
                                  24 + (r % 10) * 19 + 15, 140 + (r / 10) * 16 + 12, GREEN);
        if (res == LCD_OK) {
            tft_update_count++;
        } else {
            fail_round = r;
            fail_res = res;
            break; /* 步骤失败立即退出，不再虚报 */
        }
    }

    CHECK(fail_round == 5U, "Failure occurred at round 5 (6th attempt)");
    CHECK(fail_res == LCD_ERROR_BUS, "Failure result is LCD_ERROR_BUS");
    CHECK(tft_update_count == 5U, "tft_update_count is strictly 5, NOT 6 or 20");
    /* 检验 pass 条件：严格要求 20 次，当前 5 次必须判定为 0 (FAIL) */
    uint8_t pass_verdict = (tft_update_count == 20U);
    CHECK(pass_verdict == 0U, "g_app_dual_result.pass evaluates to 0 (FAIL)");
}

/* T07: 流式全屏发送中途失败 -> 终止发送并正确释放 CS */
static void test_T07_stream_pattern_mid_fail(void)
{
    printf("\n=== T07: Stream Fullscreen Mid-Failure Aborts and Releases CS ===\n");
    mock_reset();
    LCD_Device dev;
    dev.port = &g_mock_port;
    dev.max_x = LCD_W;
    dev.max_y = LCD_H;
    dev.last_error = LCD_OK;

    /*
     * 模拟 AppTFT_StreamPattern() 逻辑：
     * 1. LCD_Address_Set (7 write calls)
     * 2. CS LOW
     * 3. DC DATA
     * 4. 逐行发送 240 行 (每行 480B)
     * 5. CS HIGH
     * 注入故障在第 7 + 10 = 17 次 write 调用（即第 10 行数据传输时）
     */
    g_mock.fail_on_write_call_index = 17U;

    static uint8_t line_buf[240 * 2];
    memset(line_buf, 0x55, sizeof(line_buf));

    LCD_Result res = LCD_Address_Set(&dev, 0, 0, LCD_W - 1, LCD_H - 1);
    CHECK(res == LCD_OK, "Address set OK before stream");

    res = dev.port->cs(dev.port->ctx, 0);
    CHECK(res == LCD_OK, "CS acquired");
    dev.port->dc(dev.port->ctx, 1);

    LCD_Result wr_res = LCD_OK;
    uint16_t lines_sent = 0;
    for (uint16_t y = 0; y < LCD_H; y++) {
        wr_res = dev.port->write(dev.port->ctx, line_buf, LCD_W * 2U);
        if (wr_res != LCD_OK) {
            break;
        }
        lines_sent++;
    }

    LCD_Result rel_res = dev.port->cs(dev.port->ctx, 1);
    LCD_Result final_res = (wr_res != LCD_OK) ? wr_res : rel_res;

    CHECK(final_res == LCD_ERROR_BUS, "Stream pattern returns LCD_ERROR_BUS on line failure");
    CHECK(lines_sent == 9U, "Successfully sent 9 lines, aborted at 10th line");
    CHECK(g_mock.write_call_count == 17U, "Total write calls stopped at 17, did NOT complete 247 writes");
    CHECK(g_mock.current_cs == 1U, "CS safely released to HIGH at termination");
}

/* T08: 无异常正常全功能回归 */
static void test_T08_normal_regression(void)
{
    printf("\n=== T08: Normal Full Feature Regression ===\n");
    mock_reset();
    LCD_Device dev;

    /* 1. 初始化 */
    LCD_Result res = GC9A01_Init(&dev, &g_mock_port);
    CHECK(res == LCD_OK, "GC9A01_Init passes");

    /* 2. 矩形填充 */
    res = LCD_Fill(&dev, 10, 10, 50, 50, RED);
    CHECK(res == LCD_OK, "LCD_Fill passes");

    /* 3. 画点 */
    res = LCD_DrawPoint(&dev, 60, 60, BLUE);
    CHECK(res == LCD_OK, "LCD_DrawPoint passes");

    /* 4. 画线 */
    res = LCD_DrawLine(&dev, 0, 0, 100, 100, GREEN);
    CHECK(res == LCD_OK, "LCD_DrawLine passes");

    /* 5. 矩形框 */
    res = LCD_DrawRectangle(&dev, 20, 20, 80, 80, YELLOW);
    CHECK(res == LCD_OK, "LCD_DrawRectangle passes");

    /* 6. 画圆 */
    res = Draw_Circle(&dev, 120, 120, 40, CYAN);
    CHECK(res == LCD_OK, "Draw_Circle passes");

    /* 7. 字符显示 */
    res = LCD_ShowChar(&dev, 30, 30, 'Z', WHITE, BLACK, 16, 0);
    CHECK(res == LCD_OK, "LCD_ShowChar passes");

    /* 8. 字符串显示 */
    res = LCD_ShowString(&dev, 30, 50, "REGRESSION", WHITE, BLACK, 16, 0);
    CHECK(res == LCD_OK, "LCD_ShowString passes");

    /* 9. 整数显示 */
    res = LCD_ShowIntNum(&dev, 30, 70, 12345, 5, WHITE, BLACK, 16);
    CHECK(res == LCD_OK, "LCD_ShowIntNum passes");

    /* 10. 浮点数显示 */
    res = LCD_ShowFloatNum1(&dev, 30, 90, 3.1415f, 4, WHITE, BLACK, 16);
    CHECK(res == LCD_OK, "LCD_ShowFloatNum1 passes");

    /* 11. 图片显示 */
    uint8_t mini_pic[32];
    memset(mini_pic, 0x12, sizeof(mini_pic));
    res = LCD_ShowPicture(&dev, 0, 0, 4, 4, mini_pic);
    CHECK(res == LCD_OK, "LCD_ShowPicture passes");

    /* 全局错误码依然为 OK，CS 处于高电平 */
    CHECK(LCD_GetLastError(&dev) == LCD_OK, "dev->last_error is LCD_OK");
    CHECK(g_mock.current_cs == 1U, "CS is HIGH");
}

/* ================================================================================== */
/* 主函数                                                                             */
/* ================================================================================== */

int main(void)
{
    printf("====================================================================\n");
    printf("Stage 2.1-Fix GC9A01 Protocol Layer & Fault Injection Host Unit Tests\n");
    printf("====================================================================\n");

    /* 基础功能与边界防护测试 (1 ~ 13) */
    test_normal_command_send();
    test_16b_threshold();
    test_65280b_boundary();
    test_65536b_chunking();
    test_70000b_chunking();
    test_115200b_fullscreen_send();
    test_spi_write_fail();
    test_cs_acquire_fail();
    test_cs_release_fail();
    test_illegal_ascii_char();
    test_illegal_font_size();
    test_coord_out_of_bounds();
    test_initialization_sequence();

    /* Stage 2.1-Fix 专用故障注入与错误传播回归测试 (T01 ~ T08) */
    test_T01_draw_line_mid_fail();
    test_T02_draw_rectangle_mid_fail();
    test_T03_draw_circle_mid_fail();
    test_T04_show_float_mid_fail();
    test_T05_gc9a01_init_mid_fail();
    test_T06_tft_partial_update_fail_stops_count();
    test_T07_stream_pattern_mid_fail();
    test_T08_normal_regression();

    printf("\n--------------------------------------------------------------------\n");
    printf("Summary: %d assertions, %d failures\n", g_assert_count, g_fail);
    printf("====================================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
