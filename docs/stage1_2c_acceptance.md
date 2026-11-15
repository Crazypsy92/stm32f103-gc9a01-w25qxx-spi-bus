# Stage 1.2-C: Flash 故障注入精确性与测试逻辑收尾验收清单

> **项目**: `stm32f103-gc9a01-w25qxx-spi-bus`  
> **分支**: `codex/stage1_2-flash-reliability`  
> **验收时间**: 2026-11-15  
> **最终验收结论**: **全项通过 (ALL PASS)**  

---

## 1. 详细验收对照表

| 序号 | 需求整改项 | 验收标准 | 实施位置 | 验收结果 |
|:---:|---|---|---|:---:|
| **1** | **T04 精确故障注入** | 注入前确认页编程指令与数据已实际发出；在 Page Program 最终 CS 释放时注入失败；断言返回 `ERROR_BUS`、`page_program_count==1`、`last_command==0x02`、`fault_injected_at==0x02`，且 CS 引脚维持拉低 | `tests/host/test_flash.c` | **PASS** (Host) |
| **2** | **T05 精确故障注入** | 注入前确认扇区擦除指令与地址已实际发出；在 Sector Erase 最终 CS 释放时注入失败；断言返回 `ERROR_BUS`、`erase_sector_count==1`、`last_command==0x20`、`fault_injected_at==0x20`，且 CS 引脚维持拉低 | `tests/host/test_flash.c` | **PASS** (Host) |
| **3** | **保留 T01~T03 故障注入** | 状态寄存器读取、数据读取、写使能各阶段末尾 CS 释放失败均准确定位断言 | `tests/host/test_flash.c` | **PASS** (Host) |
| **4** | **破坏性模式基准动态更新** | 破坏性模式下，每当一轮写入并读回校验成功后实时同步 `expected_256` 为最新写入数据；恢复测试比对最新数据；中途写入失败时置 `expected_256_valid=0` 拒绝误报 | `BSP/Src/app_test.c`<br>`tests/host/test_flash.c` | **PASS** (Host & Code) |
| **5** | **初始基准读取有效性检查** | 检查 256B 与 480B 两次基准读取的返回值；维护 `reference_valid` 状态位图（bit0=256B, bit1=480B）；基准读取失败时记录错误、标记依赖测试为 FAIL 并阻断未初始化比对 | `BSP/Src/app_test.c` | **PASS** (Code & HIL) |
| **6** | **基准失效状态机测试** | 覆盖 256B 读取失败、480B 读取失败、两次均成功以及数据一致性检验的分支断言 | `tests/host/test_flash.c` | **PASS** (Host Test 8) |
| **7** | **综合 PASS 前置条件强化** | 综合测试通过必须满足 `(reference_valid == 0x03U)`，任何基准读取失败均不得报告系统 PASS | `BSP/Src/app_test.c` | **PASS** (Code & HIL) |
| **8** | **真机非破坏性模式安全运行** | 默认安全模式下严格 0 擦除（`erase_count=0`），SWD 实测读取 `reference_valid=3`, `expected_256_valid=1`, `pass=1` | STM32F103 实机 SWD | **PASS** (HIL) |
| **9** | **真机破坏性物理擦写测试** | 安全约束：禁止在真实 Flash 上执行破坏性物理擦写操作，防范芯片硬件磨损与数据丢失 | STM32F103 硬件策略 | **BLOCKED** (按规范标注) |
| **10** | **双配置编译零告警** | Debug (Ninja) 与 Release (Make) 构建均无任何警告或错误 | 编译工具链 | **PASS** |

---

## 2. 验收指标分类总表

| 维度 | 指标项 | 预期目标 | 实测结果 | 判定状态 |
|:---|---|:---:|:---:|:---:|
| **Host 单元测试** | `test_bus` 契约断言 | 26 / 26 PASS | 26 / 26 PASS | **PASS** |
| **Host 单元测试** | `test_ll` 异常注入 | 10 / 10 PASS | 10 / 10 PASS | **PASS** |
| **Host 单元测试** | `test_flash` 全量断言 (含精确注入与基准跟踪) | 104 / 104 PASS | 104 / 104 PASS | **PASS** |
| **编译质量** | Debug 编译警告/错误 | 0 Warnings / 0 Errors | 0 / 0 | **PASS** |
| **编译质量** | Release 编译警告/错误 | 0 Warnings / 0 Errors | 0 / 0 | **PASS** |
| **固件资源** | Debug Flash / SRAM | $\le$ 64KB / 20KB | 40,184 B / 5,280 B | **PASS** |
| **固件资源** | Release Flash / SRAM | $\le$ 64KB / 20KB | 29,892 B / 5,272 B | **PASS** |
| **真机 HIL (非破坏性)** | 总线契约检查 (`ct_pass / ct_total`) | 18 / 18 | 18 / 18 PASS | **PASS** |
| **真机 HIL (非破坏性)** | 20kHz 抢占探针中断被拒绝 (`probe.granted`) | 严格为 0 | 0 | **PASS** |
| **真机 HIL (非破坏性)** | 基准数据读取有效性 (`reference_valid`) | 0x03 (3) | 3 (0x03) | **PASS** |
| **真机 HIL (非破坏性)** | 动态恢复基准有效性 (`expected_256_valid`) | 1 | 1 | **PASS** |
| **真机 HIL (非破坏性)** | 1000 次连续读取通信成功率 (`read_ok`) | 1000 / 1000 | 1000 / 1000 PASS | **PASS** |
| **真机 HIL (非破坏性)** | 1000 次连续读取数据一致率 (`data_match_ok`) | 1000 / 1000 | 1000 / 1000 PASS | **PASS** |
| **真机 HIL (非破坏性)** | 数据不一致计数 (`data_mismatch`) | 严格为 0 | 0 | **PASS** |
| **真机 HIL (非破坏性)** | 统一异常恢复检验 (`rec_ok`) | 1 | 1 PASS | **PASS** |
| **真机 HIL (非破坏性)** | 默认上电物理扇区擦除次数 (`erase_count`) | 严格为 0 | 0 | **PASS** |
| **真机 HIL (非破坏性)** | 总体测试通过标记 (`pass`) | 1 | 1 PASS | **PASS** |
| **真机 HIL (破坏性)** | 物理 Flash 破坏性擦除与写入 | 依安全规范禁用 | 未执行物理擦写 | **BLOCKED** |

---

## 3. 验收总结与签署

Stage 1.2-C 已彻底解决 Stage 1.2 / 1.2-B 遗留的 3 项可靠性与测试精度缺陷：
1. **精确故障注入验证**：T04 与 T05 成功验证了真实页编程与扇区擦除指令末尾 CS 释放失败时的防御能力，杜绝了提前在 WREN 阶段返回的测试漏洞；
2. **动态基准自适应**：在破坏性模式下，恢复测试与最新有效数据进行比对，并在写入失败时严厉阻断误报；
3. **基准有效性熔断**：显式捕获基准读取失败，杜绝未初始化内存假通过；
4. **测试分级清晰明确**：Host Mock 104 项全量通过（**PASS**），非破坏性真机 HIL 验证达标（**PASS**），破坏性实机物理擦写按安全规则严格保持禁用（**BLOCKED**）。

**Stage 1.2-C 验收结论：通过 (PASS)**。
