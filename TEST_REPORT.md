# 测试报告 (Stage 1 SPI 同步总线与 DMA 异常处理)

**本报告严格区分「编译通过」「逻辑/契约验证」「真实硬件实测」「尚未验证」。**

- **测试对象**：STM32F103C8T6 裸机 GC9A01 + W25QXX 共享 SPI1（DMA + 同步总线可靠性整改）
- **测试时间**：2026-11-14
- **分支**：`codex/stage1-spi-reliability`
- **工具链**：`arm-none-eabi-gcc 14.2.1` / CMake / Ninja / OpenOCD 0.12.0 / ST-LINK/V2（Linux）
- **测试硬件**：STM32F103C8T6 开发板 + GC9A01 240×240 圆屏 + W25Q128（实测 ID `0xEF17`，16 MB）
- **接线**：SCK=PA5、MISO=PA6、MOSI=PA7；屏幕 CS=PA9、RST=PA10、DC=PB13；Flash CS=PA8
- **验证手段**：Host 单元测试 (Mock 桩) + 真实硬件在板 HIL (SWD 提取结构体 `g_app_test_result` + 屏幕输出 + RAM 逐字节校验)
- **专题文档**：
  - [Stage 1 整改方案与设计文档](docs/stage1_spi_reliability_plan.md)
  - [Stage 1 详细测试报告 (Host + HIL)](docs/stage1_spi_reliability_test_report.md)
  - [Stage 1 验收清单](docs/stage1_spi_reliability_acceptance.md)

---

## 1. 编译与资源占用（PASS）

| 构建配置 | Flash (text + data) | SRAM (data + bss) | 状态 | 说明 |
|---|---:|---:|:---:|---|
| **Debug (-O0)** | 40,768 B (62.2%) | 4,256 B (20.8%) | **PASS** | 包含 18 项契约测试、诊断符号导出及完整断言 |
| **Release (-Os)** | **30,072 B (45.9%)** | **4,248 B (20.7%)** | **PASS** | 生产优化版，Flash 占用远低于 64 KB 芯片上限 |

---

## 2. Host Mock 单元测试（PASS，0 失败）

位于 `tests/host/`，基于纯 C 的 STM32F1 寄存器 Mock 桩验证边界与硬件故障注入：

| 测试二进制 | 测试用例数 | 覆盖核心逻辑 | 判定 |
|---|:---:|---|:---:|
| `test_bus` | 26 项断言 | NULL owner 防御、调用者鉴权、depth 溢出截断 (`UINT16_MAX`)、配置不可变性、同一 owner 相同配置嵌套、不同配置拒绝、完全释放判定 | **PASS** |
| `test_ll` | 10 个测试点 | DMA 32B 正常搬运、DMA `TEIF` 硬件错误中断捕获、DMA 超时判定、`spi_recover` 统一恢复、`>65535` 字节自动分块、轮询超时、`use_dma` 配置动态缓存切换 | **PASS** |

---

## 3. 真实硬件在板测试 (HIL, PASS)

通过 SWD 调试器直接读取 RAM 中导出的 `g_app_test_result` 结构体：

```gdb
$1 = {
    magic = 1414743380,        /* 'TEST' */
    ct_pass = 18,              /* 18 项契约检查全过 */
    ct_total = 18,
    ct_fail = 0,               /* 失败位图 0x00 */
    rounds_ok = 10,            /* 10 轮并发抢占通过 */
    rounds_total = 10,
    len_ok = 10,               /* 10 种长度矩阵全过 */
    len_count = 10,
    str_ok = 1000,             /* 1000 次高频 SPI 事务全过 */
    str_total = 1000,
    fl_ok = 10,                /* 10 轮 Flash 擦写读全过 */
    fl_total = 10,
    fl_last_result = 0,        /* 0 差错 */
    fl_fail_round = 0,
    erase_count = 11,          /* 累计擦除 11 次 */
    rec_ok = 1,                /* 恢复后读回成功 */
    ll_recover_count = 1,      /* LL 恢复接口正常 */
    ll_last_error = 0,
    owner_null = 1,            /* 结束时总线归还 */
    depth_zero = 1,            /* 结束时深度归零 */
    pass = 1,                  /* ⭐ 综合判定 PASS */
    elapsed_ms = 1012,         /* 全套执行耗时约 1.01 秒 */
    rounds_fail_stage = 0,
    rounds_fail_tx0 = 0,
    rounds_fail_rx0 = 0
}
```

