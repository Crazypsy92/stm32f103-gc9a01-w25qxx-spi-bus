/**
 * @file  test_spi_multi_instance.c
 * @brief Host 单元测试：SPI 多实例管理与设备配置注入隔离性验证
 *        验证 Bus A / Bus B 独立状态机、设备配置注入、故障隔离与总线仲裁。
 */
#include "spi_bus.h"
#include "bsp_spi.h"
#include "gc9a01_port.h"
#include "w25qxx_port.h"
#include "stm32f1_host_stub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================================== */
/* 测试框架宏                                                                         */
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

/* ================================================================================== */
/* 独立 Mock 外设状态与操作实现                                                        */
/* ================================================================================== */

typedef struct {
    uint32_t       clock_hz;
    spi_bus_mode_t mode;
    uint8_t        use_dma;
    uint32_t       transfer_count;
    uint32_t       total_bytes;
    int            inject_timeout;
    uint32_t       recover_count;
    int32_t        last_error;
    uint8_t        last_tx_byte;
    uint8_t        last_rx_byte;
} mock_hw_t;

static int mock_configure(spi_bus_t *bus, const spi_bus_config_t *cfg)
{
    mock_hw_t *hw = (mock_hw_t *)bus->ctx;
    if (hw == NULL || cfg == NULL) {
        return -1;
    }
    hw->clock_hz = cfg->clock_hz;
    hw->mode     = cfg->mode;
    hw->use_dma  = cfg->use_dma;
    return 0;
}

static int mock_transfer(spi_bus_t *bus, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    mock_hw_t *hw = (mock_hw_t *)bus->ctx;
    if (hw == NULL || tx == NULL || len == 0U) {
        return -1;
    }
    if (hw->inject_timeout) {
        hw->last_error = -2; /* SPI_LL_ERROR_TIMEOUT */
        return -2;
    }
    hw->transfer_count++;
    hw->total_bytes += len;
    hw->last_tx_byte = tx[len - 1U];

    if (rx != NULL) {
        for (uint32_t i = 0; i < len; i++) {
            rx[i] = (uint8_t)(tx[i] ^ 0x5A);
        }
        hw->last_rx_byte = rx[len - 1U];
    }
    return 0;
}

static int mock_in_isr(spi_bus_t *bus)
{
    (void)bus;
    return 0; /* 测试环境默认线程模式 */
}

static const spi_bus_ops_t s_mock_ops = {
    .configure = mock_configure,
    .transfer  = mock_transfer,
    .in_isr    = mock_in_isr,
};

static void mock_hw_init(mock_hw_t *hw)
{
    memset(hw, 0, sizeof(*hw));
}

static void mock_bus_init(spi_bus_t *bus, mock_hw_t *hw)
{
    memset(bus, 0, sizeof(*bus));
    bus->ops = &s_mock_ops;
    bus->ctx = hw;
    bus->owner = NULL;
    bus->depth = 0U;
    bus->rejects = 0U;
}

static int mock_recover(spi_bus_t *bus)
{
    mock_hw_t *hw = (mock_hw_t *)bus->ctx;
    if (hw == NULL) {
        return -1;
    }
    hw->recover_count++;
    hw->inject_timeout = 0;
    hw->last_error = 0;
    return 0;
}

/* ================================================================================== */
/* 模拟设备上下文（不依赖具体 MCU）                                                   */
/* ================================================================================== */

typedef struct {
    const char       *name;
    spi_bus_t        *bus;
    spi_bus_config_t  cfg;
    uint8_t           cs_level;
} mock_device_t;

static void mock_device_init(mock_device_t *dev, const char *name, spi_bus_t *bus,
                             uint32_t hz, spi_bus_mode_t mode, uint8_t dma)
{
    dev->name = name;
    dev->bus = bus;
    dev->cfg.clock_hz = hz;
    dev->cfg.mode = mode;
    dev->cfg.use_dma = dma;
    dev->cs_level = 1U; /* 初始空闲高电平 */
}

static int mock_device_cs_low(mock_device_t *dev)
{
    if (dev == NULL || dev->bus == NULL) return -1;
    if (spi_bus_acquire(dev->bus, dev, &dev->cfg) != 0) {
        return -1;
    }
    dev->cs_level = 0U;
    return 0;
}

