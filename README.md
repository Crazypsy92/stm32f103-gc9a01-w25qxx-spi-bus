# STM32F103C8T6 裸机 GC9A01 + W25QXX 共享 SPI1（DMA + 总线仲裁）

> 接线、构建与排障见 [USAGE.md](USAGE.md)；实测数据与限制见 [TEST_REPORT.md](TEST_REPORT.md)。

240×240 圆屏 GC9A01 与 W25Q128 NOR Flash **共用同一个 SPI1**，两者都启用 DMA，靠一层与 MCU 无关的总线抽象 `spi_bus` 完成速率切换与独占仲裁；高优先级中断试图抢占总线会被直接拒绝。全部结论来自实物测试（SWD 读数 + 屏幕输出 + RAM 数据校验）。

## 1. 硬件

| 项目 | 配置 |
|---|---|
| MCU | STM32F103C8T6，HSE 8 MHz → PLL 72 MHz，64 KB Flash / 20 KB SRAM |
| 库 / 构建 | STM32Cube LL（无 HAL 外设初始化）+ CMake + Ninja + arm-none-eabi-gcc |
| 屏幕 | GC9A01，240×240，RGB565，SPI 模式 0，**18 MHz**（72 MHz ÷ 4），DMA |
| Flash | W25Q128（实测 ID `EF17`，16 MB），SPI 模式 0，**9 MHz**（72 MHz ÷ 8），DMA |
| 共用 SPI | SPI1：SCK=PA5、MISO=PA6、MOSI=PA7（CubeMX 未使能，代码配置） |
| 屏幕控制 | CS=PA9、RST=PA10、DC=PB13（背光常亮，无控制脚） |
| Flash 片选 | CS=PA8 |
| DMA | 仅 DMA1：SPI1_TX→`DMA1_Channel3`，SPI1_RX→`DMA1_Channel2` |
| 按键 / LED | KEY1=PB12、KEY2=PB14、LED=PC13（模块已实现，当前主流程未启用） |
| 调试 | SWD（PA13/PA14），`LL_GPIO_AF_Remap_SWJ_NOJTAG()` 保留 SW-DP |

## 2. 目录结构

```
GC9A01/
├── CMakeLists.txt                 BSP 源文件清单（产物 gc9a01.elf）
├── CMakePresets.json              Debug / Release（Ninja）
├── gc9a01.ioc                     CubeMX 配置（仅时钟/GPIO，SPI 与 DMA 由代码配置）
├── cmake/
│   ├── gcc-arm-none-eabi.cmake    工具链
│   └── stm32cubemx/               CubeMX 生成的源文件/符号清单（含补齐的 LL SPI/DMA）
├── Core/
│   ├── Inc/                       main.h / stm32f1xx_it.h
│   └── Src/                       main.c、stm32f1xx_it.c（SysTick → Timebase）
├── BSP/
│   ├── Inc/
│   │   ├── spi_bus.h              ⭐ SPI 总线抽象接口（MCU 无关，含仲裁与并发约束）
│   │   ├── spi_stm32_ll.h         STM32F1 LL 实现描述（硬件描述结构体）
│   │   ├── bsp_spi.h              板级 SPI1 单例 + TIM2 抢占探针接口
│   │   ├── gc9a01.h / gc9a01_port.h       屏幕协议层 / 移植层
│   │   ├── w25qxx.h / w25qxx_port.h       Flash 协议层 / 移植层
│   │   ├── app_test.h             上电自测与总线并发测试
│   │   ├── delay.h                SysTick 1ms + DWT 微秒时基
│   │   ├── multi_button.h / key_port.h    按键状态机（预留）
│   │   ├── lcdfont.h / image.h    字库 / 图片资源
│   │   └── bsp_includes.h         BSP 统一入口
│   └── Src/                       对应实现
├── Drivers/                       STM32F1xx LL + CMSIS
└── STM32F103xx_FLASH.ld           链接脚本
```

## 3. 分层与数据流

```
app_test.c / main.c                     应用与测试
      │  LCD_Fill / LCD_ShowString / W25QXX_Read / W25QXX_Write
      ▼
gc9a01.c / w25qxx.c                     纯协议层（不含任何 MCU 头文件）
      │  LCD_Port 回调表            W25QXX_Bus 回调表
      │  cs/rst/dc/delay/write      cs_low/cs_high/swap/millis
      │                             + 可选 read_bulk/write_bulk
      ▼
gc9a01_port.c / w25qxx_port.c           移植层：引脚、速率、acquire/release
      │  spi_bus_acquire → 传输 → spi_bus_release
      ▼
spi_bus.h                               抽象总线（owner/depth/rejects 仲裁）
      │  configure / transfer / in_isr
      ▼
spi_stm32_ll.c                          唯一的 SPI/DMA 时序实现（LL）
      ▲
bsp_spi.c                               SPI1 单例（BSP_SPI1_GetBus）+ TIM2 抢占探针
```

