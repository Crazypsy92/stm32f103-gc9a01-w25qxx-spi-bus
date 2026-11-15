# Stage 1: SPI 同步总线与 DMA 异常处理验收清单

> **项目**: `stm32f103-gc9a01-w25qxx-spi-bus`  
> **分支**: `codex/stage1-spi-reliability`  
> **验收时间**: 2026-11-14  
> **验收结论**: **全项通过 (ALL PASS)**

---

## 1. 详细验收对照表

### 3.1 SPI 总线所有权管理整改

| 序号 | 需求整改项 | 验收标准 | 实施位置 | 验收结果 |
|:---:|---|---|---|:---:|
| 1 | `transfer` 增加调用者 `owner` 参数 | 必须传入 `owner`，且必须与当前占用者一致；非持有者调用立即拒绝 | `BSP/Inc/spi_bus.h` | **PASS** |
| 2 | NULL owner 防御 | 禁止 `owner == NULL` 进行申请或释放，状态保持不变 | `BSP/Inc/spi_bus.h` | **PASS** |
| 3 | `depth` 深度溢出保护 | 达到 `UINT16_MAX` 时拒绝继续嵌套，防止无符号溢出回绕 | `BSP/Inc/spi_bus.h` | **PASS** |
| 4 | `depth` 与 `owner` 状态一致性 | 强约束 `(owner == NULL) <=> (depth == 0)`，违反则拒绝操作 | `BSP/Inc/spi_bus.h` | **PASS** |
| 5 | 嵌套期间禁止重配 SPI | 同一 owner 嵌套 `acquire` 时仅递增 `depth`，不重复调用 `configure` | `BSP/Inc/spi_bus.h` | **PASS** |
| 6 | 嵌套配置一致性校验 | 嵌套申请时，入参配置必须与 `active_cfg` 完全一致，否则拒绝 | `BSP/Inc/spi_bus.h` | **PASS** |
| 7 | 占用期间配置锁定 | 总线被占用期间外部直接调用 `configure` 立即拒绝 | `BSP/Inc/spi_bus.h` | **PASS** |
| 8 | 片选与嵌套深度对齐 | 仅当 `spi_bus_release` 返回 1（深度归零）时，Port 层才拉高物理 CS | `BSP/Src/gc9a01_port.c`<br>`BSP/Src/w25qxx_port.c` | **PASS** |

---

### 3.2 SPI 配置缓存整改

| 序号 | 需求整改项 | 验收标准 | 实施位置 | 验收结果 |
|:---:|---|---|---|:---:|
| 1 | 配置缓存全覆盖 | 缓存覆盖全部字段：`clock_hz`、`mode`、`use_dma` | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 2 | `use_dma` 切换感知 | 即使时钟与模式不变，`use_dma` 改变后准确更新传输路径 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 3 | 运行态禁止重配 | SPI 处于 BSY 或 DMA 通道使能运行时，拒绝修改配置并返回 `SPI_LL_ERROR_BUSY` | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 4 | 参数范围检查 | 拒绝非法时钟（0 或超过 PCLK）及非法 SPI 模式（>3），返回 `SPI_LL_ERROR_PARAM` | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 5 | 避免无意义重复初始化 | 参数未改变时（`clock_hz` 与 `mode` 相同），不重新开关 SPI 外设 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 6 | 动态频率切换测试 | 18 MHz (屏幕) 与 9 MHz (Flash) 之间反复交替切换，外设稳定运行 | HIL 实测 + Host Mock | **PASS** |

---

### 3.3 DMA 错误检测与时序整改

| 序号 | 需求整改项 | 验收标准 | 实施位置 | 验收结果 |
|:---:|---|---|---|:---:|
| 1 | TX/RX 完成标志检查 | 同时监测 TX 与 RX 的 `TCIF` 标志 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 2 | DMA 传输错误判定 | 同时监测 TX 与 RX 的 `TEIF` 标志，一旦触发立即终止等待并返回 `SPI_LL_ERROR_DMA` | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 3 | 超时判定不能按正常处理 | DMA 超时返回明确的 `SPI_LL_ERROR_TIMEOUT`，不走正常完成逻辑 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 4 | 统一错误状态码 | 区分返回 `SPI_LL_OK`, `SPI_LL_ERROR_PARAM`, `SPI_LL_ERROR_TIMEOUT`, `SPI_LL_ERROR_DMA`, `SPI_LL_ERROR_BUSY`, `SPI_LL_ERROR_RECOVERY` | `BSP/Inc/spi_stm32_ll.h` | **PASS** |
| 5 | 传输结束硬件确认 | DMA 搬运完成后，严格等待 `TXE == 1` 且 `BSY == 0` 后才返回 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 6 | 防片选提前释放 | 不允许 DMA/SPI 尚未完全退出繁忙就提前释放总线 | `BSP/Src/spi_stm32_ll.c` | **PASS** |

