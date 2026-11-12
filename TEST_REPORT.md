# 测试报告

**本报告严格区分「编译通过」「逻辑/契约验证」「真实硬件实测」「尚未验证」。
编译通过不代表 SPI/DMA 或 Flash 读写功能正常。**

- 测试对象：STM32F103C8T6 裸机 GC9A01 + W25QXX 共享 SPI1（DMA + 总线仲裁）工程
- 测试时间：2026-11-13
- 工具链：`arm-none-eabi-gcc 14.2.1` / CMake / Ninja / OpenOCD 0.12.0 / ST-LINK/V2（Linux）
- 测试硬件：STM32F103C8T6 开发板 + GC9A01 240×240 圆屏 + W25Q128（实测 ID `0xEF17`，16 MB）
- 接线：SCK=PA5、MISO=PA6、MOSI=PA7；屏幕 CS=PA9、RST=PA10、DC=PB13；Flash CS=PA8
- 验证手段：屏幕输出 + OpenOCD SWD 读 RAM 全局变量 + RAM 静态收发缓冲逐字节比对

---

## 1. 编译与资源占用（PASS）

| 项目 | 命令 | 结果 |
|---|---|---|
| Debug 全量编译 | `cmake --preset Debug && cmake --build build/Debug` | PASS，无 error / warning |
| 烧录与校验 | `openocd ... -c "program build/Debug/gc9a01.elf verify reset exit"` | PASS（`Programming Finished` / `Verified OK`） |

`arm-none-eabi-size build/Debug/gc9a01.elf`：

| 区域 | 占用 | 占比 |
|---|---|---|
| Flash（text+data） | 36468 B / 64 KB | 55.7% |
| SRAM（data+bss） | 3688 B / 20 KB | 18.0% |

---

## 2. 真实硬件实测（PASS）

### 2.1 基础功能

| 测试项 | 方法 | 结果 |
|---|---|---|
| 屏幕初始化与显存写入 | 红/绿/蓝全屏 + 6 色条 + 文字 + 边框圆 | PASS，肉眼确认无花屏/错色 |
| Flash ID 识别 | `W25QXX_Init()` 读 `0x90` 命令 | PASS，`ID:EF17 CAP:16MB` |
| 双设备 DMA（3 轮 256 B） | `AppFlash_Test()` 写-读-memcmp，轮间插入 TFT DMA 色块 | PASS，`R:3/3`，屏幕 `TFT:DMA FLASH:DMA` |
| 主循环心跳 | `AppTFT_Task()` 每 500 ms 翻转方块 | PASS，持续运行无卡死 |

DMA 路径的 SWD 旁证（事务进行中抓取）：

- `DMA1_Channel2`（SPI1_RX）：`CPAR=0x4001300C`（SPI1->DR），`CMAR` 指向读缓冲 `rx+0xC0`，计数 64，连续 4 次即 256 B
- `DMA1_Channel3`（SPI1_TX）：页写时 `CMAR` 指向静态发送缓冲；Flash 读期间发送静态 `0xFF` 哑元
- 传输期间总线 `owner` 指向 W25QXX 上下文、`depth=1`、SCLK=9 MHz；空闲时回到 18 MHz、`owner=NULL`

### 2.2 总线契约与并发（`AppBusTest()`）

10 项接口契约全部覆盖：未占用禁止传输、未占用禁止释放、设备 A 可占用、设备 B 抢占被拒、ISR 抢占被拒、同 owner 可重入、两次 release 才真正释放、多释放一次失败、释放后 owner 为空。

实测（SWD 读取 `build/Debug/gc9a01.elf` 对应符号）：

| 变量 | 地址 | 实测值 | 判定 |
|---|---|---|---|
| `s_ct_fail` | 0x20000454 | `0x0000` | **CT:10/10 全部通过** |
| `s_probe_probes` | 0x20000204 | 12500 | 20 kHz 中断共触发 12500 次 |
| `s_probe_saw_busy` | 0x20000208 | 9593（76.7%） | 大多数中断确实落在主循环事务中间 |
| `s_probe_granted` | 0x2000020C | **0** | ISR 一次都没拿到总线 |
| `s_spi1.owner` | 0x200001DC | `NULL` | 无残留占用 |
| `s_spi1.depth` | 0x200001E0 | 0 | 无残留重入计数 |
| `s_spi1.rejects` | 0x200001E4 | 12502 | = 12500 次 ISR + 契约测试 2 次拒绝 |
| CFSR / HFSR | 0xE000ED28 / 0xE000ED2C | `0` / `0` | 无 HardFault / 总线错误 |

