# 嵌入式双 SPI 驱动架构设计规范 (Architecture)

## 一、分层架构设计与原则

本项目针对资源受限的 Cortex-M3 微控制器（STM32F103C8T6，64KB Flash / 20KB SRAM），采用高内聚、低耦合的五层模块化架构：

```mermaid
flowchart TD
    subgraph AppLayer [1. 应用逻辑层]
        APP_DEMO["交互式 Demo (app_demo.c)"]
        APP_HIL["HIL 自动化测试 (app_hil_test.c)"]
    end

    subgraph ProtocolLayer [2. 设备协议驱动层 (MCU 无关)]
        DRV_TFT["GC9A01 协议驱动 (gc9a01.c)"]
        DRV_FLASH["W25QXX 协议驱动 (w25qxx.c)"]
    end

    subgraph PortLayer [3. 设备端口移植层 (配置注入)]
        PORT_TFT["GC9A01 移植层 (gc9a01_port.c)"]
        PORT_FLASH["W25QXX 移植层 (w25qxx_port.c)"]
    end

    subgraph BusLayer [4. SPI 抽象总线核心 (spi_bus.h)]
        BUS_CORE["spi_bus_t 抽象接口\n* 互斥获取 (acquire)\n* 总线释放 (release)\n* 块传输 (transfer)\n* 速率/模式切换 (configure)"]
    end

    subgraph HwLayer [5. 硬件与 LL 实现层]
        BSP_SPI["板级 SPI 实例管理 (bsp_spi.c)"]
        LL_SPI["STM32 LL 硬件适配 (spi_stm32_ll.c)"]
        HW_PERIPH["STM32 硬件外设 (SPI1, SPI2, DMA1, GPIO)"]
    end

    AppLayer --> ProtocolLayer
    AppLayer --> PortLayer
    ProtocolLayer --> PortLayer
    PortLayer --> BusLayer
    BusLayer --> HwLayer
```

---

## 二、各层核心职责

### 1. 应用逻辑层 (Application Layer)
- **`app_demo.c / app_demo.h`**：提供单按键交互式多页面用户界面，负责状态机切换、图形组织与按键业务处理。
- **`app_hil_test.c / app_hil_test.h`**：硬件在环自动化测试，与正式固件完全解耦，仅在开启 `CONFIG_HIL_TESTS` 时编译。
- **职责边界**：不直接操作寄存器，不直接调用底层 SPI 发送函数，全部通过协议层接口操作。

### 2. 设备协议驱动层 (Protocol Layer)
- **`gc9a01.c / gc9a01.h`**：GC9A01 液晶控制器协议实现，包含初始化序列、窗口设置、几何绘图、字符与数字渲染。
- **`w25qxx.c / w25qxx.h`**：W25QXX 系列 SPI NOR Flash 协议实现，包含 JEDEC ID 读取、状态寄存器查询、读写操作与擦除控制。
- **核心原则**：**严格与 MCU 硬件解耦**。代码中绝不包含 `stm32f1xx.h`、寄存器指针或特定硬件库。所有通信均通过抽象函数指针委托给端口层。

### 3. 设备端口移植层 (Port Layer)
- **`gc9a01_port.c / gc9a01_port.h`**：适配 GC9A01 的 CS、DC、RST 引脚控制与 SPI 抽象总线绑定。支持配置注入（`GC9A01_PortInitWithConfig`）。
- **`w25qxx_port.c / w25qxx_port.h`**：适配 W25QXX 的 CS 引脚控制与 SPI 抽象总线绑定。支持配置注入（`W25QXX_PortInitWithConfig`）。
- **职责边界**：负责板级具体的 GPIO 高低电平操作，将协议层的数据传输请求转交至绑定的 `spi_bus_t` 对象。

### 4. SPI 抽象总线层 (Bus Abstraction Layer)
- **`spi_bus.h`**：纯 C 语言编写的 SPI 总线抽象接口。
- **核心机制**：
  1. **互斥仲裁 (`acquire / release`)**：同一时刻仅允许一个设备持有总线，防止时序交织。
  2. **嵌套重入 (`re-entrancy`)**：同一持有者允许嵌套调用 `acquire`，内部维护递归深度计数器，仅在最外层释放时真正归还总线。
  3. **中断抢占防御**：禁止中断上下文尝试抢占正在进行 DMA 传输的总线。
  4. **动态参数重配 (`configure`)**：不同设备持有总线时可按需自动切换波特率与时钟极性/相位。

### 5. 板级硬件实现层 (Hardware LL Layer)
- **`bsp_spi.c / bsp_spi.h`**：板级 SPI 实例管理器。实例化 SPI1 与 SPI2 对象，分配硬件引脚与 DMA 通道。
- **`spi_stm32_ll.c / spi_stm32_ll.h`**：实现基于 STM32 低层 (LL) 库的 SPI 硬件驱动，包含轮询与 DMA 传输引擎、总线超时与硬件异常恢复机制。

---

## 三、内存架构与安全性保证

### 1. 零全屏帧缓冲 (Zero Full FrameBuffer)
- 240×240 分辨率、RGB565 格式的全屏显存需要：
  $$240 \times 240 \times 2 = 115,200 \text{ 字节} \approx 112.5 \text{ KB}$$
- STM32F103C8T6 物理 SRAM 仅有 **20 KB**，根本无法开辟全屏 FrameBuffer。
- 本架构采用**行缓冲局部刷屏算法**（Line-Buffer Rendering），最大单次传输缓冲控制在 **480 字节**（单行像素），通过 DMA 循环泵送完成高速绘图，总 RAM 占用仅约 **3 KB**（占 SRAM 的 15%）。

### 2. 严格零动态内存分配 (Zero Dynamic Allocation)
- 全工程禁止使用 `malloc`、`calloc`、`free`。
- 驱动上下文、菜单状态机、按键事件与临时比对缓冲区全部静态分配（BSS/Data 段），彻底杜绝内存碎片化风险与堆溢出隐患。

### 3. Flash 安全保护机制
- 正式应用 Demo 运行期间，外部 Flash 处于**严格只读模式**。
- 固件中严禁调用 `W25QXX_EraseChip`、`W25QXX_EraseSector` 或 `W25QXX_Write`。
- 仅在显式配置 `CONFIG_HIL_TESTS=1` 且由开发者手动定义 `CONFIG_FLASH_DESTRUCTIVE_TEST=1` 时，才允许在远离代码区的专用安全测试扇区（`0x00100000`）执行写入测试。