static int mock_device_cs_high(mock_device_t *dev)
{
    if (dev == NULL || dev->bus == NULL) return -1;
    int rel = spi_bus_release(dev->bus, dev);
    if (rel > 0) {
        dev->cs_level = 1U;
        return 0;
    }
    return (rel == 0) ? 0 : -1;
}

static int mock_device_write(mock_device_t *dev, const uint8_t *tx, uint32_t len)
{
    if (dev == NULL || dev->bus == NULL) return -1;
    return spi_bus_transfer(dev->bus, dev, tx, NULL, len);
}

static int mock_device_transfer(mock_device_t *dev, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    if (dev == NULL || dev->bus == NULL) return -1;
    return spi_bus_transfer(dev->bus, dev, tx, rx, len);
}

/* ================================================================================== */
/* 测试用例 M01 ~ M08                                                                 */
/* ================================================================================== */

/* M01: 创建 Bus A 和 Bus B，初始化互不影响 */
static void test_M01_create_independent_buses(void)
{
    printf("\n=== M01: Create Independent Bus A & Bus B ===\n");
    mock_hw_t hw_a, hw_b;
    spi_bus_t bus_a, bus_b;

    mock_hw_init(&hw_a);
    mock_hw_init(&hw_b);
    mock_bus_init(&bus_a, &hw_a);
    mock_bus_init(&bus_b, &hw_b);

    CHECK(bus_a.ctx == &hw_a, "Bus A points to HW A");
    CHECK(bus_b.ctx == &hw_b, "Bus B points to HW B");
    CHECK(bus_a.ctx != bus_b.ctx, "Bus A and Bus B have distinct contexts");

    CHECK(spi_bus_owner(&bus_a) == NULL, "Bus A initially unowned");
    CHECK(spi_bus_owner(&bus_b) == NULL, "Bus B initially unowned");
    CHECK(spi_bus_depth(&bus_a) == 0U, "Bus A initial depth == 0");
    CHECK(spi_bus_depth(&bus_b) == 0U, "Bus B initial depth == 0");
}

/* M02: 两条总线配置不同频率，参数独立 */
static void test_M02_independent_configs(void)
{
    printf("\n=== M02: Independent Bus Configurations ===\n");
    mock_hw_t hw_a, hw_b;
    spi_bus_t bus_a, bus_b;
    mock_device_t dev_a, dev_b;

    mock_hw_init(&hw_a);
    mock_hw_init(&hw_b);
    mock_bus_init(&bus_a, &hw_a);
    mock_bus_init(&bus_b, &hw_b);

    /* Dev A 在 Bus A 配置 18MHz / Mode 0 / DMA 1 */
    mock_device_init(&dev_a, "LCD_18M", &bus_a, 18000000U, SPI_BUS_MODE_0, 1);
    /* Dev B 在 Bus B 配置 9MHz / Mode 3 / DMA 0 */
    mock_device_init(&dev_b, "FLASH_9M", &bus_b, 9000000U, SPI_BUS_MODE_3, 0);

    CHECK(mock_device_cs_low(&dev_a) == 0, "Acquire Bus A with 18MHz/Mode0");
    CHECK(hw_a.clock_hz == 18000000U, "HW A clock is 18MHz");
    CHECK(hw_a.mode == SPI_BUS_MODE_0, "HW A mode is Mode 0");
    CHECK(hw_a.use_dma == 1U, "HW A use_dma is 1");

    CHECK(mock_device_cs_low(&dev_b) == 0, "Acquire Bus B with 9MHz/Mode3");
    CHECK(hw_b.clock_hz == 9000000U, "HW B clock is 9MHz");
    CHECK(hw_b.mode == SPI_BUS_MODE_3, "HW B mode is Mode 3");
    CHECK(hw_b.use_dma == 0U, "HW B use_dma is 0");

    /* 验证 A 的配置未被 B 改变 */
    CHECK(hw_a.clock_hz == 18000000U, "HW A clock remained 18MHz after Bus B configured");
    CHECK(hw_a.mode == SPI_BUS_MODE_0, "HW A mode remained Mode 0");

    CHECK(mock_device_cs_high(&dev_a) == 0, "Release Bus A");
    CHECK(mock_device_cs_high(&dev_b) == 0, "Release Bus B");
}

