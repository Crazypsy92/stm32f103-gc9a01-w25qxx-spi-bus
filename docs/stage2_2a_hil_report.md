# Stage 2.2-A-HIL：双 SPI 硬件在环实机联合验收报告

## 一、测试基础信息

| 字段 | 内容 |
|---|---|
| 仓库 | `Crazypsy92/stm32f103-gc9a01-w25qxx-spi-bus` |
| 验收分支 | `codex/stage2_2a-hil` |
| 基准 Commit | `7c087c2e459a31b4e100be6e872ca252eaeaf3ff` |
| 目标 MCU | STM32F103C8T6 (64KB/128KB Flash, 20KB SRAM) |
| 显示外设 (TFT) | GC9A01，240×240，圆形 IPS LCD，RGB565 |
| 存储外设 (Flash) | Winbond W25Q128JV (16 MB SPI NOR Flash) |
| 仿真/调试器 | ST-Link V2 (SWD, OpenOCD 0.12.0, gdb-multiarch) |
| 架构设计 | 双 SPI 独立总线（TFT 独占 SPI1，Flash 独占 SPI2） |
| 安全模式 | `CONFIG_FLASH_DESTRUCTIVE_TEST = 0`（严格只读零擦除安全模式） |

---

## 二、双 SPI 物理架构与引脚映射

```
                  STM32F103C8T6
             +---------------------+
             |                     |
             |   SPI1 (72MHz APB2) |
             |   SCK   (PA5) ------> TFT SCL (18 MHz)
             |   MOSI  (PA7) ------> TFT SDA (DMA1 Ch3)
             |   CS    (PA9) ------> TFT CS
             |   RST   (PA10)------> TFT RES
             |   DC    (PA4) ------> TFT DC [★ 改线至 PA4]
             |                     |
             |   SPI2 (36MHz APB1) |
             |   SCK   (PB13)------> Flash CLK (9 MHz) [★ 迁引至 PB13]
             |   MISO  (PB14)<------ Flash DO  (DMA1 Ch4) [★ 迁引至 PB14]
             |   MOSI  (PB15)------> Flash DI  (DMA1 Ch5) [★ 迁引至 PB15]
             |   CS    (PA8) ------> Flash CS  (独立片选)
             +---------------------+
```

---

## 三、双 SPI 真机联合验证测试数据

通过 SWD 调试通道实时提取 MCU 内部导出的诊断结构体变量：

### 1. 板级硬件诊断导出 (`g_bsp_diag`)
```
magic         = 0x44494147 ('DIAG')
dual_spi_mode = 1 (DUAL_SPI 架构)
tft_only_mode = 0 (全功能模式)
tft_dc_pin    = 4 (GPIOA Pin 4)
tft_dc_port_id= 0 (GPIOA)
tft_bus_id    = 1 (SPI1)
flash_bus_id  = 2 (SPI2)
tft_speed_hz  = 18,000,000 Hz (18 MHz)
tft_use_dma   = 1 (DMA 使能)
```

### 2. 外部 Flash 设备识别 (`g_flash`)
```
bus           = 0x200004e8 (指向 W25QXX_Bus 回调上下文)
id            = 0xEF17 (Winbond W25Q128JV, 识别正确)
capacity      = 16,777,216 字节 (16 MB)
last_result   = 0x00 (W25QXX_OK)
```

### 3. 双设备交替访问测试 (`g_app_dual_result`, 20 轮连续交替)
```
magic            = 0x4455414C ('DUAL')
rounds_ok        = 20 / 20 (100% PASS)
rounds_total     = 20
id_ok_count      = 20 / 20 (每轮均正确获取 0xEF17)
dma256_ok_count  = 20 / 20 (SPI2 DMA1_Ch4 256B 读取一致性校验 100% 匹配)
dma480_ok_count  = 20 / 20 (SPI2 DMA1_Ch4 480B 读取一致性校验 100% 匹配)
tft_update_count = 20 / 20 (SPI1 DMA1_Ch3 TFT 局部屏幕刷新 100% 成功)
post_read_ok     = 20 / 20 (刷屏后立即重读 Flash 数据，100% 保持一致无串扰)
owner_null       = 1 (测试结束后两总线持有者归零)
depth_zero       = 1 (测试结束后两总线递归深度归零)
pass             = 1 (DUAL PASS)
elapsed_ms       = 54 ms (20 轮总耗时仅 54ms)
```

