/**
 * @file  test_flash.c
 * @brief Host 离线单元测试：W25Q128 Flash 数据可靠性与安全保护整改 (Stage 1.2 & 1.2-B)
 * @details 覆盖：
 *          - F01: 地址边界与参数合法性检查（首末边界、跨页跨扇区、24位回绕、未对齐擦除、capacity=0）
 *          - F02: 写使能 (WEL) 状态校验与重试机制（WEL 失败严禁破坏性操作）
 *          - F03: SPI 单字节交换失败与 CS 失败错误向上传播
 *          - F04: WaitBusy 状态机错误与超时检测（禁止通信错误误判为完成）
 *          - F06: 读写跨页、跨扇区数据一致性与端到端闭环验证
 *          - Stage 1.2-B Task 2: 片选释放 (W25_CS_HIGH) 失败全链路故障注入测试 (T01 ~ T08)
 *          - Stage 1.2-B Task 3: 非破坏性重复读取数据校验与数据不一致检测注入
 */

#include "w25qxx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ====================================================================
 * 桩实现：模拟延时与时钟
 * ==================================================================== */

static uint32_t s_mock_millis = 0;

void Delay_us(uint32_t nus) {
    (void)nus;
}

void Delay_ms(uint32_t nms) {
    s_mock_millis += nms;
}

uint32_t GetTick_ms(void) {
    return s_mock_millis;
}

/* ====================================================================
 * Mock Flash 硬件模拟器
 * ==================================================================== */

#define MOCK_FLASH_SIZE (16U * 1024U * 1024U)

static uint8_t *s_mock_flash_mem = NULL;

typedef enum {
    SPI_STATE_IDLE = 0,
    SPI_STATE_CMD,
    SPI_STATE_ADDR_0,
    SPI_STATE_ADDR_1,
    SPI_STATE_ADDR_2,
    SPI_STATE_DATA_READ,
    SPI_STATE_DATA_WRITE,
    SPI_STATE_READ_STATUS,
    SPI_STATE_READ_ID_DUMMY0,
    SPI_STATE_READ_ID_DUMMY1,
    SPI_STATE_READ_ID_DUMMY2,
    SPI_STATE_READ_ID_0,
    SPI_STATE_READ_ID_1,
} spi_state_t;

typedef struct {
    uint8_t status_reg;        /* bit0: BUSY, bit1: WEL */
    spi_state_t state;
    uint8_t current_cmd;
    uint32_t current_addr;
    uint16_t id;
    int cs_asserted;           /* 模拟硬件 CS 引脚状态：1=拉低选中，0=拉高释放 */

    /* 故障注入开关 */
    int inject_cs_low_fail;
    int inject_cs_high_fail;       /* CS 释放失败注入（模拟硬件引脚或总线释放被拒） */
    int inject_swap_fail;
    int inject_wel_fail;           /* WREN 后 WEL 保持为 0 */
    int inject_busy_timeout;       /* BUSY 保持为 1 */
    int inject_status_reg_swap_err;/* 读状态寄存器时 swap 报错 */

    /* 统计 */
    uint32_t erase_sector_count;
    uint32_t page_program_count;
} mock_flash_ctx_t;

static mock_flash_ctx_t s_mock_ctx;

static void mock_flash_reset(void)
{
    if (s_mock_flash_mem == NULL) {
        s_mock_flash_mem = (uint8_t *)malloc(MOCK_FLASH_SIZE);
        assert(s_mock_flash_mem != NULL);
    }
    memset(s_mock_flash_mem, 0xFF, MOCK_FLASH_SIZE);
    memset(&s_mock_ctx, 0, sizeof(s_mock_ctx));
    s_mock_ctx.id = W25Q128;
    s_mock_ctx.status_reg = 0x00;
    s_mock_millis = 0;
}

/* ====================================================================
 * W25QXX_Bus Mock 回调实现
 * ==================================================================== */

static int mock_bus_cs_low(void *ctx)
{
    mock_flash_ctx_t *m = (mock_flash_ctx_t *)ctx;
    if (m->inject_cs_low_fail) {
        return -1;
    }
    m->cs_asserted = 1;
    m->state = SPI_STATE_CMD;
    return 0;
}