/* M03: Bus A 已被占用，Bus B 发起传输成功 (并发独立性) */
static void test_M03_bus_a_busy_bus_b_transfers(void)
{
    printf("\n=== M03: Bus A Busy, Bus B Transfers Independently ===\n");
    mock_hw_t hw_a, hw_b;
    spi_bus_t bus_a, bus_b;
    mock_device_t dev_a, dev_b;

    mock_hw_init(&hw_a);
    mock_hw_init(&hw_b);
    mock_bus_init(&bus_a, &hw_a);
    mock_bus_init(&bus_b, &hw_b);

    mock_device_init(&dev_a, "DEV_A", &bus_a, 18000000U, SPI_BUS_MODE_0, 1);
    mock_device_init(&dev_b, "DEV_B", &bus_b, 9000000U, SPI_BUS_MODE_0, 0);

    /* 设备 A 占用 Bus A */
    CHECK(mock_device_cs_low(&dev_a) == 0, "Dev A acquires Bus A");
    CHECK(spi_bus_owner(&bus_a) == &dev_a, "Bus A is held by Dev A");
    CHECK(spi_bus_depth(&bus_a) == 1U, "Bus A depth is 1");

    /* 此时设备 B 访问 Bus B，应当完全不受 Bus A 占用的影响 */
    CHECK(mock_device_cs_low(&dev_b) == 0, "Dev B acquires Bus B while Bus A is busy");
    CHECK(spi_bus_owner(&bus_b) == &dev_b, "Bus B is held by Dev B");

    uint8_t tx_b[4] = {0x11, 0x22, 0x33, 0x44};
    uint8_t rx_b[4] = {0};
    CHECK(mock_device_transfer(&dev_b, tx_b, rx_b, 4) == 0, "Dev B transfers on Bus B");
    CHECK(hw_b.transfer_count == 1U, "Bus B transfer executed");
    CHECK(hw_a.transfer_count == 0U, "Bus A transfer count untouched (0)");

    CHECK(mock_device_cs_high(&dev_b) == 0, "Dev B releases Bus B");
    CHECK(spi_bus_owner(&bus_b) == NULL, "Bus B released");

    /* Bus A 依然处于占用状态 */
    CHECK(spi_bus_owner(&bus_a) == &dev_a, "Bus A still held by Dev A");
    CHECK(mock_device_cs_high(&dev_a) == 0, "Dev A releases Bus A");
    CHECK(spi_bus_owner(&bus_a) == NULL, "Bus A released");
}

