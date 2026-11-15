# SPI 总线与设备移植开发指南 (SPI Porting Guide)

## 一、系统分层架构与设计原则

本项目采用清晰的四层解耦架构，实现硬件外设、总线仲裁、设备协议与应用逻辑的完全隔离：

```
+------------------------------------------------------------------------+
|                              应用逻辑层                                 |
|          (main.c, app_test.c, UI 控件, 文件系统 / Flash 数据管理)         |
+------------------------------------------------------------------------+
                                    |
          +-------------------------+-------------------------+
          |                                                   |
+-----------------------+                           +--------------------+
|  GC9A01 屏幕协议驱动   |                           |  W25QXX Flash 驱动 |
|   (gc9a01.c / .h)     |                           |   (w25qxx.c / .h)  |
|     * 纯协议逻辑      |                           |    * 纯协议逻辑    |
|     * MCU 无关        |                           |    * MCU 无关      |
+-----------------------+                           +--------------------+
          |                                                   |
+-----------------------+                           +--------------------+
|  GC9A01 端口适配层    |                           |  W25QXX 端口适配层 |
| (gc9a01_port.c / .h)  |                           | (w25qxx_port.c/.h) |
|   * CS/DC/RST GPIO    |                           |     * CS GPIO      |
|   * 配置注入适配      |                           |   * 配置注入适配   |
+-----------------------+                           +--------------------+
          |                                                   |
          +-------------------------+-------------------------+
                                    |
+------------------------------------------------------------------------+
|                           SPI 抽象总线核心                             |
|                              (spi_bus.h)                               |
|        * 纯 C 接口，零 MCU 依赖                                         |
|        * 互斥仲裁 (acquire / release)、嵌套重入、ISR 抢占防御           |
+------------------------------------------------------------------------+
                                    |
+------------------------------------------------------------------------+
|                         板级硬件与外设实现层                           |
|      (bsp_spi.c / .h, spi_stm32_ll.c / .h, STM32 LL / CMSIS 驱动)      |
|        * 物理 SPI1 / SPI2 外设实例管理                                 |
|        * SCK/MISO/MOSI 复用、DMA1 通道配置、波特率分频换算             |
+------------------------------------------------------------------------+
```

### 设计原则：
1. **协议驱动纯净性**：`gc9a01.c/h` 和 `w25qxx.c/h` 绝不包含任何 MCU 头文件（如 `stm32f1xx.h`）或板级入口（如 `BSP_SPI1_GetBus`）。
2. **总线核心通用性**：`spi_bus.h` 不定义任何硬件寄存器或引脚宏，支持任意 32 位 MCU。
3. **配置注入灵活性**：通过 `GC9A01_PortInitWithConfig` 与 `W25QXX_PortInitWithConfig` 显式注入目标总线与控制引脚。

---

## 二、配置与使用示例

### 示例 A：GC9A01 + W25Q128 共享 SPI1 单总线（当前默认硬件配置）

两设备物理共用同一组 SCK(PA5)、MISO(PA6)、MOSI(PA7)，由 `spi_bus` 自动仲裁切换时钟频率（TFT 18MHz / Flash 9MHz）与片选：

```c
#include "bsp_spi.h"
#include "gc9a01_port.h"
#include "w25qxx_port.h"

void App_Init_Shared_SPI1(void)
{
    /* 1. 获取共享 SPI1 抽象总线句柄 */
    spi_bus_t *bus1 = BSP_SPI1_GetBus();

    /* 2. 配置并初始化 GC9A01（18MHz，模式 0，DMA 启用） */
    GC9A01_PortConfig lcd_cfg = {
        .bus          = bus1,
        .spi_clock_hz = 18000000U,
        .spi_mode     = SPI_BUS_MODE_0,
        .use_dma      = 1U,
        .cs_port      = GPIOA,
        .cs_pin       = LL_GPIO_PIN_9,
        .rst_port     = GPIOA,
        .rst_pin      = LL_GPIO_PIN_10,
        .dc_port      = GPIOB,
        .dc_pin       = LL_GPIO_PIN_13,
    };
    (void)GC9A01_PortInitWithConfig(&g_lcd, &lcd_cfg);

    /* 3. 配置并初始化 W25Q128（9MHz，模式 0，DMA 启用） */
    W25QXX_PortConfig flash_cfg = {
        .bus          = bus1,
        .spi_clock_hz = 9000000U,
        .spi_mode     = SPI_BUS_MODE_0,
        .use_dma      = 1U,
        .cs_port      = GPIOA,
        .cs_pin       = LL_GPIO_PIN_8,
    };
    (void)W25QXX_PortInitWithConfig(&g_flash, &flash_cfg);
}
```

