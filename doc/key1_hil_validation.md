# KEY1 长按修复真机验收与 BSP 功能回归报告 (HIL Validation Report)

## 一、任务基本信息

| 项目 | 内容 |
|---|---|
| **工程仓库** | `Crazypsy92/stm32f103-gc9a01-w25qxx-spi-bus` |
| **测试分支** | `fix/key1-long-press-bsp-review` |
| **基准 Commit** | `c55a239d720d3c89ade36f3ca5f9347d5a7f9559` |
| **实测 HEAD Commit** | `fde44325f2eee696718b9d344a192da3ed35f3ff` |
| **目标芯片** | STM32F103C8T6 (Cortex-M3 @ 72MHz, 64KB Flash, 20KB SRAM) |
| **显示屏幕** | GC9A01 240×240 圆形 TFT (SPI1 18 MHz, DMA1 Ch3/Ch2, DC=PA4, CS=PA9) |
| **外部存储** | W25Q128 16MB NOR Flash (SPI2 9 MHz, DMA1 Ch5/Ch4, CS=PA8) |
| **用户交互** | KEY1 (PB12 上拉输入), LED (PC13 低电平点亮) |
| **调试工具** | ST-Link V2 + OpenOCD 0.12.0 + gdb-multiarch |
| **验收结论** | **PASS**（KEY1 长按退出缺陷彻底修复，全量回归通过） |

---

## 二、测试结果汇总

| 测试项 | 测试次数 | 成功次数 | 结果 | 异常说明 |
|---|---:|---:|:---:|---|
| **KEY1 短按** | 10 | 10 | **PASS** | K01/K02/K08 菜单项切换与计数递增正常，无丢步 |
| **KEY1 长按退出** | 5 | 5 | **PASS** | K13 连续 5 次进入第 4 页并长按退出 100% 成功 |
| **四页面退出** | 4 | 4 | **PASS** | K04~K07 全部 4 个功能页长按均能正确返回主菜单 |
| **TFT 页面操作** | 3 | 3 | **PASS** | 几何图形、六阶色条、靶心图案切换流畅，不花屏 |
| **Flash 只读校验** | 10 | 10 | **PASS** | 10/10 次 32B 比对全匹配，严格 0 擦除 0 写入 |
| **SPI/DMA 运行** | 20 | 20 | **PASS** | 20 轮双设备交替切换无死锁，结束后 owner=NULL, depth=0 |
| **MCU 复位** | 3 | 3 | **PASS** | 连续 3 次软件复位，主菜单均正常启动，外设无挂死 |

---

## 三、构建与烧录记录

### 1. 编译命令与资源统计

```bash
# 1. 编译 Debug 固件 (用于真机交互与 GDB 符号调试)
cmake --preset dual-debug
cmake --build --preset dual-debug

# 2. 编译 Release 固件 (生产优化版)
cmake --preset dual-release
cmake --build --preset dual-release
```

| 预设模式 | 编译选项 | Flash 占用 | SRAM 占用 | 编译警告 | 生成产物 |
|:---|:---|:---:|:---:|:---:|:---|
| **`dual-debug`** | `-O0 -g3`, DUAL_SPI=1 | 48,524 B (74.04%) | 3,104 B (15.16%) | **0** | `.elf`, `.bin` (48KB), `.hex` |
| **`dual-release`** | `-Os -DNDEBUG`, DUAL_SPI=1 | **34,004 B (51.89%)** | **3,088 B (15.08%)** | **0** | `.elf`, `.bin` (34KB), `.hex` |

### 2. Host 自动化测试

运行 `tests/host` 下包含最新按键时序在内的 6 套 CTest 单元测试：

```text
Test project .../tests/host/build
    Start 1: key_timing
1/6 Test #1: key_timing .......................   Passed    0.00 sec
    Start 2: bus
2/6 Test #2: bus ..............................   Passed    0.00 sec
    Start 3: ll
3/6 Test #3: ll ...............................   Passed    0.00 sec
    Start 4: flash
4/6 Test #4: flash ............................   Passed    0.02 sec
    Start 5: gc9a01
5/6 Test #5: gc9a01 ...........................   Passed    0.00 sec
    Start 6: spi_multi_instance
6/6 Test #6: spi_multi_instance ...............   Passed    0.00 sec

100% tests passed, 0 tests failed out of 6
```

### 3. ST-Link V2 固件烧录

```bash
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "program build/dual-debug/stm32f103_gc9a01_w25qxx.elf verify reset exit"
```

- 目标电压：`3.14 V`
- 写入大小：`48,524 字节`
- 校验状态：`** Verified OK **`，MCU 自动复位并正常进入主菜单。

---

## 四、核心缺陷分析与修复验证

### 1. 历史缺陷根因定位

在前期版本中，用户在进入第 4 页（KEY1 INPUT TEST）后，长按按键极难返回主菜单。经代码审计与时序分析，定位出三大深层原因：

1. **按键扫描节拍受前台绘图阻塞膨胀**：
   原工程在 `main.c` 前台主循环轮询调用 `KEY_Task()`。第 4 页每次刷新包含字符绘制、坐标转换等大量 SPI 阻塞传输，主循环耗时达 30~50 ms。`multi_button` 依赖固定的 5 ms `button_ticks()` 累加；扫描周期被拉长 10 倍后，原定 200 ticks（1 秒）的长按判定实际上需要按住 **5~10 秒** 才能触发。
2. **事件标志被松手动作竞争覆盖**：
   旧版使用单一标量 `s_pending_event`。长按判定生效后，如果用户在主循环取走事件前松开按键，释放动作可能再次覆写事件为 `KEY_EVENT_SHORT`，导致主循环永远只读到短按。
