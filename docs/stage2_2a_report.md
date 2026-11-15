# Stage 2.2-A：SPI 多实例与设备配置注入整改报告

## 一、任务背景与整改目标

在 Stage 2.1 完成 GC9A01 驱动规范化与全链路错误传播的基础上，为提高 SPI 总线框架与外设驱动的可复用性，Stage 2.2-A 重点完成从“固定 SPI1 共享总线”向“多 SPI 总线实例与设备配置注入”的架构演进。

### 核心目标：
1. **总线多实例支持**：解耦单一固定全局总线假设，支持创建并独立运行多个 `spi_bus_t` 实例（如 Bus A / Bus B），各实例独立维护外设句柄、时钟分频、工作模式、DMA 资源、事务占用者（owner）、嵌套深度（depth）与错误恢复状态。
2. **设备驱动配置注入**：在 GC9A01 与 W25QXX 移植层引入配置注入接口（`GC9A01_PortInitWithConfig` 与 `W25QXX_PortInitWithConfig`），允许板级或应用主动指定设备挂载的总线及控制引脚，设备协议层不依赖任何板级单例。
3. **保持向后兼容**：保留原有 `BSP_SPI1_GetBus()`、`GC9A01_PortInit()`、`W25QXX_PortInit()` 封装，不破坏已有上层代码与测试。
4. **Host 仿真与多实例专项测试**：新增 `test_spi_multi_instance.c`，覆盖 M01 ~ M08 场景。

---

## 二、架构设计与核心代码实现

### 2.1 Task 1：SPI 总线多实例管理

涉及文件：
- `BSP/Inc/spi_stm32_ll.h`
- `BSP/Src/spi_stm32_ll.c`
- `BSP/Inc/bsp_spi.h`
- `BSP/Src/bsp_spi.c`

#### 关键改动：
1. **实例状态与错误隔离**：
   在 `spi_stm32_ll_t` 结构体中新增实例级错误与恢复统计字段：
   ```c
   typedef struct {
       spi_bus_t            bus;           /* 对外的抽象总线 */
       const spi_stm32_ll_hw_t *hw;        /* 硬件描述 */
       spi_bus_config_t     current;       /* 当前生效参数 */
       uint8_t              configured;    /* 首次配置标志 */
       uint32_t             recover_count; /* 实例独立恢复次数 */
       int32_t              last_error;    /* 实例独立错误码 */
   } spi_stm32_ll_t;
   ```
   在 `spi_recover()` 与 `ll_transfer()` 中优先更新本实例的统计状态，并导出 `spi_stm32_ll_get_recover_count`、`spi_stm32_ll_get_last_error` 和 `spi_stm32_ll_clear_error` 接口。
2. **多总线实例管理工厂**：
   在 `bsp_spi.h` 中定义总线枚举 `BSP_SPI_BUS_1`、`BSP_SPI_BUS_2`；在 `bsp_spi.c` 中定义 `s_spi1_hw` 与 `s_spi2_hw` 独立硬件描述，使用实例数组 `s_spi_instances[BSP_SPI_BUS_MAX]` 进行惰性按需初始化：
   ```c
   spi_bus_t *BSP_SPI_GetBus(bsp_spi_id_t id);
   spi_bus_t *BSP_SPI1_GetBus(void);
   spi_bus_t *BSP_SPI2_GetBus(void);
   int        BSP_SPI_RecoverBus(spi_bus_t *bus);
   int        BSP_SPI1_Recover(void);
   ```
3. **消除硬件假设**：
   通用 `spi_bus.h` 保持纯 C 接口与零 MCU 依赖，无任何特定引脚或寄存器定义。

---

### 2.2 Task 2：设备驱动配置注入

涉及文件：
- `BSP/Inc/gc9a01_port.h`
- `BSP/Src/gc9a01_port.c`
- `BSP/Inc/w25qxx_port.h`
- `BSP/Src/w25qxx_port.c`