> **仲裁机制**：GC9A01 刷屏占用期间，W25QXX 若尝试访问总线会被 `spi_bus_acquire` 安全拒绝，片选保持高电平；刷屏完成释放后，Flash 立即无缝切至 9MHz 执行读写。

---

### 示例 B：GC9A01 使用 Bus A，W25Q128 使用 Bus B 双总线（独立运行模式）

在具备两条物理 SPI 或 Host 仿真环境下，两设备分别绑定不同的总线实例，实现互不阻塞的并发操作：

```c
#include "bsp_spi.h"
#include "gc9a01_port.h"
#include "w25qxx_port.h"

void App_Init_Dual_Bus(void)
{
    /* 1. 分别获取 Bus A (SPI1) 和 Bus B (SPI2) 实例 */
    spi_bus_t *bus_a = BSP_SPI_GetBus(BSP_SPI_BUS_1);
    spi_bus_t *bus_b = BSP_SPI_GetBus(BSP_SPI_BUS_2);

    /* 2. GC9A01 注入绑定 Bus A */
    GC9A01_PortConfig lcd_cfg = {
        .bus          = bus_a,
        .spi_clock_hz = 18000000U,
        .spi_mode     = SPI_BUS_MODE_0,
        .use_dma      = 1U,
        .cs_port      = GPIOA,
        .cs_pin       = LL_GPIO_PIN_9,
        .rst_port     = GPIOA,
        .rst_pin      = LL_GPIO_PIN_10,
        .dc_port      = GPIOB,
        .dc_pin       = LL_GPIO_PIN_13,
    };
    (void)GC9A01_PortInitWithConfig(&g_lcd, &lcd_cfg);

    /* 3. W25Q128 注入绑定 Bus B (独立总线) */
    W25QXX_PortConfig flash_cfg = {
        .bus          = bus_b,
        .spi_clock_hz = 9000000U,
        .spi_mode     = SPI_BUS_MODE_0,
        .use_dma      = 1U,
        .cs_port      = GPIOB,
        .cs_pin       = LL_GPIO_PIN_12,
    };
    (void)W25QXX_PortInitWithConfig(&g_flash, &flash_cfg);
}
```

> **独立性收益**：Bus A 发生大块 DMA 刷屏或异常恢复时，Bus B 上的 Flash 访问与恢复完全独立，互无干扰与串扰。

---

## 三、更换 SPI 外设或移植至新 MCU 的文件修改清单

| 层次 | 文件路径 | 更换 SPI 外设 (如 SPI1 -> SPI2) | 移植到其他 32 位 MCU (如 GD32/STM32H7/ESP32) |
|---|---|---|---|
| **协议层** | `BSP/Src/gc9a01.c` / `gc9a01.h` | **无需修改 (0%)** | **无需修改 (0%)** |
| **协议层** | `BSP/Src/w25qxx.c` / `w25qxx.h` | **无需修改 (0%)** | **无需修改 (0%)** |
| **总线层** | `BSP/Inc/spi_bus.h` | **无需修改 (0%)** | **无需修改 (0%)** |
| **应用层** | `Core/Src/main.c` / `app_test.c` | **无需修改** (仅改配置注入结构体) | **无需修改** |
| **适配层** | `BSP/Src/gc9a01_port.c` / `.h` | 仅调整 `GC9A01_PortConfig` 端口参数 | 实现对应平台的 GPIO/延时回调 |
| **适配层** | `BSP/Src/w25qxx_port.c` / `.h` | 仅调整 `W25QXX_PortConfig` 片选引脚 | 实现对应平台的片选 GPIO 回调 |
| **板级层** | `BSP/Src/bsp_spi.c` / `bsp_spi.h` | 在板级增加新外设配置映射 | 替换为新 MCU 的总线实例化工厂 |
| **硬件层** | `BSP/Src/spi_stm32_ll.c` / `.h` | 复用现有实现，传入对应 HW 结构体 | 使用目标 MCU LL/HAL 编写一套底层适配器 |

---

## 四、移植关键注意事项

1. **CS 片选生命周期安全**：
   - 片选拉低与拉高必须严格与 `spi_bus_acquire()` 和 `spi_bus_release()` 返回值关联。
   - `spi_bus_release()` 返回 1 时才允许将 CS 拉高（事务彻底完成）；返回 0 表示同 owner 嵌套未结束，CS 必须维持低电平有效。
2. **上下文持久化规则**：
   - 端口层必须将传入的 `config` 结构体内容深拷贝至生命周期覆盖整个运行期的静态上下文槽位中，禁止保存局部栈变量地址。
3. **时钟频率与模式隔离**：
   - 不同总线实例维护独立的 `current_cfg`，各总线切换频率时互不影响；同总线多设备切换前由总线层自动判定配置差异并按需重设分频器。