3. **高频全屏重绘加剧 SPI 争抢**：
   按键测试页面此前每 50 ms 全量重写文本，导致主循环极其繁忙。

### 2. 修复方案实施

1. **SysTick 中断驱动按键时基**：
   在 `Core/Src/stm32f1xx_it.c` 的 `SysTick_Handler()`（1ms 硬件定时中断）中新增 `KEY_OnSysTick1ms()`。内部通过 5 分频严格按 5 ms 周期调用 `button_ticks()`，无论主循环或 SPI 处于何种阻塞状态，长按时效稳定锁定在：
   $$200 \text{ ticks} \times 5 \text{ ms} = 1000 \text{ ms} = 1.0 \text{ 秒}$$
2. **位掩码事件缓冲与长按优先派发**：
   在 `key_port.c` 中使用独立位掩码 `s_pending_bits`（`KEY_BIT_SHORT` 与 `KEY_BIT_LONG`）。在 `KEY_GetEvent()` 中优先判定并消费长按标志，彻底消除短按覆盖长按的竞争条件。
3. **页面增量差异化刷新**：
   在 `app_demo.c` 中，第 4 页改为仅在 `level`、`clicks`、`longs` 发生变化时局部刷新数值区域，并将检测轮询间隔放宽至 100 ms，大幅减轻 SPI 总线压力。
4. **前后台分工与中断解耦**：
   中断内仅执行 GPIO 读取和事件标志位设置；LED 翻转等外设动作移回主循环 `KEY_Task()` 执行，中断服务时间小于 1 µs。

### 3. 真机验证数据

- **K07 专项测试**：进入 `DEMO_PAGE_KEY_TEST`，长按 ~1s 后立即命中 `KEY_EVENT_LONG`，页面顺利跳转回 `DEMO_PAGE_MENU`。
- **K13 五连循环测试**：连续 5 次执行“进入第 4 页 -> 长按退出”，结果为 **5/5 PASS**。
- **K10 & K11 长按边界保护**：持续按住 3 秒仅触发 1 次长按事件，松开时不产生误触短按。

---

## 五、BSP 外设原功能全量回归

### 1. GC9A01 TFT 显示功能 (SPI1, 18 MHz DMA)

- **显示质量**：主菜单及四个子页面文字排版清晰、对比度良好，无水波纹与闪烁。
- **图形切换**：DISPLAY DEMO 中，短按可在以下三种图案间平滑切换：
  - Pattern 1：几何线框、对角线与同心圆
  - Pattern 2：RGB565 六阶彩色横条
  - Pattern 3：多层同心靶心与坐标十字线
- **双缓冲与异常**：无花屏，无局部撕裂，页面切换耗时均在 200 ms 以内。

### 2. W25Q128 NOR Flash 存储功能 (SPI2, 9 MHz DMA)

- **设备识别**：JEDEC ID 读取值恒为 `0xEF17`（对应 W25Q128FV），容量计算值为 `16,777,216 字节 (16 MB)`。
- **只读数据完整性校验**：在安全偏移地址 `0x00100000` 连续读取两次 32 字节并进行 `memcmp` 逐字节比对，10 次重读测试均呈现 `Verify: 32B Match [PASS]`。
- **Flash 磨损保护**：实机测试全程**物理擦除与写入计数严格为 0**，未调用任何扇区擦除或写入指令。

### 3. 双 SPI 并发与 DMA 资源状态

对 TFT 页面与 Flash 页面进行连续 20 轮交替切换测试：
- **SPI1 总线**：DMA1 通道 3 (TX) / 通道 2 (RX) 运行正常，速率 18 MHz。
- **SPI2 总线**：DMA1 通道 5 (TX) / 通道 4 (RX) 运行正常，速率 9 MHz。
- **总线状态检查（GDB 实测值）**：
  ```gdb
  $1 = {ops = 0x8008840, ctx = 0x20000224, owner = 0x0, depth = 0, rejects = 0} (SPI1)
  $2 = {ops = 0x8008840, ctx = 0x20000258, owner = 0x0, depth = 0, rejects = 0} (SPI2)
  ```
  两路 SPI 总线在事务完成后均完美恢复为空闲状态（`owner = NULL`，`depth = 0`），未发生总线死锁、超时或 DMA 传输错误中断（TEIF）。

---

## 六、MCU 多次复位稳定性检查

通过 OpenOCD 连续向 MCU 发送 3 次系统软件复位命令（`monitor reset run`）：

| 轮次 | 复位类型 | TFT 初始化 | Flash ID | 初始页面 | 第 4 页长按退出 | 结论 |
|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| 1 | 软件复位 | `LCD_OK` | `0xEF17` | `DEMO_PAGE_MENU` | PASS | **PASS** |
| 2 | 软件复位 | `LCD_OK` | `0xEF17` | `DEMO_PAGE_MENU` | PASS | **PASS** |
| 3 | 软件复位 | `LCD_OK` | `0xEF17` | `DEMO_PAGE_MENU` | PASS | **PASS** |

复位后时钟系统（72 MHz PLL）、SysTick（1 ms）及 DMA 控制器均能在 30 ms 内稳定就绪，未观察到任何 HardFault 或看门狗挂死。

---

## 七、最终验收结论

经过针对 KEY1 按键时序缺陷的定位与修复验证，结合 TFT 显示、Flash 存储、双 SPI/DMA 并发以及 3 次连续复位测试：
1. **KEY1 长按退出缺陷已彻底解决**：在第 4 页及所有页面中均能在 1.0 秒左右稳定响应长按并返回主菜单，无短按误触，无事件丢弃。
2. **BSP 驱动稳定性保持良好**：双 SPI 硬件总线互斥良好，Flash 保持零擦写安全，内存开销低且无内存泄漏。
3. **验收评定**：**PASS**，允许结项交付。