/* M04: 两个设备共用 Bus A，正确顺序仲裁 */
static void test_M04_two_devices_on_same_bus_arbitration(void)
{
    printf("\n=== M04: Two Devices on Bus A Proper Arbitration ===\n");
    mock_hw_t hw_a;
    spi_bus_t bus_a;
    mock_device_t dev_1, dev_2;

    mock_hw_init(&hw_a);
    mock_bus_init(&bus_a, &hw_a);

    mock_device_init(&dev_1, "TFT", &bus_a, 18000000U, SPI_BUS_MODE_0, 1);
    mock_device_init(&dev_2, "FLASH", &bus_a, 9000000U, SPI_BUS_MODE_0, 1);

    /* 1. Dev 1 占用 Bus A */
    CHECK(mock_device_cs_low(&dev_1) == 0, "Dev 1 acquires Bus A");
    CHECK(dev_1.cs_level == 0U, "Dev 1 CS asserted (LOW)");

    /* 2. Dev 2 试图抢占 Bus A -> 必须被拒绝 */
    CHECK(mock_device_cs_low(&dev_2) == -1, "Dev 2 acquire Bus A rejected while Dev 1 holds it");
    CHECK(dev_2.cs_level == 1U, "Dev 2 CS kept unasserted (HIGH)");
    CHECK(bus_a.rejects == 1U, "Bus A reject count incremented");

    /* 3. Dev 1 完成传输并释放 */
    uint8_t tx[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(mock_device_write(&dev_1, tx, 8) == 0, "Dev 1 transfers 8 bytes");
    CHECK(mock_device_cs_high(&dev_1) == 0, "Dev 1 releases Bus A");
    CHECK(dev_1.cs_level == 1U, "Dev 1 CS deasserted (HIGH)");
    CHECK(spi_bus_owner(&bus_a) == NULL, "Bus A now free");

    /* 4. Dev 2 再次申请 -> 成功 */
    CHECK(mock_device_cs_low(&dev_2) == 0, "Dev 2 now acquires Bus A successfully");
    CHECK(dev_2.cs_level == 0U, "Dev 2 CS asserted (LOW)");
    CHECK(mock_device_write(&dev_2, tx, 4) == 0, "Dev 2 transfers 4 bytes");
    CHECK(mock_device_cs_high(&dev_2) == 0, "Dev 2 releases Bus A");
    CHECK(dev_2.cs_level == 1U, "Dev 2 CS deasserted (HIGH)");
    CHECK(spi_bus_owner(&bus_a) == NULL, "Bus A returned to free state");

    CHECK(hw_a.transfer_count == 2U, "Total 2 sequential transfers executed on Bus A");
    CHECK(hw_a.total_bytes == 12U, "Total 12 bytes transferred on Bus A");
}

/* M05: 设备切换绑定 Bus A / Bus B，协议驱动逻辑不变 */
static void test_M05_device_switch_bus_binding(void)
{
    printf("\n=== M05: Device Rebinding Bus A -> Bus B Without Protocol Change ===\n");
    mock_hw_t hw_a, hw_b;
    spi_bus_t bus_a, bus_b;
    mock_device_t sensor;

    mock_hw_init(&hw_a);
    mock_hw_init(&hw_b);
    mock_bus_init(&bus_a, &hw_a);
    mock_bus_init(&bus_b, &hw_b);

    /* 1. Sensor 绑定在 Bus A */
    mock_device_init(&sensor, "Sensor", &bus_a, 4000000U, SPI_BUS_MODE_0, 0);

    CHECK(mock_device_cs_low(&sensor) == 0, "Sensor acquires on Bus A");
    uint8_t tx[2] = {0x9F, 0x00};
    uint8_t rx[2] = {0};
    CHECK(mock_device_transfer(&sensor, tx, rx, 2) == 0, "Sensor transfers on Bus A");
    CHECK(mock_device_cs_high(&sensor) == 0, "Sensor releases Bus A");
    CHECK(hw_a.transfer_count == 1U, "HW A transfer count is 1");
    CHECK(hw_b.transfer_count == 0U, "HW B transfer count is 0");

    /* 2. 动态重绑定至 Bus B（模拟通过配置注入切换总线） */
    sensor.bus = &bus_b;

    CHECK(mock_device_cs_low(&sensor) == 0, "Sensor acquires on Bus B");
    CHECK(mock_device_transfer(&sensor, tx, rx, 2) == 0, "Sensor transfers on Bus B");
    CHECK(mock_device_cs_high(&sensor) == 0, "Sensor releases Bus B");
    CHECK(hw_a.transfer_count == 1U, "HW A transfer count still 1");
    CHECK(hw_b.transfer_count == 1U, "HW B transfer count is now 1");
    CHECK(rx[0] == (0x9F ^ 0x5A), "Valid data looped back from HW B");
}

/* M06: Bus A 传输超时，Bus B 不受影响 */
static void test_M06_bus_a_timeout_does_not_affect_bus_b(void)
{
    printf("\n=== M06: Bus A Timeout Failure Isolated From Bus B ===\n");
    mock_hw_t hw_a, hw_b;
    spi_bus_t bus_a, bus_b;
    mock_device_t dev_a, dev_b;

    mock_hw_init(&hw_a);
    mock_hw_init(&hw_b);
    mock_bus_init(&bus_a, &hw_a);
    mock_bus_init(&bus_b, &hw_b);

    mock_device_init(&dev_a, "DEV_A", &bus_a, 18000000U, SPI_BUS_MODE_0, 1);
    mock_device_init(&dev_b, "DEV_B", &bus_b, 9000000U, SPI_BUS_MODE_0, 1);

    /* 在 HW A 上注入传输超时故障 */
    hw_a.inject_timeout = 1;

    CHECK(mock_device_cs_low(&dev_a) == 0, "Dev A acquires Bus A");
    uint8_t tx[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    int ret_a = mock_device_write(&dev_a, tx, 4);
    CHECK(ret_a == -2, "Bus A transfer reports timeout error (-2)");
    CHECK(mock_device_cs_high(&dev_a) == 0, "Dev A releases Bus A cleanly");

    /* 验证 Bus B 上的通信仍然完全正常 */
    CHECK(mock_device_cs_low(&dev_b) == 0, "Dev B acquires Bus B");
    int ret_b = mock_device_write(&dev_b, tx, 4);
    CHECK(ret_b == 0, "Bus B transfer succeeds normally despite Bus A failure");
    CHECK(hw_b.transfer_count == 1U, "HW B transfer count is 1");
    CHECK(hw_b.last_error == 0, "HW B error status is clean (0)");
    CHECK(mock_device_cs_high(&dev_b) == 0, "Dev B releases Bus B");
}

/* M07: Bus A 执行恢复，Bus B 状态不改变 */
static void test_M07_bus_a_recovery_isolated(void)
{
    printf("\n=== M07: Bus A Recovery Does Not Affect Bus B ===\n");
    mock_hw_t hw_a, hw_b;
    spi_bus_t bus_a, bus_b;

    mock_hw_init(&hw_a);
    mock_hw_init(&hw_b);
    mock_bus_init(&bus_a, &hw_a);
    mock_bus_init(&bus_b, &hw_b);

    hw_a.last_error = -2;
    hw_a.inject_timeout = 1;

    /* 针对 Bus A 执行异常恢复 */
    CHECK(mock_recover(&bus_a) == 0, "Bus A recover succeeds");
    CHECK(hw_a.recover_count == 1U, "HW A recover_count is 1");
    CHECK(hw_a.last_error == 0, "HW A last_error cleared");

    /* 验证 Bus B 的统计与状态完全未被触碰 */
    CHECK(hw_b.recover_count == 0U, "HW B recover_count is strictly 0");
    CHECK(hw_b.last_error == 0, "HW B last_error remains 0");
}

/* M08: 多实例完整初始化及嵌套释放，无状态泄漏 */
static void test_M08_full_lifecycle_and_nested_release(void)
{
    printf("\n=== M08: Multi-Instance Full Lifecycle & Reentrancy ===\n");
    mock_hw_t hw_a, hw_b;
    spi_bus_t bus_a, bus_b;
    mock_device_t dev_a, dev_b;

    mock_hw_init(&hw_a);
    mock_hw_init(&hw_b);
    mock_bus_init(&bus_a, &hw_a);
    mock_bus_init(&bus_b, &hw_b);

    mock_device_init(&dev_a, "DEV_A", &bus_a, 18000000U, SPI_BUS_MODE_0, 1);
    mock_device_init(&dev_b, "DEV_B", &bus_b, 9000000U, SPI_BUS_MODE_0, 1);

    /* 嵌套测试：Dev A 在 Bus A 嵌套 3 层 */
    CHECK(spi_bus_acquire(&bus_a, &dev_a, &dev_a.cfg) == 0, "Bus A depth 1");
    CHECK(spi_bus_depth(&bus_a) == 1U, "Depth is 1");
    CHECK(spi_bus_acquire(&bus_a, &dev_a, &dev_a.cfg) == 0, "Bus A depth 2");
    CHECK(spi_bus_depth(&bus_a) == 2U, "Depth is 2");
    CHECK(spi_bus_acquire(&bus_a, &dev_a, &dev_a.cfg) == 0, "Bus A depth 3");
    CHECK(spi_bus_depth(&bus_a) == 3U, "Depth is 3");

    /* 此时 Dev B 在 Bus B 进行单层调用 */
    CHECK(spi_bus_acquire(&bus_b, &dev_b, &dev_b.cfg) == 0, "Bus B depth 1");
    CHECK(spi_bus_depth(&bus_b) == 1U, "Bus B depth is 1");
    CHECK(spi_bus_release(&bus_b, &dev_b) == 1, "Bus B fully released");
    CHECK(spi_bus_depth(&bus_b) == 0U, "Bus B depth returns to 0");
    CHECK(spi_bus_owner(&bus_b) == NULL, "Bus B owner returns to NULL");

    /* Dev A 逐层释放 Bus A */
    CHECK(spi_bus_release(&bus_a, &dev_a) == 0, "Bus A release depth 3 -> 2");
    CHECK(spi_bus_depth(&bus_a) == 2U, "Depth is 2");
    CHECK(spi_bus_release(&bus_a, &dev_a) == 0, "Bus A release depth 2 -> 1");
    CHECK(spi_bus_depth(&bus_a) == 1U, "Depth is 1");
    CHECK(spi_bus_release(&bus_a, &dev_a) == 1, "Bus A release depth 1 -> 0 (fully returned)");
    CHECK(spi_bus_depth(&bus_a) == 0U, "Bus A depth returns to 0");
    CHECK(spi_bus_owner(&bus_a) == NULL, "Bus A owner returns to NULL");
}

/* ================================================================================== */
/* M09: BSP_SPI_RecoverBus 严格合法句柄校验                                            */
/* ================================================================================== */
static void test_M09_recover_bus_handle_validation(void)
{
    printf("\n[Test M09] BSP_SPI_RecoverBus Handle Validation\n");

    BSP_SPI_ResetBuses();
    mock_reset();

    /* 1. NULL 指针安全拒绝 */
    CHECK(BSP_SPI_RecoverBus(NULL) == -1, "Recover with NULL returns -1");

    /* 2. 未注册的伪造/外来总线指针安全拒绝 */
    spi_bus_t fake_bus;
    memset(&fake_bus, 0, sizeof(fake_bus));
    fake_bus.ctx = (void*)0x12345678;
    CHECK(BSP_SPI_RecoverBus(&fake_bus) == -1, "Recover with foreign/unregistered bus returns -1");

    /* 3. 在总线尚未被 BSP_SPI_GetBus 初始化前，未注册激活的总线句柄拒绝恢复 */
    CHECK(BSP_SPI_IsBusActive(BSP_SPI_BUS_1) == 0, "SPI1 initially not active");
    CHECK(BSP_SPI_IsBusActive(BSP_SPI_BUS_2) == 0, "SPI2 initially not active");

    /* 4. 获取 SPI1 后，合法 SPI1 总线恢复成功 */
    spi_bus_t *bus1 = BSP_SPI1_GetBus();
    CHECK(bus1 != NULL, "BSP_SPI1_GetBus returns non-NULL");
    CHECK(BSP_SPI_IsBusActive(BSP_SPI_BUS_1) == 1, "SPI1 is active");
    CHECK(BSP_SPI_RecoverBus(bus1) == 0, "Recover registered SPI1 bus returns 0");

    /* 5. 获取 SPI2 后，合法 SPI2 总线恢复成功 */
    spi_bus_t *bus2 = BSP_SPI2_GetBus();
    CHECK(bus2 != NULL, "BSP_SPI2_GetBus returns non-NULL");
    CHECK(BSP_SPI_IsBusActive(BSP_SPI_BUS_2) == 1, "SPI2 is active");
    CHECK(BSP_SPI_RecoverBus(bus2) == 0, "Recover registered SPI2 bus returns 0");

    /* 6. 再次传入未注册指针仍被拒绝 */
    CHECK(BSP_SPI_RecoverBus(&fake_bus) == -1, "Foreign bus still safely rejected");
}

/* ================================================================================== */
/* M10: GC9A01 静态槽位用尽保护与设备复用                                              */
/* ================================================================================== */
static void test_M10_gc9a01_slot_exhaustion_and_reuse(void)
{
    printf("\n[Test M10] GC9A01 Slot Exhaustion and Reuse\n");

    BSP_SPI_ResetBuses();
    GC9A01_Port_ResetSlots();
    mock_reset();

    spi_bus_t *bus1 = BSP_SPI1_GetBus();
    GC9A01_PortConfig cfg1 = {
        .bus          = bus1,
        .spi_clock_hz = 18000000U,
        .spi_mode     = SPI_BUS_MODE_0,
        .use_dma      = 1,
        .cs_port      = GPIOA,
        .cs_pin       = LL_GPIO_PIN_9,
        .rst_port     = GPIOA,
        .rst_pin      = LL_GPIO_PIN_10,
        .dc_port      = GPIOA,
        .dc_pin       = LL_GPIO_PIN_4,
    };

    LCD_Device dev1, dev2, dev3;
    memset(&dev1, 0, sizeof(dev1));
    memset(&dev2, 0, sizeof(dev2));
    memset(&dev3, 0, sizeof(dev3));

    /* 1. 初始化第 1 个设备 -> 成功占用 slot 1 */
    LCD_Result res1 = GC9A01_PortInitWithConfig(&dev1, &cfg1);
    CHECK(res1 == LCD_OK, "Init dev1 succeeds");

    /* 2. 重复初始化 dev1 -> 应当复用已有 slot，不浪费新 slot */
    cfg1.spi_clock_hz = 12000000U;
    LCD_Result res1_reinit = GC9A01_PortInitWithConfig(&dev1, &cfg1);
    CHECK(res1_reinit == LCD_OK, "Re-init dev1 succeeds and reuses existing slot");

    /* 3. 初始化默认 g_lcd -> 成功占用 slot 0 */
    LCD_Result res_def = GC9A01_PortInit();
    CHECK(res_def == LCD_OK, "Init default g_lcd succeeds in slot 0");

    /* 此时 slot 0 和 slot 1 均已被占用 (GC9A01_MAX_INSTANCES = 2) */

    /* 4. 初始化第 3 个不同设备 dev2 -> 槽位已满，必须直接返回 LCD_ERROR_PARAM */
    LCD_Result res2 = GC9A01_PortInitWithConfig(&dev2, &cfg1);
    CHECK(res2 == LCD_ERROR_PARAM, "Init dev2 fails with LCD_ERROR_PARAM due to slot exhaustion");

    /* 5. 验证已分配的 dev1 上下文未被破坏覆盖 */
    CHECK(dev1.port != NULL, "dev1 port table remains intact");
    CHECK(dev1.port->write != NULL, "dev1 write op intact");

    /* 6. 初始化 dev3 同样安全拒绝 */
    LCD_Result res3 = GC9A01_PortInitWithConfig(&dev3, &cfg1);
    CHECK(res3 == LCD_ERROR_PARAM, "Init dev3 fails with LCD_ERROR_PARAM");
}

/* ================================================================================== */
/* M11: W25QXX 静态槽位用尽保护与设备复用                                              */
/* ================================================================================== */
static void test_M11_w25qxx_slot_exhaustion_and_reuse(void)
{
    printf("\n[Test M11] W25QXX Slot Exhaustion and Reuse\n");

    BSP_SPI_ResetBuses();
    W25QXX_Port_ResetSlots();
    mock_reset();

    spi_bus_t *bus1 = BSP_SPI1_GetBus();
    W25QXX_PortConfig cfg1 = {
        .bus          = bus1,
        .spi_clock_hz = 9000000U,
        .spi_mode     = SPI_BUS_MODE_0,
        .use_dma      = 1,
        .cs_port      = GPIOA,
        .cs_pin       = LL_GPIO_PIN_8,
    };

    W25QXX_Device flash1, flash2, flash3;
    memset(&flash1, 0, sizeof(flash1));
    memset(&flash2, 0, sizeof(flash2));
    memset(&flash3, 0, sizeof(flash3));

    /* 1. 初始化 flash1 -> 占用 slot 1 */
    W25QXX_Result res1 = W25QXX_PortInitWithConfig(&flash1, &cfg1);
    CHECK(res1 == W25QXX_OK, "Init flash1 succeeds");

    /* 2. 重复初始化 flash1 -> 复用现有槽位 */
    cfg1.spi_clock_hz = 6000000U;
    W25QXX_Result res1_reinit = W25QXX_PortInitWithConfig(&flash1, &cfg1);
    CHECK(res1_reinit == W25QXX_OK, "Re-init flash1 succeeds and reuses existing slot");

    /* 3. 初始化默认 g_flash -> 占用 slot 0 */
    W25QXX_PortInit();
    CHECK(g_flash.bus != NULL, "Init default g_flash succeeds in slot 0");

    /* 4. 初始化第 3 个不同设备 flash2 -> 槽位已满，必须直接返回 W25QXX_ERROR_PARAM */
    W25QXX_Result res2 = W25QXX_PortInitWithConfig(&flash2, &cfg1);
    CHECK(res2 == W25QXX_ERROR_PARAM, "Init flash2 fails with W25QXX_ERROR_PARAM due to slot exhaustion");

    /* 5. 验证已分配 flash1 上下文未被破坏覆盖 */
    CHECK(flash1.bus != NULL, "flash1 bus ops intact");

    /* 6. 初始化 flash3 同样安全拒绝 */
    W25QXX_Result res3 = W25QXX_PortInitWithConfig(&flash3, &cfg1);
    CHECK(res3 == W25QXX_ERROR_PARAM, "Init flash3 fails with W25QXX_ERROR_PARAM");
}

/* ================================================================================== */
/* M12: 双 SPI GPIO 资源冲突检查与配置约束                                            */
/* ================================================================================== */
static void test_M12_dual_spi_gpio_conflict_guard(void)
{
    printf("\n[Test M12] Dual SPI GPIO Conflict Guard\n");

    BSP_SPI_ResetBuses();
    GC9A01_Port_ResetSlots();
    mock_reset();

    spi_bus_t *bus1 = BSP_SPI1_GetBus();

    /* 场景 A: TFT DC 绑定 PB13 时，尝试启用 SPI2 (SCK 为 PB13) */
    GC9A01_PortConfig cfg_pb13 = {
        .bus          = bus1,
        .spi_clock_hz = 18000000U,
        .spi_mode     = SPI_BUS_MODE_0,
        .use_dma      = 1,
        .cs_port      = GPIOA,
        .cs_pin       = LL_GPIO_PIN_9,
        .rst_port     = GPIOA,
        .rst_pin      = LL_GPIO_PIN_10,
        .dc_port      = GPIOB,
        .dc_pin       = LL_GPIO_PIN_13, /* 占用 PB13 */
    };

    LCD_Device lcd_dev;
    CHECK(GC9A01_PortInitWithConfig(&lcd_dev, &cfg_pb13) == LCD_OK, "Init LCD with DC=PB13 on SPI1 succeeds");
    CHECK(GC9A01_Port_IsPinUsed(GPIOB, LL_GPIO_PIN_13) == 1, "GC9A01_Port_IsPinUsed reports PB13 occupied");

    /* 此时尝试获取 SPI2：因 PB13 已被 TFT DC 占用，必须返回 NULL 拒绝使能 */
    spi_bus_t *spi2_conflict = BSP_SPI_GetBus(BSP_SPI_BUS_2);
    CHECK(spi2_conflict == NULL, "BSP_SPI_GetBus(SPI2) returns NULL when PB13 is used by TFT DC");
    CHECK(BSP_SPI_IsBusActive(BSP_SPI_BUS_2) == 0, "SPI2 is NOT active due to GPIO conflict");

    /* 场景 B: 复位环境，先启用 SPI2，再尝试将 TFT DC 绑定至 PB13 */
    BSP_SPI_ResetBuses();
    GC9A01_Port_ResetSlots();
    mock_reset();

    spi_bus_t *spi2_ok = BSP_SPI_GetBus(BSP_SPI_BUS_2);
    CHECK(spi2_ok != NULL, "SPI2 initializes cleanly when PB13 is free");
    CHECK(BSP_SPI_IsBusActive(BSP_SPI_BUS_2) == 1, "SPI2 is active");

    /* 尝试将 TFT DC 配置为 PB13 -> 必须被 GC9A01_PortInitWithConfig 拒绝 */
    cfg_pb13.bus = BSP_SPI1_GetBus();
    LCD_Result res_conflict = GC9A01_PortInitWithConfig(&lcd_dev, &cfg_pb13);
    CHECK(res_conflict == LCD_ERROR_PARAM, "Init LCD with DC=PB13 rejected when SPI2 is active");

    /* 场景 C: 将 TFT DC 切换至非冲突引脚 PA4 -> 顺利共存 */
    GC9A01_PortConfig cfg_pa4 = cfg_pb13;
    cfg_pa4.bus     = BSP_SPI1_GetBus();
    cfg_pa4.dc_port = GPIOA;
    cfg_pa4.dc_pin  = LL_GPIO_PIN_4; /* 改用 PA4 */

    LCD_Result res_coexist = GC9A01_PortInitWithConfig(&lcd_dev, &cfg_pa4);
    CHECK(res_coexist == LCD_OK, "Init LCD with DC=PA4 succeeds alongside active SPI2");
    CHECK(GC9A01_Port_IsPinUsed(GPIOA, LL_GPIO_PIN_4) == 1, "PA4 reported in use by LCD");
    CHECK(GC9A01_Port_IsPinUsed(GPIOB, LL_GPIO_PIN_13) == 0, "PB13 reported NOT used by LCD");
}

/* ================================================================================== */
/* 主函数                                                                             */
/* ================================================================================== */

int main(void)
{
    printf("====================================================================\n");
    printf("Stage 2.2-A SPI Multi-Instance & Config Injection Unit Tests\n");
    printf("====================================================================\n");

    test_M01_create_independent_buses();
    test_M02_independent_configs();
    test_M03_bus_a_busy_bus_b_transfers();
    test_M04_two_devices_on_same_bus_arbitration();
    test_M05_device_switch_bus_binding();
    test_M06_bus_a_timeout_does_not_affect_bus_b();
    test_M07_bus_a_recovery_isolated();
    test_M08_full_lifecycle_and_nested_release();
    test_M09_recover_bus_handle_validation();
    test_M10_gc9a01_slot_exhaustion_and_reuse();
    test_M11_w25qxx_slot_exhaustion_and_reuse();
    test_M12_dual_spi_gpio_conflict_guard();

    printf("\n--------------------------------------------------------------------\n");
    printf("Summary: %d assertions, %d failures\n", g_assert_count, g_fail);
    printf("====================================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