#### 关键改动：
1. **GC9A01 配置注入结构体与接口**：
   ```c
   typedef struct {
       spi_bus_t       *bus;          /* 目标总线，NULL 默认 SPI1 */
       uint32_t         spi_clock_hz; /* 时钟速率，默认 18MHz */
       spi_bus_mode_t   spi_mode;     /* 模式，默认 Mode 0 */
       uint8_t          use_dma;      /* 是否启用 DMA */
       GPIO_TypeDef    *cs_port;      /* 片选 GPIO */
       uint32_t         cs_pin;
       GPIO_TypeDef    *rst_port;     /* 复位 GPIO */
       uint32_t         rst_pin;
       GPIO_TypeDef    *dc_port;      /* DC GPIO */
       uint32_t         dc_pin;
   } GC9A01_PortConfig;

   LCD_Result GC9A01_PortInitWithConfig(LCD_Device *dev, const GC9A01_PortConfig *config);
   LCD_Result GC9A01_PortInit(void); /* 兼容原入口 */
   ```
2. **W25QXX 配置注入结构体与接口**：
   ```c
   typedef struct {
       spi_bus_t       *bus;          /* 目标总线，NULL 默认 SPI1 */
       uint32_t         spi_clock_hz; /* 时钟速率，默认 9MHz */
       spi_bus_mode_t   spi_mode;     /* 模式，默认 Mode 0 */
       uint8_t          use_dma;      /* 是否启用 DMA */
       GPIO_TypeDef    *cs_port;      /* 片选 GPIO */
       uint32_t         cs_pin;
   } W25QXX_PortConfig;

   W25QXX_Result W25QXX_PortInitWithConfig(W25QXX_Device *dev, const W25QXX_PortConfig *config);
   void          W25QXX_PortInit(void); /* 兼容原入口 */
   ```
3. **上下文持久化与生命周期保障**：
   端口层内部维护静态槽位池（`s_lcd_hw[GC9A01_MAX_INSTANCES]` 与 `s_flash_contexts[W25QXX_MAX_INSTANCES]`），将传入的配置值完整拷贝入持久化槽位中。即使调用方传入局部栈变量，也不会发生指针失效或内存悬空缺陷。
4. **动态引脚与总线解耦**：
   底层回调函数（`Port_Cs`、`Port_Rst`、`Port_Dc`、`Port_Write` 及 Flash 对应回调）彻底移除对宏常量的依赖，全部通过 `(GC9A01_HW *)ctx` 或 `(W25QXX_PortContext *)ctx` 动态获取目标总线和 GPIO 端口。

---

### 2.3 Task 3：Host 多实例验证

涉及文件：
- `tests/host/test_spi_multi_instance.c`
- `tests/host/CMakeLists.txt`

针对多总线场景构建独立 Mock 硬件（`hw_a` 与 `hw_b`），实现 8 项专项测试用例：
- **M01**：创建 Bus A 和 Bus B，验证初始状态机互相独立，指针与上下文完全隔离。
- **M02**：两条总线分别配置 18MHz/Mode0 与 9MHz/Mode3，断言参数互不覆盖。
- **M03**：Bus A 被 Dev A 占用期间，Dev B 在 Bus B 上发起并发传输，断言 Bus B 成功传输且 Bus A 计数为 0。
- **M04**：两个设备挂在同一条 Bus A 上，断言设备互斥仲裁有效（Dev 2 抢占被拒，Dev 1 释放后 Dev 2 成功执行）。
- **M05**：同一个设备对象动态从 Bus A 重新绑定至 Bus B，驱动收发逻辑无需修改。
- **M06**：Bus A 注入传输超时故障（返回 -2），验证 Bus B 上的通信依然完全正常，错误状态不串扰。
- **M07**：Bus A 执行异常恢复，断言 Bus A 恢复计数增加，而 Bus B 恢复计数严格保持为 0。
- **M08**：验证跨总线的多次嵌套占用与释放（Depth 1 -> 2 -> 3 -> 0），断言总线最终完全交还，无内存泄漏与状态残留。

---

## 三、构建与测试结果

### 3.1 Host 单元测试结果（5/5 CTest PASS）

运行测试：
```bash
cmake -S tests/host -B tests/host/build && cmake --build tests/host/build && ctest --test-dir tests/host/build --output-on-failure
```

