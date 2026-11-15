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
typedef struct {
    int (*configure)(void *ctx, const spi_bus_config_t *cfg);
    int (*transfer)(void *ctx, const void *tx, void *rx, size_t len);
    int (*recover)(void *ctx);
} spi_bus_ops_t;
```

### 2. 各函数实现要求

1. **`configure`**：根据传入的 `cfg->clock_hz` 和 `cfg->mode` 配置底层外设波特率分频与 CPOL/CPHA。
2. **`transfer`**：实现数据双向同步交换或单向发送（支持轮询或 DMA）。返回 0 表示成功，负数表示超时或硬件错误。
3. **`recover`**：在总线异常（如从机拉死时钟、DMA 悬挂）时重置外设状态机。

### 3. 当前移植与验证状态说明

> [!NOTE]
> - **已实机验证平台**：STM32F103C8T6 (Cortex-M3 @ 72MHz)，基于 STM32 LL 库与 DMA1。
> - **其他平台状态**：系统协议层（`gc9a01.c` 与 `w25qxx.c`）已通过 Host 单元测试验证其纯 C 逻辑独立性；其他 MCU 平台的实际硬件适配器（Adapter）尚未在物理硬件上进行量产级回归，开发者如需移植需自行实现对应芯片的底层硬件驱动。