设备协议层完全不知道 MCU 的存在：换平台时只替换 `spi_stm32_ll.c`（新平台的 SPI 实现），`gc9a01.c`、`w25qxx.c`、`spi_bus.h`、`app_test.c` 一行都不用改。

## 4. 总线接口（`spi_bus.h`）

| 接口 / 字段 | 说明 |
|---|---|
| `spi_bus_config_t` | `clock_hz`（目标 SCLK）+ `mode`（0~3）+ `use_dma` |
| `spi_bus_acquire(bus, owner, cfg)` | 占用总线并切到本设备参数；被他人占用或在 ISR 中调用返回 `-1` |
| `spi_bus_release(bus, owner)` | 释放；仅占用者本人可调用，支持同 owner 嵌套计数 |
| `spi_bus_owner(bus)` | 查询当前占用者，空闲返回 `NULL` |
| `spi_bus_transfer(bus, tx, rx, len)` | 全双工传输；未 acquire 直接返回 `-1`；`rx` 可为 `NULL` |
| `bus->owner / depth / rejects` | 占用者 / 重入深度 / 被拒绝次数（调试与测试用） |
| `ops->in_isr(bus)` | 可选：实现方上报是否处于中断上下文 |

关键设计：

- **owner 用设备上下文指针本身充当身份**（`&s_hw` / `&s_ctx`），不需要额外的 ID 分配，也不可能重复。
- **同一 owner 可重入**（`depth++`），只有深度归零才真正释放，方便在一个事务里连续调用多个绘图函数。
- **`rejects` 只增不减**，测试里用它判断"是否有抢占被挡住"。

## 5. 并发模型（使用前必须理解）

1. **独占式**：`acquire → transfer… → release` 期间总线只属于一个设备。`transfer()` 是阻塞的（DMA 也等 TC/BSY 才返回），因此任何时刻只有一条 SPI 事务在跑，不存在两个设备交叉改写同一外设。
2. **中断安全**：ISR 里调用 `acquire()` 会被直接拒绝（`spi_stm32_ll.c` 用 `__get_IPSR()` 哨兵）。原因是中断若插进主循环事务中间，会改掉当前速率/模式并重配 DMA 通道。若中断确实要访问 SPI，应把请求排进队列，由主循环统一执行。
3. **异步 DMA 尚未支持**：当前 `release` 必须在 `transfer()` 返回后立即执行。要改成非阻塞，需把 `release` 挪到"传输完成"回调里，并给 `acquire` 增加忙等待/超时语义。

这三条不是"理论上应该"，而是被 `AppBusTest()` 的 10 项契约检查 + 20 kHz 高优先级中断压力测试覆盖，并在实物上验证过（见 [TEST_REPORT.md](TEST_REPORT.md)）。

## 6. DMA 策略

| 规则 | 说明 |
|---|---|
| 阈值 `SPI_DMA_MIN_LEN = 16` | ≥16 字节走 DMA，小块自动回落轮询（单字节启动 DMA 的开销大于传输本身） |
| 通道固定 | SPI1_TX→DMA1_Ch3、SPI1_RX→DMA1_Ch2（C8T6 只有 DMA1） |
| 双缓冲等待 | TX/RX 同时启动，等 TX 完成、RX 完成、再等 SPI 总线空闲（BSY=0）才返回 |
| Flash 读 | 命令后按 64 字节分块，用静态 `0xFF` 哑元缓冲喂 TX，边发边收 |
| Flash 写 | 页写数据段整块交给 `write_bulk`（TX-only DMA） |
| 屏幕 | `gc9a01.c` 的 480 字节行缓冲作为 DMA 源，`LCD_Fill` 按行发送 |
| 冲突规避 | 两个设备争用同一组通道，但事务是阻塞且互斥的，通道不会在传输中被改配 |

## 7. 设备驱动接口

**GC9A01（`gc9a01.h`）**

| 接口 | 说明 |
|---|---|
| `GC9A01_PortInit()` | 初始化总线绑定、CS/RST/DC 并发送屏幕初始化序列（`gc9a01_port.c`） |
| `GC9A01_Init(dev, port)` | 协议层初始化，绑定 `LCD_Port` 回调表 |
| `LCD_Fill / LCD_DrawPoint / LCD_DrawLine / LCD_DrawRectangle / Draw_Circle` | 基本图形 |
| `LCD_ShowChar / LCD_ShowString / LCD_ShowIntNum / LCD_ShowFloatNum1` | 文字（16/24 号字，RGB565 前景/背景） |
| `LCD_ShowPicture` | RGB565 图片块 |
| `g_lcd` | 全局设备句柄（`gc9a01_port.c` 中定义） |

**W25QXX（`w25qxx.h`）**