测试汇总：
```
Test project /home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/tests/host/build
    Start 1: bus
1/5 Test #1: bus ..............................   Passed    0.00 sec
    Start 2: ll
2/5 Test #2: ll ...............................   Passed    0.00 sec
    Start 3: flash
3/5 Test #3: flash ............................   Passed    0.02 sec
    Start 4: gc9a01
4/5 Test #4: gc9a01 ...........................   Passed    0.00 sec
    Start 5: spi_multi_instance
5/5 Test #5: spi_multi_instance ...............   Passed    0.00 sec

100% tests passed, 0 tests failed out of 5
Total Test time (real) = 0.03 sec
```

`test_spi_multi_instance` 断言详情：
- **断言总数**：91 项断言
- **失败数量**：0 项失败
- **测试结果**：M01 ~ M08 全量通过。

### 3.2 AddressSanitizer / UndefinedBehaviorSanitizer 结果

在 `-fsanitize=address,undefined -g` 开启下编译运行 Host 测试：
- 5 个测试套件全部通过（100% Passed），未检出任何内存泄漏、非法越界或未定义行为。

### 3.3 固件交叉编译资源统计

| 构建配置 | 编译器 / 构建工具 | Flash 使用量 | SRAM 使用量 | Flash 占比 | SRAM 占比 | 编译告警 |
|---|---|---|---|---|---|:---:|
| **Debug** | GCC 10.3 / Ninja | 47,588 B | 7,120 B | 72.61% (64 KB) | 34.77% (20 KB) | 0 warnings |
| **Release** | GCC 10.3 / Make | 32,784 B | 7,112 B | 50.02% (64 KB) | 34.73% (20 KB) | 0 warnings |

---

## 四、硬件验收状态明确区分（按规范声明）

依照任务规范第六节要求，本报告对各环境验收状态进行严格的分类说明与区分：

| 验收分类 | 验收结果 | 说明与依据 |
|---|:---:|---|
| **Host 多实例验证** | **PASS** | M01 ~ M08 全部通过，91/91 项断言无一失败。 |
| **Host 全量回归** | **PASS** | 5 大 CTest 套件通过，ASan/UBSan 无内存隐患。 |
| **SPI1 真实硬件回归** | **NOT TESTED** | 尝试通过 OpenOCD 连接 ST-Link 时报错 `unable to connect to the target`（目标板 SWD 信号线物理接触不良/松动）。遵照“严禁虚构实测数据”规范，真实标记为 NOT TESTED，不借用上轮数据冒充。 |
| **SPI2 真实硬件运行** | **NOT TESTED** | 在现有硬件上 PB13 已物理连接至 GC9A01_DC 引脚；在未重新飞线前严禁向实机烧录启用 SPI2（SCK 亦为 PB13）的固件，以防硬件短路冲突。 |
| **其他 32 位 MCU 移植** | **NOT TESTED** | 本阶段重点建立可移植框架与 Host 仿真，未在其他物理 MCU 评估板上运行。 |

---

## 五、交付物索引

1. **整改报告**：[docs/stage2_2a_report.md](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/docs/stage2_2a_report.md)
2. **阶段验收单**：[docs/stage2_2a_acceptance.md](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/docs/stage2_2a_acceptance.md)
3. **移植开发指南**：[docs/spi_porting_guide.md](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/docs/spi_porting_guide.md)
4. **多实例单元测试源码**：[tests/host/test_spi_multi_instance.c](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/tests/host/test_spi_multi_instance.c)
5. **板级总线实现**：[BSP/Src/bsp_spi.c](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/BSP/Src/bsp_spi.c)、[BSP/Inc/bsp_spi.h](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/BSP/Inc/bsp_spi.h)
6. **屏幕移植适配实现**：[BSP/Src/gc9a01_port.c](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/BSP/Src/gc9a01_port.c)、[BSP/Inc/gc9a01_port.h](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/BSP/Inc/gc9a01_port.h)
7. **Flash 移植适配实现**：[BSP/Src/w25qxx_port.c](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/BSP/Src/w25qxx_port.c)、[BSP/Inc/w25qxx_port.h](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/BSP/Inc/w25qxx_port.h)