---

### 3.4 轮询传输整改

| 序号 | 需求整改项 | 验收标准 | 实施位置 | 验收结果 |
|:---:|---|---|---|:---:|
| 1 | 等待环节全超时保护 | 轮询中每个字节的 TXE、RXNE 等待均具备 DWT 绝对时基超时保护 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 2 | 超时统一清理 | 发生超时调用 `spi_recover` 复位外设，绝不陷入死循环 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 3 | 最后一字节移出确认 | 循环结束后等待 `BSY == 0`，确认总线真正空闲 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 4 | 全双工与 OVR 处理 | TX-only 传输自动排空全双工产生的 RX 残留，并清除 OVR 标志 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 5 | 基于 DWT 周期计时 | 基于 `DWT->CYCCNT`，无符号差值天然处理 32 位回绕，设置合理超时预算 | `BSP/Src/spi_stm32_ll.c` | **PASS** |

---

### 3.5 统一 SPI 异常恢复机制

| 序号 | 需求整改项 | 验收标准 | 实施位置 | 验收结果 |
|:---:|---|---|---|:---:|
| 1 | 禁用 DMA 请求与通道 | 关闭 SPI DMA 请求，禁用 DMA 通道并清除通道中断标志 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 2 | 排空接收与清 OVR | 读空 SPI_DR 并执行清除 OVR 序列 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 3 | 外设状态重置 | 检测 BSY 并在异常时重新使能 SPI 外设，重载已知配置 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 4 | 片选独立性 | 恢复机制不触碰设备 CS 引脚，CS 由上层 Port 严格管理 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 5 | 恢复后通信验证 | 恢复完成后立即执行 Flash 读回，校验通过（`rec_ok == 1`） | `BSP/Src/app_test.c` | **PASS** |

---

### 3.6 DMA 长度边界分块处理

| 序号 | 需求整改项 | 验收标准 | 实施位置 | 验收结果 |
|:---:|---|---|---|:---:|
| 1 | 长度安全分块 | 单次 DMA 限制 $\le 65535$，超过则自动循环切块传输 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 2 | 分块片选连续性 | 分块期间由 Port 层保持 CS 有效，不拉高 CS，不重置外设配置 | `BSP/Src/spi_stm32_ll.c` | **PASS** |
| 3 | 常用块兼容性 | 1B, 4B, 8B, 15B, 16B, 17B, 32B, 64B, 256B, 480B 逐字节完全一致 | `BSP/Src/app_test.c` | **PASS** |

---

## 2. 测试验收指标汇总

| 类别 | 指标项 | 预期目标 | 实际测试值 | 判定 |
|:---|---|:---:|:---:|:---:|
| **Host Mock** | `test_bus` 契约断言 | 26 / 26 PASS | 26 / 26 PASS | **PASS** |
| **Host Mock** | `test_ll` 异常注入 | 10 / 10 PASS | 10 / 10 PASS | **PASS** |
| **HIL 契约** | `ct_pass` (总线契约测试) | 18 项全部通过 | 18 / 18 PASS | **PASS** |
| **HIL 契约** | `ct_fail` (契约失败位图) | `0x00000000` | `0x00000000` | **PASS** |
| **HIL 中断** | 20 kHz 中断抢占 `probe.granted` | 严格为 0 | 0 | **PASS** |
| **HIL 长度** | `len_ok` (1~480B 全长度矩阵) | 10 / 10 项通过 | 10 / 10 PASS | **PASS** |
| **HIL 压力** | `str_ok` (1000 次连续 SPI 事务) | 1000 / 1000 | 1000 / 1000 PASS | **PASS** |
| **HIL Flash**| `fl_ok` (Flash 擦写读端到端闭环) | 10 / 10 轮 | 10 / 10 PASS | **PASS** |
| **HIL 恢复** | `rec_ok` (热重启与恢复测试) | 1 (成功) | 1 | **PASS** |
| **HIL 状态** | 结束状态 `owner == NULL && depth == 0` | 保持纯净 | `owner_null=1, depth_zero=1` | **PASS** |
| **HIL 总体** | `g_app_test_result.pass` | 1 (PASS) | **1 (PASS)** | **PASS** |

---

## 3. 验收结论

Stage 1 中定义的所有可靠性改进任务已 100% 落实到位，代码通过 Host Mock 单元测试与真实硬件在板 HIL 测试的双重闭环验证。所有安全不变式在实际物理硬件上均保持成立，系统具备确定的所有权隔离、完善的 DMA 错误捕获及自动热恢复能力。

**Stage 1 准予正式验收结项。**