static int mock_bus_cs_high(void *ctx)
{
    mock_flash_ctx_t *m = (mock_flash_ctx_t *)ctx;
    if (m->inject_cs_high_fail) {
        /* 模拟物理 CS 释放失败，引脚维持低电平（未释放），总线被占用 */
        return -1;
    }
    m->cs_asserted = 0;
    m->state = SPI_STATE_IDLE;
    return 0;
}

static int mock_bus_swap(void *ctx, uint8_t tx, uint8_t *rx)
{
    mock_flash_ctx_t *m = (mock_flash_ctx_t *)ctx;
    uint8_t r = 0xFF;

    if (!m->cs_asserted) {
        return -1;
    }
    if (m->inject_swap_fail) {
        return -1;
    }

    switch (m->state) {
        case SPI_STATE_CMD:
            m->current_cmd = tx;
            if (tx == W25X_ManufactDeviceID) {
                m->state = SPI_STATE_READ_ID_DUMMY0;
            } else if (tx == W25X_ReadStatusReg) {
                if (m->inject_status_reg_swap_err) {
                    return -1;
                }
                m->state = SPI_STATE_READ_STATUS;
            } else if (tx == W25X_WriteEnable) {
                if (!m->inject_wel_fail) {
                    m->status_reg |= 0x02U; /* WEL = 1 */
                }
                m->state = SPI_STATE_IDLE;
            } else if (tx == W25X_WriteDisable) {
                m->status_reg &= ~0x02U;
                m->state = SPI_STATE_IDLE;
            } else if (tx == W25X_ReadData || tx == W25X_PageProgram || tx == W25X_SectorErase) {
                m->state = SPI_STATE_ADDR_0;
            } else {
                m->state = SPI_STATE_IDLE;
            }
            break;

        case SPI_STATE_READ_ID_DUMMY0:
            m->state = SPI_STATE_READ_ID_DUMMY1;
            break;
        case SPI_STATE_READ_ID_DUMMY1:
            m->state = SPI_STATE_READ_ID_DUMMY2;
            break;
        case SPI_STATE_READ_ID_DUMMY2:
            m->state = SPI_STATE_READ_ID_0;
            break;
        case SPI_STATE_READ_ID_0:
            r = (uint8_t)(m->id >> 8);
            m->state = SPI_STATE_READ_ID_1;
            break;
        case SPI_STATE_READ_ID_1:
            r = (uint8_t)(m->id & 0xFF);
            m->state = SPI_STATE_IDLE;
            break;

        case SPI_STATE_READ_STATUS:
            if (m->inject_status_reg_swap_err) {
                return -1;
            }
            r = m->status_reg;
            if (m->inject_busy_timeout) {
                r |= 0x01U; /* BUSY 永不清除 */
                s_mock_millis += 10;
            }
            break;

        case SPI_STATE_ADDR_0:
            m->current_addr = ((uint32_t)tx) << 16;
            m->state = SPI_STATE_ADDR_1;
            break;
        case SPI_STATE_ADDR_1:
            m->current_addr |= ((uint32_t)tx) << 8;
            m->state = SPI_STATE_ADDR_2;
            break;
        case SPI_STATE_ADDR_2:
            m->current_addr |= tx;
            if (m->current_cmd == W25X_ReadData) {
                m->state = SPI_STATE_DATA_READ;
            } else if (m->current_cmd == W25X_PageProgram) {
                m->state = SPI_STATE_DATA_WRITE;
                m->page_program_count++;
            } else if (m->current_cmd == W25X_SectorErase) {
                m->erase_sector_count++;
                if (m->current_addr < MOCK_FLASH_SIZE) {
                    uint32_t s_base = (m->current_addr / W25QXX_SECTOR_SIZE) * W25QXX_SECTOR_SIZE;
                    memset(s_mock_flash_mem + s_base, 0xFF, W25QXX_SECTOR_SIZE);
                }
                m->status_reg &= ~0x02U; /* 擦除后自动清 WEL */
                m->state = SPI_STATE_IDLE;
            }
            break;

        case SPI_STATE_DATA_READ:
            if (m->current_addr < MOCK_FLASH_SIZE) {
                r = s_mock_flash_mem[m->current_addr++];
            }
            break;

        case SPI_STATE_DATA_WRITE:
            if (m->current_addr < MOCK_FLASH_SIZE) {
                s_mock_flash_mem[m->current_addr++] &= tx; /* Flash 只能写 0 */
            }
            break;

        default:
            break;
    }

    if (rx) {
        *rx = r;
    }
    return 0;
}