说明：压力测试期间主循环持续做 `W25QXX_Write/Read`（256 B，DMA）与 `LCD_Fill`（DMA）交替，20 kHz 的 TIM2 中断（NVIC 优先级 0，高于 SysTick 15）不断在事务中间请求总线，全部被 `in_isr` 检查拦下。

### 2.3 RAM 数据复核（不依赖屏幕）

读取 `AppBusTest()` 的静态收发缓冲并逐字节比对：

| 缓冲 | 结果 |
|---|---|
| Bus `tx` vs `rx` | 完全一致 |
| Bus `tx` vs 期望 `(i^0x5A)+9`（256 B） | 全匹配 → 第 10 轮（r=9）确实跑完，即 `W/R:10/10` |
| Flash `tx` vs `rx` | 完全一致 |
| Flash `tx` 首字节 | `0x02` → 对应第 3 轮，`R:3/3` |

---

## 3. 本轮定位并修复的问题

| 问题 | 现象 | 根因 | 修复 |
|---|---|---|---|
| 契约测试 9/10 | 屏幕显示 `CT:9/10`，`probes=0` | `BSP_SPI1_PreemptProbe_Init()` 只配 NVIC，未开 TIM2 时钟；而 `Inject()` 在 `Start()` 之前调用，写 `DIER/EGR` 不生效 | 把 TIM2 时钟使能与寄存器复位移入 `Init()`，重新实测 10/10 |
| 单字节命令走 DMA 反而更慢 | 早期实现全部走 DMA 时，读 ID 等小事务耗时明显增加 | DMA 启动/等待开销大于 1~4 字节的轮询传输 | 增加 `SPI_DMA_MIN_LEN=16` 阈值，小块自动回落轮询 |
| Flash 批量传输错误被吞掉 | 总线超时后 `W25QXX_Write` 仍返回 OK | `write_bulk/read_bulk` 只返回 void，错误无法上报 | 回调改为返回 `int`，驱动据返回值置 `last_result=W25QXX_ERROR_BUS` |
| 工程未内置 LL TIM 驱动 | 编译报 `stm32f1xx_ll_tim.h: No such file` | 该工程只补齐了 LL SPI/DMA/I2C 的源文件 | 探针改为直接写 TIM2 寄存器（`PSC/ARR/EGR/SR/DIER/CR1`） |

---

## 4. 尚未验证 / 已知限制

| 项目 | 说明 |
|---|---|
| 异步（非阻塞）DMA | 未支持。当前 `transfer()` 阻塞至 TC/BSY，`release` 必须紧随其后；改成异步需把 `release` 移入完成回调并给 `acquire` 加超时语义 |
| 中断内 SPI 访问 | 一律拒绝（设计如此）。需要时请在 ISR 中入队，由主循环执行 |
| 双任务并发 | 裸机单线程模型下验证；未做 RTOS 多任务抢占测试 |
| 长时压力 | 压力测试约 0.6 s / 12500 次中断，未做小时级 soak |
| SPI 模式 | 只实测模式 0；模式 3 未验证 |
| DMA 控制器 | C8T6 仅 DMA1，通道 2/3 与 SPI1 固定映射；换芯片需改 `bsp_spi.c` 的通道定义 |
| `W25QXX_Write` | 会整扇区擦除后写入，同扇区其它数据丢失 |
| 按键模块 | `multi_button.c` / `key_port.c` 已编译但主流程未调用，按键功能未验证 |
| 屏幕背光 | 无控制引脚（模块常亮），未验证 PWM 调光 |
| 探针代码 | `BSP_SPI1_PreemptProbe_*` 与 `TIM2_IRQHandler` 属测试代码，可整体删除，不影响总线与设备功能 |

---

## 5. 结论

- **已完成并实测通过**：GC9A01 与 W25Q128 共享 SPI1，两者均走 DMA（`DMA1_Ch3/Ch2`）；总线抽象层按设备切换 18 MHz / 9 MHz；10 项接口契约 10/10；20 kHz 高优先级中断 12500 次抢占尝试全部被拒绝（`granted=0`）；事务结束后 `owner/depth` 归零，无 HardFault / 总线错误；10 轮 256 B 读写数据逐字节正确。
- **可直接复用**：`spi_bus.h` 与 `gc9a01.c`、`w25qxx.c` 不含 MCU 相关代码，换平台只需替换 `spi_stm32_ll.c` 与板级单例。
- **遗留项**：见第 4 节，均不影响当前阻塞式单主循环模型下的正常使用。