| 接口 | 说明 |
|---|---|
| `W25QXX_PortInit()` | 配置 CS(PA8)、绑定共享总线、读 ID/容量到 `g_flash` |
| `W25QXX_Init(dev, bus)` | 协议层绑定 `W25QXX_Bus` 回调表并识别型号 |
| `W25QXX_Read / Write / Write_NoCheck` | 读 / 自动擦除+翻页写 / 纯页写 |
| `W25QXX_EraseSector / EraseChip / PowerDown / WakeUp / ReadID / RefreshID` | 擦除、低功耗与 ID 操作 |
| 返回值 | `W25QXX_OK / ERROR_PARAM / ERROR_TIMEOUT / ERROR_BUS`；`dev->last_result` 保留最近一次结果 |

注意：`W25QXX_Write()` 会先擦除覆盖到的扇区，**同扇区其它数据会丢失**；需要保留时自行「读出 → 修改 → 擦除 → 回写」。

## 8. 配置位置

| 位置 | 宏 | 当前值 |
|---|---|---|
| `bsp_spi.c` 顶部 | `BSP_SPI1_*`（外设/时钟/SCK/MISO/MOSI/DMA 通道） | SPI1、72 MHz、PA5/6/7、DMA1 |
| `gc9a01_port.c` 顶部 | `GC9A01_CS/RST/DC_PORT/PIN` | PA9 / PA10 / PB13 |
| `gc9a01_port.c` 顶部 | `GC9A01_SPI_CLOCK_HZ / MODE / USE_DMA` | 18 MHz / 模式 0 / 1 |
| `w25qxx_port.c` 顶部 | `W25QXX_CS_PORT/PIN` | PA8 |
| `w25qxx_port.c` 顶部 | `W25QXX_SPI_CLOCK_HZ / MODE / USE_DMA / BULK_CHUNK` | 9 MHz / 模式 0 / 1 / 64 |
| `gc9a01.h` | `LCD_W / LCD_H / LCD_ROTATION` | 240 / 240 / 180° |
| `delay.h` | `ENABLE_PRINTF_REDIRECT` | 0（关闭 printf 重定向） |
| `bsp_spi.h` | `BSP_SPI1_PreemptProbe_*` | 测试用探针，可整体删除 |

所有构建选项都在源码宏里，**没有 CMake `-D` 后端开关**，改完重新编译即可。

## 9. 编译与烧录

```bash
cmake --preset Debug && cmake --build build/Debug     # 产物 build/Debug/gc9a01.elf

openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "program build/Debug/gc9a01.elf verify reset exit"
```

在线调试：

```bash
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg      # 终端 1
arm-none-eabi-gdb build/Debug/gc9a01.elf -ex "target remote :3333"
```

## 10. 上电流程

`main()` 依次执行：

1. `Timebase_Init()`：SysTick 1 ms 心跳 + DWT 微秒计数
2. `GC9A01_PortInit()` → 屏幕初始化
3. `W25QXX_PortInit()` → 绑定同一 SPI1 并读 ID
4. `AppTFT_Test()`：红/绿/蓝全屏 → 6 色条 → 文字 → 边框与圆（约 1 s）
5. `AppFlash_Test()`：3 轮 256 B 写-读-校验，每轮插入 TFT DMA 色块
6. `AppBusTest()`：10 项接口契约 + 20 kHz 中断抢占压力测试，结果显示在屏幕上
7. 主循环：`AppTFT_Task()` 心跳方块每 500 ms 翻转

屏幕最终画面（正常情况）：

```
BUS CONCURRENCY TEST
CT:10/10 G:0 E:0000        ← 契约全过、ISR 占用次数 0、失败位图 0
IRQ:12500 HIT:9593         ← 中断次数 / 其中撞上总线正忙的次数
W/R:10/10 SUM:xxxx         ← 10 轮 Flash 读写校验全过
OWNER:NULL DEPTH:0
TFT:DMA FLASH:DMA
PASS xxxms
```

## 11. 新增第三个 SPI 设备 / 换平台

新增设备（例如另一颗 SPI 传感器）：

1. 在它的 port 层保存 `spi_bus_t *bus` 和 `spi_bus_config_t cfg`；
2. 事务开始 `spi_bus_acquire(bus, &s_ctx, &cfg)`，结束 `spi_bus_release(bus, &s_ctx)`；
3. 片选、命令/数据切换全部在 port 层完成，协议层只调用回调表。

换 MCU：只需重写 `spi_stm32_ll.c`（或新写一个 `spi_xxx.c` 实现 `spi_bus_ops_t`）并在板级文件里创建单例；`spi_bus.h`、两个设备驱动、`app_test.c` 无需修改。

## 12. 验收状态

代码已在 STM32F103C8T6 + GC9A01 + W25Q128 实物上完成编译、烧录与 SWD 验证：契约 10/10、抢占 `granted=0`、`owner/depth` 归零、CFSR/HFSR 为 0、10 轮读写数据逐字节正确。详细数据与已知限制见 [TEST_REPORT.md](TEST_REPORT.md)。