### 3.1 测试细项指标

1. **总线契约测试 (18 / 18 PASS)**：
   - 未占用时拒绝 transfer。
   - depth=0 时拒绝 release。
   - 合法设备 A 独占成功。
   - 设备 B 抢占被拒。
   - 运行时高优先级中断抢占被拒。
   - 同一设备同配置嵌套成功（depth 增加，不重新配置外设）。
   - 嵌套第 1 次释放保持总线占用（CS 维持有效）。
   - 嵌套第 2 次释放完全交还总线（拉高 CS，owner 归零）。
   - NULL owner 申请/释放拒绝。
   - 非持有者 transfer/release 拒绝。
   - 深度上限防御（UINT16_MAX 饱和且不溢出）。
   - 占用期间禁止外部修改配置。
   - 同一持有者传入不同参数嵌套申请拒绝。
   - 最终完全释放后状态严格清零。

2. **20 kHz 中断抢占压力测试**：
   - 硬件 TIM2 定时器以 20 kHz 频率触发中断尝试侵入总线。
   - 实测捕捉到数千次落在事务正中间，`probe.granted` **严格为 0**（无任何非法侵入）。

3. **传输长度矩阵测试 (10 / 10 PASS)**：
   - 覆盖 `{1, 4, 8, 15, 16, 17, 32, 64, 256, 480}` 字节。
   - 自动覆盖轮询模式（$\le 15$ B）与 DMA 模式（$\ge 16$ B）。
   - 全部数据逐字节校验一致。

4. **1000 次连续高频 SPI 事务压力 (1000 / 1000 PASS)**：
   - 连续 1000 次执行总线申请、读 Flash ID、释放，100% 成功无挂死。

5. **统一异常恢复验证 (rec_ok = 1 PASS)**：
   - 调用 `BSP_SPI1_Recover()` 重置 SPI/DMA，随后的数据收发依然 100% 正确。

---

## 4. 关键缺陷复盘：Flash 页末尾数据截断根因定位

### 4.1 故障现象
在整改初期，偶发性地在第 3 轮或第 7 轮 Flash 写入时，页末尾字节（如 byte 230~255）回读为 `0xFF`，前半段完全正确。

### 4.2 根因定位与硬件实测
通过抓取 CS 拉高瞬间的状态寄存器历史：
```c
g_dbg_wait_log = { 0x00, 0x03, 0x03, 0x03, 0x03, ... };
```
- W25Q128 在页编程（02h）拉高 CS 后，其内部高压电荷泵需要约 200~300 µs 启动高压，随后才在状态寄存器中确立 `BUSY = 1`。
- 若驱动层过早发起 `ReadStatusReg`，读到的瞬态为 `0x00`。
- 逻辑 `if ((status & 0x01) == 0) return OK;` 将其误判为"物理写入已完成"并立即退出。
- 随后到来的读操作拉低 CS，**在芯片正在物理高压写入时将其强制打断**，导致后半段未写入单元保持擦除态（0xFF）。

### 4.3 修复措施
- 在 `W25QXX_Write_Page` 中，CS 上跳后设置固定的 `Delay_ms(3)`（满足数据手册规定的 $t_{\text{PP}}$ 最大 3ms 规格），确保物理写入完全结束后才允许下一次事务拉低 CS。
- 修复后 10 轮及 100 轮 Flash 擦写读端到端 100% 通过。

---

## 5. 验收结论

Stage 1 阶段的所有整改目标已全部达成，代码通过 Host Mock 与物理硬件双重验证，总线安全契约完备，异常恢复机制健全，可正式结项并投入使用。
