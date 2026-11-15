# 使用指南（STM32F103C8T6 + GC9A01 + W25QXX）

裸机工程：STM32F103C8T6 用同一个 SPI1 同时驱动 240×240 圆屏 GC9A01 和 W25Q128 NOR Flash，两个设备都走 DMA。上电后依次执行屏幕自测、双设备 DMA 读写测试、总线并发测试，结果显示在屏幕上。

## 1. 芯片与系统配置

| 项目 | 配置 |
|---|---|
| MCU | STM32F103C8T6，Cortex-M3，LQFP48 |
| 存储 | 64 KB Flash / 20 KB SRAM |
| 主频 | HSE 8 MHz → PLL ×9 = 72 MHz |
| 总线 | AHB 72 MHz / APB1 36 MHz / APB2 72 MHz |
| Flash 等待 | 2 WS（`LL_FLASH_LATENCY_2`） |
| 库 | STM32Cube LL（FW_F1 V1.8.7），无 HAL 外设初始化 |
| 构建 | CMake + Ninja + arm-none-eabi-gcc 14.2.1 |
| 调试 | SWD（ST-LINK/V2）+ OpenOCD 0.12.0 |
| 供电 | 3.3 V（屏幕与 Flash 均为 3.3 V 器件） |

**CubeMX 只负责时钟与基础 GPIO**。SPI1、DMA1、片选/复位/DC 引脚全部由代码初始化（`bsp_spi.c`、`gc9a01_port.c`、`w25qxx_port.c`），不要在 CubeMX 里再使能 SPI1 或 DMA，否则两处配置会互相覆盖。

## 2. 引脚资源

| 功能 | 外设 | 引脚 | 配置 |
|---|---|---|---|
| SPI 时钟 | SPI1_SCK | PA5 | 复用推挽 |
| SPI 主出从入 | SPI1_MOSI | PA7 | 复用推挽 |
| SPI 主入从出 | SPI1_MISO | PA6 | 浮空输入 |
| 屏幕片选 | GPIO 输出 | PA9 | 推挽，低有效 |
| 屏幕复位 | GPIO 输出 | PA10 | 推挽，低有效 |
| 屏幕命令/数据 | GPIO 输出 | PB13 | 推挽（0=命令，1=数据） |
| Flash 片选 | GPIO 输出 | PA8 | 推挽，低有效 |
| 按键 1 / 2 | GPIO 输入 | PB12 / PB14 | 内部上拉，按下为低（模块预留，当前主流程未调用） |
| 运行灯 | GPIO 输出 | PC13 | 推挽，低电平点亮 |
| 调试口 | SWD | PA13 / PA14 | 运行时保留 SW-DP |
| DMA | DMA1 | Ch3=SPI1_TX，Ch2=SPI1_RX | 最高优先级，单次模式 |

总线速率按设备切换：屏幕 18 MHz（72÷4），Flash 9 MHz（72÷8），都在 `spi_bus_acquire()` 时生效。

## 3. 接线

### 3.1 GC9A01（SPI 圆屏）

| 屏幕引脚 | 接到 | 说明 |
|---|---|---|
| VCC | 3.3 V | 必须 3.3 V；就近放 0.1 µF 去耦 |
| GND | GND | 与 MCU、ST-LINK 共地 |
| SCL / CLK | PA5 | SPI1_SCK |
| SDA / MOSI | PA7 | STM32 发 → 屏幕收 |
| RES / RST | PA10 | 复位，低有效 |
| DC | PB13 | 0=命令，1=数据 |
| CS | PA9 | 片选，低有效 |
| BLK / BL | 3.3 V | 背光常亮（工程未接控制脚） |

注意：不同厂家模块的丝印可能是 `SDA` 而非 `MOSI`，屏幕只写不读，**不需要接 MISO**。

### 3.2 W25Q128（SPI NOR Flash）

| Flash 引脚 | 接到 | 说明 |
|---|---|---|
| VCC（8） | 3.3 V | 就近 0.1 µF 去耦 |
| GND（4） | GND | 共地 |
| CS#（1） | PA8 | 低有效，建议 10 kΩ 上拉 |
| CLK（6） | PA5 | 与屏幕共用 |
| DI / MOSI（5） | PA7 | 与屏幕共用 |
| DO / MISO（2） | PA6 | STM32 收 |
| WP#（3） | 3.3 V | 标准 SPI 模式必须为高，10 kΩ 上拉 |
| HOLD#（7） | 3.3 V | 同上 |

- 6 针模块通常已在板上处理 WP#/HOLD#；裸芯片或 8 针模块必须把这两个脚拉高，否则写入失败或 ID 读错。
- 杜邦线尽量短（建议 < 10 cm），18 MHz 下长线容易花屏或读 ID 出错。

### 3.3 调试口与启动

| 项目 | 接线 |
|---|---|
| ST-LINK | SWDIO→PA13，SWCLK→PA14，GND→GND，3.3 V 参考→3.3 V |
| 启动 | BOOT0 = 0（从 Flash 启动） |

## 4. 编译与烧录

```bash
# 编译（Debug）
cmake --preset Debug
cmake --build build/Debug

# 烧录 + 校验
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "program build/Debug/gc9a01.elf verify reset exit"

# 生成 hex（可选）
arm-none-eabi-objcopy -O ihex build/Debug/gc9a01.elf build/Debug/gc9a01.hex
```

在线调试：

```bash
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg     # 终端 1：GDB Server，端口 3333
arm-none-eabi-gdb build/Debug/gc9a01.elf -ex "target remote :3333"
```

