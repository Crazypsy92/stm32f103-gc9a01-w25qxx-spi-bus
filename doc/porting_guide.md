# 驱动移植与二次开发指南 (Porting Guide)

本文档面向希望将本项目驱动移植到其他引脚、其他 STM32 芯片系列或第三方 MCU 架构的开发者。

---

## 一、引脚修改指南（同一 MCU 系列内）

若您使用的是不同的 STM32F103 开发板，只需修改端口适配层中的引脚宏定义即可：

### 1. 修改 GC9A01 屏幕控制引脚

在 `BSP/Src/gc9a01_port.c` 中修改：

```c
/* 默认 GC9A01 引脚映射 */
#define GC9A01_DEFAULT_CS_PORT    GPIOA
#define GC9A01_DEFAULT_CS_PIN     LL_GPIO_PIN_9
#define GC9A01_DEFAULT_RST_PORT   GPIOA
#define GC9A01_DEFAULT_RST_PIN    LL_GPIO_PIN_10
#define GC9A01_DEFAULT_DC_PORT    GPIOA
#define GC9A01_DEFAULT_DC_PIN     LL_GPIO_PIN_4
```

> [!WARNING]
> 确保所选的 DC 引脚不要与 SPI 外设引脚（如 SPI2_SCK 占用的 PB13）发生冲突。

### 2. 修改 W25Q128 Flash 片选引脚

在 `BSP/Src/w25qxx_port.c` 中修改：

```c
#define W25QXX_DEFAULT_CS_PORT    GPIOA
#define W25QXX_DEFAULT_CS_PIN     LL_GPIO_PIN_8
```

### 3. 修改按键 KEY1 与 LED 引脚

在 `Core/Inc/main.h` 与 `BSP/Inc/key_port.h` 中修改：

```c
#define led_Pin LL_GPIO_PIN_13
#define led_GPIO_Port GPIOC
#define key1_Pin LL_GPIO_PIN_12
#define key1_GPIO_Port GPIOB
```

---

## 二、配置注入接口（多设备动态创建）

本项目支持在运行时动态创建多个独立的设备实例，无需直接修改驱动源码。

### 1. GC9A01 配置注入示例

```c
LCD_Device custom_lcd;

GC9A01_PortConfig tft_cfg = {
    .bus          = BSP_SPI_GetBus(BSP_SPI_BUS_1), /* 绑定 SPI1 总线 */
    .spi_clock_hz = 18000000U,                     /* 18 MHz */
    .spi_mode     = SPI_BUS_MODE_0,
    .use_dma      = 1U,                            /* 启用 DMA */
    .cs_port      = GPIOA,
    .cs_pin       = LL_GPIO_PIN_9,
    .rst_port     = GPIOA,
    .rst_pin      = LL_GPIO_PIN_10,
    .dc_port      = GPIOA,
    .dc_pin       = LL_GPIO_PIN_4,
};

LCD_Result res = GC9A01_PortInitWithConfig(&custom_lcd, &tft_cfg);
if (res == LCD_OK) {
    LCD_Fill(&custom_lcd, 0, 0, LCD_W, LCD_H, BLACK);
}
```

### 2. W25QXX 配置注入示例

```c
W25QXX_Device custom_flash;

W25QXX_PortConfig flash_cfg = {
    .bus          = BSP_SPI_GetBus(BSP_SPI_BUS_2), /* 绑定 SPI2 总线 */
    .spi_clock_hz = 9000000U,                      /* 9 MHz */
    .spi_mode     = SPI_BUS_MODE_0,
    .use_dma      = 1U,
    .cs_port      = GPIOA,
    .cs_pin       = LL_GPIO_PIN_8,
};

W25QXX_Result f_res = W25QXX_PortInitWithConfig(&custom_flash, &flash_cfg);
if (f_res == W25QXX_OK) {
    uint16_t id = W25QXX_ReadID(&custom_flash);
}
```

---

## 三、跨 MCU 系列与平台移植

系统架构通过 `spi_bus.h` 实现了总线与 MCU 底层硬件的彻底隔离。

### 1. SPI 总线抽象接口 (`spi_bus_ops_t`)

若要移植至 STM32F4、STM32G4、GD32、ESP-IDF 或 Linux 用户态 (spidev)，只需实现以下操作函数表并注册为 `spi_bus_t` 对象：

```c
/* 与 BSP/Inc/spi_bus.h 的实际声明一致。ctx 位于 spi_bus_t 内。 */
typedef struct {
    int (*configure)(spi_bus_t *bus, const spi_bus_config_t *cfg);
    int (*transfer)(spi_bus_t *bus, const uint8_t *tx,
                    uint8_t *rx, uint32_t len);
    int (*in_isr)(spi_bus_t *bus); /* 可选：ISR 上下文检测 */
} spi_bus_ops_t;
```

### 2. 各函数实现要求

1. **`configure`**：根据传入的 `cfg->clock_hz` 和 `cfg->mode` 配置底层外设波特率分频与 CPOL/CPHA。
2. **`transfer`**：实现数据双向同步交换或单向发送（支持轮询或 DMA）。返回 0 表示成功，负数表示超时或硬件错误。
3. **`in_isr`**：可选；返回 1 表示当前是中断上下文，禁止在 ISR 中占用 SPI 总线。
4. **异常恢复**：当前板级实现通过 `BSP_SPI_RecoverBus(bus)` 或底层 `spi_stm32_ll_recover()` 提供；`spi_bus_ops_t` 本身**没有** `recover` 函数指针。
5. **延时移植**：`w25qxx.c` 当前通过 `delay.h` 使用 `Delay_us`/`Delay_ms`；移植到其他 MCU 前需提供对应时基，或者进一步将延时回调注入 W25QXX Port 层。

### 3. 当前移植与验证状态说明

> [!NOTE]
> - **已实机验证平台**：STM32F103C8T6 (Cortex-M3 @ 72MHz)，基于 STM32 LL 库与 DMA1。
> - **其他平台状态**：GC9A01 协议层已通过 Host Mock 测试；W25QXX 的 SPI 操作已通过 Port 回调隔离，但仍依赖项目的延时接口。其他 MCU 的真实硬件适配尚未验证，开发者应移植 SPI/GPIO/时间基准后分别测试。
- **写入风险**：`W25QXX_Write()` 不是保留扇区原有数据的通用局部更新。它会先擦除受影响的整个 4KB 扇区，随后只写入指定缓冲；无关数据可能丢失。正式 Demo 无任何写入行为。