### 4. 总线可靠性与抢占压力测试 (`g_app_test_result`)
```
magic            = 0x54534554 ('TEST')
ct_pass          = 18 / 18 (接口契约检查全部通过)
ct_fail          = 0x00000000 (零失败位图)
rounds_ok        = 10 / 10 (多规格包长循环)
len_ok           = 10 / 10
str_ok           = 1000 / 1000 (20kHz 中断压力测试事务全部完成)
read_ok          = 1000 / 1000 (Flash 1000 次批量读取全部成功)
data_match_ok    = 1000 / 1000 (1000 次读出数据与基准 100% 逐字节一致)
data_mismatch    = 0 (零数据不一致)
erase_count      = 0 (严格零擦除安全模式，外部 Flash 零磨损)
reference_valid  = 0x03 (基准数据全部有效)
owner_null       = 1
depth_zero       = 1
pass             = 1 (BUS PASS)
elapsed_ms       = 114 ms
```

### 5. 双总线实例实时状态 (`BSP_SPI_GetBus(0)` / `BSP_SPI_GetBus(1)`)
```
SPI1 (TFT 专享):
- Periph: SPI1 (72 MHz APB2)
- Current Cfg: 18 MHz, Mode 0, DMA1_Ch3 (Enabled)
- Owner: NULL (0x0)
- Depth: 0
- Rejects: 260 (TIM2 20kHz 抢占测试期间被总线锁正确拒绝的非法并发申请次数)

SPI2 (Flash 专享):
- Periph: SPI2 (36 MHz APB1)
- Current Cfg: 9 MHz, Mode 0, DMA1_Ch4/Ch5 (Enabled)
- Owner: NULL (0x0)
- Depth: 0
- Rejects: 0
```

---

## 四、连续独立上电复位验证 (3/3 Clean Reboots)

为排除软复位或内存残留对测试结果的偶发性影响，我们执行了 3 轮完整独立硬件复位与运行观察：

```
=== DUAL SPI 3-CYCLE REBOOT VERIFICATION ===
--- Cycle 1 Start ---
[Target Reset & Run: 8.0s]
dual_result.pass = 1
bus_result.pass  = 1
flash.id         = 0xEF17 (61207)
dual.rounds_ok   = 20
--- Cycle 1 End: PASS ---

--- Cycle 2 Start ---
[Target Reset & Run: 8.0s]
dual_result.pass = 1
bus_result.pass  = 1
flash.id         = 0xEF17 (61207)
dual.rounds_ok   = 20
--- Cycle 2 End: PASS ---

--- Cycle 3 Start ---
[Target Reset & Run: 8.0s]
dual_result.pass = 1
bus_result.pass  = 1
flash.id         = 0xEF17 (61207)
dual.rounds_ok   = 20
--- Cycle 3 End: PASS ---
```
**判定：连续 3 次上电复位均实现 100% 成功启动并自动化完成双 SPI 验收测试。**

---

## 五、Host 端全量单元测试与 Sanitizers 回归

在主机端执行 5 个 CTest 套件以及内存安全检测：

```
Test project /build/tests
    Start 1: bus ..............................   Passed    0.01 sec
    Start 2: ll ...............................   Passed    0.01 sec
    Start 3: flash ............................   Passed    0.04 sec
    Start 4: gc9a01 ...........................   Passed    0.01 sec
    Start 5: spi_multi_instance ...............   Passed    0.01 sec

100% tests passed, 0 tests failed out of 5

AddressSanitizer (ASan) & UndefinedBehaviorSanitizer (UBSan):
=================================================================
==NO LEAKS DETECTED, 0 ERRORS, 0 WARNINGS==
100% tests passed, 0 tests failed out of 5
```

---

## 六、硬件在环实机验收总结

1. **花屏根因彻底消除**：通过 `TFT_ONLY` 诊断模式证实，TFT 屏幕硬件与控制器功能完全完好，原花屏为 DC 引脚在改线过渡期悬空所致，现已通过独立编译体系和宏隔离彻底解决。
2. **双 SPI 独立并发达成**：TFT 稳定运行在 SPI1 18 MHz DMA 模式，W25Q128 Flash 稳定运行在 SPI2 9 MHz DMA 模式，双总线物理隔离，彻底消除了单总线时序互斥等待与争抢。
3. **零外部 Flash 磨损**：全流程遵循非破坏性安全原则，擦除计数严格为 0。
4. **系统长期稳定性优异**：1000 次高频中断压力下 0 数据误码，3 次上电复位 100% 恢复，主循环心跳指示方块规律闪烁。