static uint32_t mock_bus_millis(void *ctx)
{
    (void)ctx;
    return s_mock_millis;
}

static int mock_bus_read_bulk(void *ctx, uint8_t *rx, uint32_t len)
{
    mock_flash_ctx_t *m = (mock_flash_ctx_t *)ctx;
    if (m->inject_swap_fail) {
        return -1;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (mock_bus_swap(ctx, 0xFF, &rx[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int mock_bus_write_bulk(void *ctx, const uint8_t *tx, uint32_t len)
{
    mock_flash_ctx_t *m = (mock_flash_ctx_t *)ctx;
    if (m->inject_swap_fail) {
        return -1;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (mock_bus_swap(ctx, tx[i], NULL) != 0) {
            return -1;
        }
    }
    return 0;
}

static const W25QXX_Bus s_mock_bus = {
    .cs_low     = mock_bus_cs_low,
    .cs_high    = mock_bus_cs_high,
    .swap       = mock_bus_swap,
    .millis     = mock_bus_millis,
    .ctx        = &s_mock_ctx,
    .read_bulk  = mock_bus_read_bulk,
    .write_bulk = mock_bus_write_bulk,
};

/* ====================================================================
 * 测试断言与套件
 * ==================================================================== */

static int s_tests_run = 0;
static int s_tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    s_tests_run++; \
    if (cond) { \
        s_tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
        assert(0); \
    } \
} while(0)

/* ---- 1. 地址边界与参数安全检查 (F01) ---- */
static void test_address_boundary_protection(void)
{
    printf("\n=== Test 1: Address Boundary & Param Validation (F01) ===\n");
    mock_flash_reset();
    W25QXX_Device dev;
    uint8_t buf[512];

    /* 未初始化设备或未识别容量 (capacity == 0) */
    memset(&dev, 0, sizeof(dev));
    dev.bus = &s_mock_bus;
    dev.capacity = 0;
    TEST_ASSERT(W25QXX_Read(&dev, buf, 0, 16) == W25QXX_ERROR_PARAM, "Read rejected when capacity=0");
    TEST_ASSERT(W25QXX_Write(&dev, buf, 0, 16) == W25QXX_ERROR_PARAM, "Write rejected when capacity=0");
    TEST_ASSERT(W25QXX_EraseSector(&dev, 0) == W25QXX_ERROR_PARAM, "EraseSector rejected when capacity=0");

    /* 正常初始化 */
    uint8_t init_ok = W25QXX_Init(&dev, &s_mock_bus);
    TEST_ASSERT(init_ok == 1, "Init succeeded for W25Q128 (16MB)");
    TEST_ASSERT(dev.capacity == 16U * 1024U * 1024U, "Capacity accurately identified as 16MB");

    /* 空指针与长度 0 检查 */
    TEST_ASSERT(W25QXX_Read(&dev, NULL, 0, 16) == W25QXX_ERROR_PARAM, "Read rejected on NULL buffer");
    TEST_ASSERT(W25QXX_Read(&dev, buf, 0, 0) == W25QXX_ERROR_PARAM, "Read rejected on len=0");
    TEST_ASSERT(W25QXX_Write(&dev, NULL, 0, 16) == W25QXX_ERROR_PARAM, "Write rejected on NULL buffer");
    TEST_ASSERT(W25QXX_Write(&dev, buf, 0, 0) == W25QXX_ERROR_PARAM, "Write rejected on len=0");

    /* 起始地址等于容量边界 (越界) */
    TEST_ASSERT(W25QXX_Read(&dev, buf, dev.capacity, 1) == W25QXX_ERROR_PARAM, "Read rejected at addr == capacity");
    TEST_ASSERT(W25QXX_Write(&dev, buf, dev.capacity, 1) == W25QXX_ERROR_PARAM, "Write rejected at addr == capacity");

    /* 起始地址超过容量 */
    TEST_ASSERT(W25QXX_Read(&dev, buf, dev.capacity + 100, 1) == W25QXX_ERROR_PARAM, "Read rejected at addr > capacity");
    TEST_ASSERT(W25QXX_Write(&dev, buf, dev.capacity + 100, 1) == W25QXX_ERROR_PARAM, "Write rejected at addr > capacity");

    /* 最后一个有效字节访问 (合法) */
    TEST_ASSERT(W25QXX_Read(&dev, buf, dev.capacity - 1, 1) == W25QXX_OK, "Read allowed at last valid byte (capacity-1)");

    /* 跨越末尾边界 (addr + len > capacity) */
    TEST_ASSERT(W25QXX_Read(&dev, buf, dev.capacity - 10, 11) == W25QXX_ERROR_PARAM, "Read rejected when addr+len > capacity");
    TEST_ASSERT(W25QXX_Write(&dev, buf, dev.capacity - 10, 11) == W25QXX_ERROR_PARAM, "Write rejected when addr+len > capacity");

    /* 24 位地址回绕溢出检查 */
    TEST_ASSERT(W25QXX_Read(&dev, buf, 0x00FFFFFFU, 256) == W25QXX_ERROR_PARAM, "Read rejected on 24-bit boundary overflow");
    TEST_ASSERT(W25QXX_Write(&dev, buf, 0x00FFFFFFU, 256) == W25QXX_ERROR_PARAM, "Write rejected on 24-bit boundary overflow");

    /* 32 位整数相加回绕检查 */
    TEST_ASSERT(W25QXX_Read(&dev, buf, 0xFFFFFFFFU, 10) == W25QXX_ERROR_PARAM, "Read rejected on 32-bit uint wrap-around");
    TEST_ASSERT(W25QXX_Write(&dev, buf, 0xFFFFFFFFU, 10) == W25QXX_ERROR_PARAM, "Write rejected on 32-bit uint wrap-around");

    /* 扇区擦除未对齐 (4KB 对齐强制检查) */
    TEST_ASSERT(W25QXX_EraseSector(&dev, 1) == W25QXX_ERROR_PARAM, "EraseSector rejected on unaligned address 1");
    TEST_ASSERT(W25QXX_EraseSector(&dev, 4095) == W25QXX_ERROR_PARAM, "EraseSector rejected on unaligned address 4095");
    TEST_ASSERT(W25QXX_EraseSector(&dev, 4097) == W25QXX_ERROR_PARAM, "EraseSector rejected on unaligned address 4097");
    TEST_ASSERT(W25QXX_EraseSector(&dev, 4096) == W25QXX_OK, "EraseSector accepted on aligned address 4096");
    TEST_ASSERT(W25QXX_EraseSector(&dev, dev.capacity - W25QXX_SECTOR_SIZE) == W25QXX_OK, "EraseSector accepted on last sector");
    TEST_ASSERT(W25QXX_EraseSector(&dev, dev.capacity) == W25QXX_ERROR_PARAM, "EraseSector rejected on addr == capacity");
}

/* ---- 2. 写使能 (WEL) 失败拦截与重试 (F02) ---- */
static void test_write_enable_failure_interception(void)
{
    printf("\n=== Test 2: Write Enable (WEL) Failure Interception (F02) ===\n");
    mock_flash_reset();
    W25QXX_Device dev;
    W25QXX_Init(&dev, &s_mock_bus);
    uint8_t data[256];
    memset(data, 0x5A, sizeof(data));

    /* 注入 WEL 硬件锁存故障 */
    s_mock_ctx.inject_wel_fail = 1;
    s_mock_ctx.erase_sector_count = 0;
    s_mock_ctx.page_program_count = 0;

    W25QXX_Result wel_res = W25QXX_WriteEnable(&dev);
    TEST_ASSERT(wel_res == W25QXX_ERROR_WEL, "W25QXX_WriteEnable returns ERROR_WEL when hardware latch fails");

    W25QXX_Result wr_res = W25QXX_Write_NoCheck(&dev, data, 0x1000, 256);
    TEST_ASSERT(wr_res == W25QXX_ERROR_WEL, "Write_NoCheck aborted immediately on WEL failure");
    TEST_ASSERT(s_mock_ctx.page_program_count == 0, "No Page Program command sent to flash on WEL failure");

    W25QXX_Result er_res = W25QXX_EraseSector(&dev, 0x1000);
    TEST_ASSERT(er_res == W25QXX_ERROR_WEL, "EraseSector aborted immediately on WEL failure");
    TEST_ASSERT(s_mock_ctx.erase_sector_count == 0, "No Sector Erase command sent to flash on WEL failure");

    W25QXX_Result ec_res = W25QXX_EraseChip(&dev);
    TEST_ASSERT(ec_res == W25QXX_ERROR_WEL, "EraseChip aborted immediately on WEL failure");

    s_mock_ctx.inject_wel_fail = 0;
    TEST_ASSERT(W25QXX_WriteEnable(&dev) == W25QXX_OK, "WriteEnable succeeds after hardware recovers");
}

/* ---- 3. 底层 SPI 通信故障向上传播 (F03) ---- */
static void test_spi_error_propagation(void)
{
    printf("\n=== Test 3: SPI Error Propagation (F03) ===\n");
    mock_flash_reset();
    W25QXX_Device dev;
    W25QXX_Init(&dev, &s_mock_bus);
    uint8_t buf[256];

    /* CS 申请失败 */
    s_mock_ctx.inject_cs_low_fail = 1;
    TEST_ASSERT(W25QXX_Read(&dev, buf, 0x1000, 16) == W25QXX_ERROR_BUS, "Read returns ERROR_BUS on cs_low failure");
    TEST_ASSERT(W25QXX_Write_NoCheck(&dev, buf, 0x1000, 16) == W25QXX_ERROR_BUS, "Write returns ERROR_BUS on cs_low failure");
    TEST_ASSERT(W25QXX_EraseSector(&dev, 0x1000) == W25QXX_ERROR_BUS, "EraseSector returns ERROR_BUS on cs_low failure");
    s_mock_ctx.inject_cs_low_fail = 0;

    /* 单字节 swap 传输失败 */
    s_mock_ctx.inject_swap_fail = 1;
    TEST_ASSERT(W25QXX_Read(&dev, buf, 0x1000, 16) == W25QXX_ERROR_BUS, "Read returns ERROR_BUS on swap failure");
    TEST_ASSERT(W25QXX_Write_NoCheck(&dev, buf, 0x1000, 16) == W25QXX_ERROR_BUS, "Write returns ERROR_BUS on swap failure");
    TEST_ASSERT(W25QXX_EraseSector(&dev, 0x1000) == W25QXX_ERROR_BUS, "EraseSector returns ERROR_BUS on swap failure");
    TEST_ASSERT(s_mock_ctx.cs_asserted == 0, "CS safely released after swap failure");
    s_mock_ctx.inject_swap_fail = 0;

    /* 状态寄存器读取失败 */
    s_mock_ctx.inject_status_reg_swap_err = 1;
    uint8_t st = 0;
    TEST_ASSERT(W25QXX_ReadStatusReg(&dev, &st) == W25QXX_ERROR_BUS, "ReadStatusReg returns ERROR_BUS on swap error");
    s_mock_ctx.inject_status_reg_swap_err = 0;
}

/* ---- 4. WaitBusy 异常处理与超时 (F04) ---- */
static void test_wait_busy_and_timeout(void)
{
    printf("\n=== Test 4: WaitBusy Error & Timeout Detection (F04) ===\n");
    mock_flash_reset();
    W25QXX_Device dev;
    W25QXX_Init(&dev, &s_mock_bus);

    /* 场景 A：状态寄存器读失败 */
    s_mock_ctx.inject_status_reg_swap_err = 1;
    W25QXX_Result res_a = W25QXX_EraseSector(&dev, 0x2000);
    TEST_ASSERT(res_a == W25QXX_ERROR_BUS, "WaitBusy returns ERROR_BUS immediately when status read fails");
    s_mock_ctx.inject_status_reg_swap_err = 0;

    /* 场景 B：BUSY 硬件超时 */
    s_mock_ctx.inject_busy_timeout = 1;
    W25QXX_Result res_b = W25QXX_EraseSector(&dev, 0x2000);
    TEST_ASSERT(res_b == W25QXX_ERROR_TIMEOUT, "WaitBusy accurately returns ERROR_TIMEOUT on long busy");
    s_mock_ctx.inject_busy_timeout = 0;
}

/* ---- 5. 跨页、跨扇区端到端数据完整性测试 (F06) ---- */
static void test_page_and_sector_cross_write(void)
{
    printf("\n=== Test 5: Cross Page & Cross Sector Data Integrity (F06) ===\n");
    mock_flash_reset();
    W25QXX_Device dev;
    W25QXX_Init(&dev, &s_mock_bus);

    uint32_t cross_page_addr = 0x000100C8U;
    uint16_t cross_page_len = 300U;
    uint8_t tx_buf[300], rx_buf[300];

    for (uint16_t i = 0; i < cross_page_len; i++) {
        tx_buf[i] = (uint8_t)(i ^ 0xA5U);
    }

    TEST_ASSERT(W25QXX_EraseSector(&dev, 0x00010000U) == W25QXX_OK, "EraseSector ok before cross-page write");
    TEST_ASSERT(W25QXX_Write_NoCheck(&dev, tx_buf, cross_page_addr, cross_page_len) == W25QXX_OK, "Write_NoCheck cross-page succeeds");

    memset(rx_buf, 0, sizeof(rx_buf));
    TEST_ASSERT(W25QXX_Read(&dev, rx_buf, cross_page_addr, cross_page_len) == W25QXX_OK, "Read cross-page data succeeds");
    TEST_ASSERT(memcmp(tx_buf, rx_buf, cross_page_len) == 0, "Cross-page read-back data byte-for-byte identical");

    uint32_t cross_sector_addr = 0x00020FF0U;
    uint16_t cross_sector_len = 64U;
    uint8_t sec_tx[64], sec_rx[64];
    for (uint16_t i = 0; i < cross_sector_len; i++) {
        sec_tx[i] = (uint8_t)(i + 0x33U);
    }

    uint32_t erase_cnt_before = s_mock_ctx.erase_sector_count;
    TEST_ASSERT(W25QXX_Write(&dev, sec_tx, cross_sector_addr, cross_sector_len) == W25QXX_OK, "W25QXX_Write across sectors succeeds");
    TEST_ASSERT((s_mock_ctx.erase_sector_count - erase_cnt_before) == 2, "Exactly 2 sectors erased for cross-sector write");

    memset(sec_rx, 0, sizeof(sec_rx));
    TEST_ASSERT(W25QXX_Read(&dev, sec_rx, cross_sector_addr, cross_sector_len) == W25QXX_OK, "Read across sectors succeeds");
    TEST_ASSERT(memcmp(sec_tx, sec_rx, cross_sector_len) == 0, "Cross-sector data byte-for-byte identical");
}

/* ====================================================================
 * Stage 1.2-B Task 2: 片选释放 (CS_HIGH) 失败全链路故障注入测试 (T01 ~ T08)
 * ==================================================================== */
static void test_cs_high_failure_injection(void)
{
    printf("\n=== Test 6 (Stage 1.2-B): CS_HIGH Failure Injection (T01 ~ T08) ===\n");
    mock_flash_reset();
    W25QXX_Device dev;
    W25QXX_Init(&dev, &s_mock_bus);
    uint8_t buf[256];
    memset(buf, 0xA5, sizeof(buf));

    /* T01: ReadStatusReg 结束时 CS 释放失败 -> 必须返回 ERROR_BUS，且不得提交状态结果 */
    {
        uint8_t status = 0x55U;
        s_mock_ctx.inject_cs_high_fail = 1;
        W25QXX_Result r = W25QXX_ReadStatusReg(&dev, &status);
        TEST_ASSERT(r == W25QXX_ERROR_BUS, "T01: ReadStatusReg returns ERROR_BUS when CS_HIGH fails");
        TEST_ASSERT(status == 0x55U, "T01: ReadStatusReg does not commit result to output buffer on CS_HIGH failure");
        s_mock_ctx.inject_cs_high_fail = 0;
        s_mock_ctx.cs_asserted = 0;
    }

    /* T02: Read 数据结束时 CS 释放失败 -> 禁止返回 OK (返回 ERROR_BUS) */
    {
        s_mock_ctx.inject_cs_high_fail = 1;
        W25QXX_Result r = W25QXX_Read(&dev, buf, 0x1000, 32);
        TEST_ASSERT(r == W25QXX_ERROR_BUS, "T02: Read returns ERROR_BUS and NEVER returns OK when CS_HIGH fails");
        s_mock_ctx.inject_cs_high_fail = 0;
        s_mock_ctx.cs_asserted = 0;
    }

    /* T03: WriteEnable 结束时 CS 释放失败 -> 返回 ERROR_BUS，禁止继续下发编程 */
    {
        s_mock_ctx.page_program_count = 0;
        s_mock_ctx.inject_cs_high_fail = 1;
        W25QXX_Result r = W25QXX_WriteEnable(&dev);
        TEST_ASSERT(r == W25QXX_ERROR_BUS, "T03: WriteEnable returns ERROR_BUS when CS_HIGH fails");
        s_mock_ctx.inject_cs_high_fail = 0;
        s_mock_ctx.cs_asserted = 0;
    }

    /* T04: Page Program 结束时 CS 释放失败 -> 不得报告编程成功 */
    {
        s_mock_ctx.inject_cs_high_fail = 1;
        W25QXX_Result r = W25QXX_Write_NoCheck(&dev, buf, 0x1000, 64);
        TEST_ASSERT(r == W25QXX_ERROR_BUS, "T04: Write_NoCheck returns ERROR_BUS and reports NO success on CS_HIGH fail");
        s_mock_ctx.inject_cs_high_fail = 0;
        s_mock_ctx.cs_asserted = 0;
    }

    /* T05: Sector Erase 结束时 CS 释放失败 -> 不得报告擦除成功 */
    {
        s_mock_ctx.inject_cs_high_fail = 1;
        W25QXX_Result r = W25QXX_EraseSector(&dev, 0x1000);
        TEST_ASSERT(r == W25QXX_ERROR_BUS, "T05: EraseSector returns ERROR_BUS and reports NO success on CS_HIGH fail");
        s_mock_ctx.inject_cs_high_fail = 0;
        s_mock_ctx.cs_asserted = 0;
    }

    /* T06: 检查故障发生时 CS 状态未被无声掩盖 */
    {
        s_mock_ctx.inject_cs_high_fail = 1;
        (void)W25QXX_Read(&dev, buf, 0x2000, 16);
        TEST_ASSERT(s_mock_ctx.cs_asserted == 1, "T06: CS hardware line remains asserted when CS_HIGH fails (no silent leak)");
        s_mock_ctx.inject_cs_high_fail = 0;
        /* 安全复位片选 */
        s_mock_ctx.cs_asserted = 0;
    }

    /* T07: 故障恢复后重新访问，状态有效时才允许通信 */
    {
        TEST_ASSERT(s_mock_ctx.cs_asserted == 0, "T07: CS line clean before retry");
        W25QXX_Result r = W25QXX_Read(&dev, buf, 0x1000, 16);
        TEST_ASSERT(r == W25QXX_OK, "T07: Read succeeds immediately after hardware recovers");
    }

    /* T08: 正常通信回归检查（读、写、擦除全功能正常） */
    {
        TEST_ASSERT(W25QXX_EraseSector(&dev, 0x3000) == W25QXX_OK, "T08: Sector erase regression OK");
        TEST_ASSERT(W25QXX_Write_NoCheck(&dev, buf, 0x3000, 16) == W25QXX_OK, "T08: Page write regression OK");
        uint8_t verify[16];
        TEST_ASSERT(W25QXX_Read(&dev, verify, 0x3000, 16) == W25QXX_OK, "T08: Read verify regression OK");
        TEST_ASSERT(memcmp(buf, verify, 16) == 0, "T08: Verified data identical");
    }
}

/* ====================================================================
 * Stage 1.2-B Task 3: 重复读取数据一致性与数据篡改异常检测
 * ==================================================================== */
static void test_repeated_read_data_consistency(void)
{
    printf("\n=== Test 7 (Stage 1.2-B): Repeated Read Data Consistency & Mismatch Detection ===\n");
    mock_flash_reset();
    W25QXX_Device dev;
    W25QXX_Init(&dev, &s_mock_bus);

    /* 准备一段预置数据 */
    for (uint32_t i = 0; i < 480; i++) {
        s_mock_flash_mem[0x00100000U + i] = (uint8_t)(i ^ 0x5AU);
    }

    uint8_t ref_buf[480];
    uint8_t rx_buf[480];
    TEST_ASSERT(W25QXX_Read(&dev, ref_buf, 0x00100000U, 480) == W25QXX_OK, "Read reference buffer OK");

    /* 1. 连续 1000 次 1B 重复读取，验证数据一致性与独立统计 */
    uint32_t read_ok = 0;
    uint32_t data_match_ok = 0;
    uint32_t data_mismatch = 0;
    uint8_t ref_byte = ref_buf[0];

    for (int k = 0; k < 1000; k++) {
        uint8_t b = 0;
        if (W25QXX_Read(&dev, &b, 0x00100000U, 1) == W25QXX_OK) {
            read_ok++;
            if (b == ref_byte) {
                data_match_ok++;
            } else {
                data_mismatch++;
            }
        }
    }
    TEST_ASSERT(read_ok == 1000, "1000/1000 repeated reads communication successful");
    TEST_ASSERT(data_match_ok == 1000, "1000/1000 repeated reads data 100% consistent");
    TEST_ASSERT(data_mismatch == 0, "0 data mismatches in consistent data test");

    /* 2. 注入数据不一致故障：模拟存储单元数据突变或传输错误 */
    s_mock_flash_mem[0x00100000U] = (uint8_t)(ref_byte ^ 0xFFU); /* 篡改首字节 */
    uint8_t corrupted_byte = 0;
    uint32_t first_mismatch_offset = 0xFFFFFFFFU;

    W25QXX_Result cr = W25QXX_Read(&dev, &corrupted_byte, 0x00100000U, 1);
    TEST_ASSERT(cr == W25QXX_OK, "Read communication succeeded even with mismatch data");
    if (corrupted_byte != ref_byte) {
        data_mismatch++;
        first_mismatch_offset = 0;
    }
    TEST_ASSERT(data_mismatch == 1, "Data mismatch accurately detected and counted");
    TEST_ASSERT(first_mismatch_offset == 0, "First mismatch offset accurately located at byte 0");

    /* 3. 全长度矩阵（1~480B）重复对比测试 */
    const uint16_t len_list[] = {1, 4, 8, 15, 16, 17, 32, 64, 256, 480};
    uint8_t len_count = sizeof(len_list) / sizeof(len_list[0]);
    uint8_t matrix_ok = 0;

    /* 恢复正确数据 */
    s_mock_flash_mem[0x00100000U] = ref_byte;

    for (uint8_t i = 0; i < len_count; i++) {
        uint16_t L = len_list[i];
        memset(rx_buf, 0, L);
        if (W25QXX_Read(&dev, rx_buf, 0x00100000U, L) == W25QXX_OK &&
            memcmp(rx_buf, ref_buf, L) == 0) {
            matrix_ok++;
        }
    }
    TEST_ASSERT(matrix_ok == len_count, "All 10 lengths in matrix match byte-for-byte with reference data");
}

int main(void)
{
    printf("====================================================\n");
    printf("   Stage 1.2 & 1.2-B: W25Q128 Flash Host Tests       \n");
    printf("====================================================\n");

    test_address_boundary_protection();
    test_write_enable_failure_interception();
    test_spi_error_propagation();
    test_wait_busy_and_timeout();
    test_page_and_sector_cross_write();
    test_cs_high_failure_injection();
    test_repeated_read_data_consistency();

    printf("\n====================================================\n");
    printf(" Summary: %d/%d assertions passed (100%% PASS)\n", s_tests_passed, s_tests_run);
    printf("====================================================\n");

    if (s_mock_flash_mem) {
        free(s_mock_flash_mem);
        s_mock_flash_mem = NULL;
    }
    return 0;
}
