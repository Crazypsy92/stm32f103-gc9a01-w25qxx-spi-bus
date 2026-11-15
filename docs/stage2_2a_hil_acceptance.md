# Stage 2.2-A-HIL：双 SPI 硬件在环联合验收签字表 (Acceptance Sheet)

## 一、验收结果矩阵

| 序号 | 验收测试项 | 验收指标 / 通过标准 | 实际测试记录 | 证据索引 | 判定 |
|:---:|---|---|---|---|:---:|
| 1 | **TFT 纯色显示** | 5/5 正确（黑/红/绿/蓝/白） | `fills_ok_count = 5/5`，肉眼目检纯净无杂色 | `evidence/stage2_2a_hil/tft_diag_log.txt` | **PASS** |
| 2 | **TFT 字符及图案** | 目检正常无乱码、位移 | 8 色条流动刷新、圆形居中、文本清晰 | `evidence/stage2_2a_hil/dual_spi_hil_log.txt` | **PASS** |
| 3 | **SPI1 轮询与 DMA** | 1.125M ~ 18M 轮询及 18M DMA 正常 | 1.125M/4.5M/9M/18M 均 5/5 PASS，DMA TEIF=0 | `docs/tft_display_diagnosis.md` | **PASS** |
| 4 | **SPI2 Flash ID** | 必须读出 `0xEF17` (W25Q128) | `g_flash.id = 0xEF17`，容量 16MB | `evidence/stage2_2a_hil/dual_spi_hil_log.txt` | **PASS** |
| 5 | **SPI2 DMA 读校验**| 256 B 与 480 B DMA 校验 100% 一致 | `dma256_ok_count = 20`，`dma480_ok_count = 20` | `evidence/stage2_2a_hil/dual_spi_hil_log.txt` | **PASS** |
| 6 | **双设备交替执行** | 20/20 PASS | `rounds_ok = 20 / 20`，`pass = 1` | `evidence/stage2_2a_hil/dual_spi_hil_log.txt` | **PASS** |
| 7 | **总线持有者与深度** | 结束后 SPI1 与 SPI2 owner/depth 归零 | `owner_null = 1`，`depth_zero = 1`（双总线独立） | `evidence/stage2_2a_hil/dual_spi_hil_log.txt` | **PASS** |
| 8 | **双总线配置隔离** | 双总线速率/模式互不串扰 | SPI1 (18M) 与 SPI2 (9M) 独立上下文运行 | `evidence/stage2_2a_hil/dual_spi_hil_log.txt` | **PASS** |
| 9 | **高频中断压力** | 20kHz TIM2 中断下 1000 次事务一致 | `str_ok = 1000`，`data_match_ok = 1000`，`mismatch = 0` | `evidence/stage2_2a_hil/dual_spi_hil_log.txt` | **PASS** |
| 10 | **独立上电复位验证**| 连续 3 次独立上电测试 3/3 PASS | 3 次硬件复位均成功运行并返回 `pass = 1` | `evidence/stage2_2a_hil/reboot_verification_log.txt` | **PASS** |
| 11 | **系统运行稳定性** | 零 HardFault，主循环心跳方块正常 | 异常计数 0，PC 处于正常任务调度，心跳规律翻转 | SWD 实时在线监视 | **PASS** |
| 12 | **外部存储安全约束**| 物理擦除次数严格为 0（零磨损） | `erase_count = 0`，未写入任何物理扇区 | `evidence/stage2_2a_hil/dual_spi_hil_log.txt` | **PASS** |
| 13 | **Host 单元测试套件**| 5 套 CTest 全部通过 (100%) | `bus`, `ll`, `flash`, `gc9a01`, `spi_multi_instance` | `evidence/stage2_2a_hil/host_ctest_log.txt` | **PASS** |
| 14 | **内存安全 (Sanitizer)**| 0 leak, 0 heap-buffer-overflow | ASan + UBSan 零警告、零泄漏 | `evidence/stage2_2a_hil/host_ctest_log.txt` | **PASS** |

---

## 二、关键技术指标达成汇总

| 指标项 | Stage 1.x (单 SPI1 共享) | Stage 2.2-A-HIL (双 SPI 独立) | 提升效果 |
|---|---|---|---|
| **TFT 总线速率** | 18 MHz (SPI1) | 18 MHz (SPI1) | 独占带宽，无等待 |
| **Flash 总线速率** | 9 MHz (SPI1 降速重配) | 9 MHz (SPI2 独占) | 硬件独立分频，无需动态重置外设寄存器 |
| **总线争抢与锁等待** | TFT 刷屏期间 Flash 必须等待 | TFT 与 Flash 物理并发，零争抢 | 彻底消除跨外设互斥停顿 |
| **20 轮交替耗时** | ~95 ms | **54 ms** | 吞吐率提升 ~43% |
| **GPIO 冲突防护** | 无冲突防护 | 软硬件双向互斥拦截与宏防御 | 杜绝接线与固件不一致烧毁外设 |

---

## 三、结项结论

Stage 2.2-A-HIL（TFT 花屏定位、最小诊断、双 SPI 独立架构与真机验收）所规划的全部 3 项开发任务与各项软硬件约束指标已**100% 达成并通过实机联合检验**。

项目已具备进入后续阶段的代码质量与硬件可靠性基线。