产物：`build/Debug/gc9a01.elf`。资源占用约 Flash 36.5 KB（55.7%）、SRAM 3.7 KB（18.0%）。

> `main.c` 中把 CubeMX 默认的 `LL_GPIO_AF_DisableRemap_SWJ()`（同时关闭 JTAG-DP 与 SW-DP）改成了 `LL_GPIO_AF_Remap_SWJ_NOJTAG()`。重新用 CubeMX 生成代码时这一行会被覆盖，务必改回，否则程序一运行 SWD 就断开。

## 5. 上电现象

上电后约 2~3 秒内依次出现：

1. **屏幕自测**：红 → 绿 → 蓝全屏（各 300 ms）→ 顶部 6 条色带 → 文字 `GC9A01 OK` / `240x240 SPI1` / 引脚信息 → 白框 + 品红圆
2. **双 DMA 测试**：`TFT+FLASH DMA TEST`，显示 `ID:EF17 CAP:16MB`、`256B x3 R:3/3`、`PASS`，并画出 3 个绿色方块
3. **总线可靠性测试**：`BUS RELIABILITY TEST`，正常结果：

```
CT:18/18 G:0 E:00000
IRQ:xxxxx HIT:xxxxx
LEN:10/10  STR:1000/1000
FL:10/10 EC:11
REC:OK
OWNER:NULL DEPTH:0
PASS 1012ms
```

4. 之后进入主循环：屏幕右下角 24×12 方块每 500 ms 红/黑翻转（心跳）

判读要点：

| 字段 | 正常 | 异常含义 |
|---|---|---|
| `CT` | 18/18 | 少一项看 `E` 位图：bit0~bit19 对应契约检查项 |
| `G` | 0 | 非 0 表示中断成功抢到了总线（ISR 保护失效） |
| `HIT` | 接近 `IRQ` | 记录高优先级探针中断落在主循环事务执行正中间的次数 |
| `LEN` | 10/10 | 1~480 字节传输长度矩阵逐字节比对结果 |
| `STR` | 1000/1000 | 1000 次连续高频 SPI 事务压力测试通过次数 |
| `FL` | 10/10 | Flash 扇区擦除 + 页编程 + 读回校验轮数；`EC` 为累计擦除次数 |
| `REC` | OK | 统一异常恢复与热重启后再次通信校验结果 |
| `OWNER` | NULL | `LEFT` 表示有设备未释放总线 |
| `DEPTH` | 0 | 非 0 表示嵌套释放与申请不平衡 |

## 6. 在自己的代码里使用

```c
#include "bsp_includes.h"

/* 初始化（main 中已调用，顺序不能反） */
Timebase_Init();        /* SysTick 1ms + DWT 微秒 */
GC9A01_PortInit();      /* 屏幕 */
W25QXX_PortInit();      /* Flash：绑定同一 SPI1 并读 ID */

/* 屏幕 */
LCD_Fill(&g_lcd, 0, 0, 240, 240, BLACK);
LCD_ShowString(&g_lcd, 20, 20, "HELLO", WHITE, BLACK, 16, 0);

/* Flash（地址可落在扇区内任意位置） */
uint8_t buf[32];
W25QXX_Read(&g_flash, buf, 0x001000, sizeof(buf));
W25QXX_Write(&g_flash, buf, 0x001000, sizeof(buf));   /* 会先擦除所在 4 KB 扇区 */
W25QXX_EraseSector(&g_flash, 0x001000);
```

超时时间：页写 100 ms、扇区擦除 3 s、全片擦除 200 s（`w25qxx.h` 顶部可改）。

**中断里不能直接用 SPI**：`spi_bus_acquire()` 在 ISR 中一律返回 `-1`。确实需要在中断里发数据时，让中断置标志/入队，由主循环执行传输。

## 7. 常见问题

| 现象 | 排查 |
|---|---|
| 屏幕全白或全黑，无自测画面 | 3.3 V 供电、共地、CS=PA9、DC=PB13、RST=PA10 是否接对；BLK 是否接 3.3 V |
| 花屏、颜色错乱、偶发闪白 | SPI 线过长或速率过高；先降 `GC9A01_SPI_CLOCK_HZ` 到 9 MHz 验证 |
| 屏幕正常但 `ID:0000` / `FFFF` | Flash 供电、CS=PA8、MISO=PA6、WP#/HOLD# 是否拉高 |
| ID 错位（如 `DE2F`） | 采样沿/线序问题：确认 MISO 接 DO、模式 0、线尽量短 |
| 能读 ID 但写入失败 | WP#/HOLD# 未拉高，或扇区未擦除干净 |
| `W/R` 少于 10/10 | Flash 写入/回读不一致，多为接线或供电；`E` 位图同时非 0 则先查总线 |
| `G` 不为 0 / `OWNER:LEFT` | 有代码在 ISR 抢总线或漏了 `spi_bus_release()`；检查新增设备的事务配对 |
| 屏幕与 Flash 交替访问后死机 | 新增设备未走 `spi_bus_acquire/release`，直接操作了 SPI1 |
| 烧录后 SWD 连不上 | 先给 **MCU 断电重上电**再连；若刚用 CubeMX 重新生成过代码，确认 `LL_GPIO_AF_Remap_SWJ_NOJTAG()` 仍在 |
| 改引脚不生效 | 引脚宏在 `bsp_spi.c` / `gc9a01_port.c` / `w25qxx_port.c` 顶部，改完必须重新编译烧录 |
