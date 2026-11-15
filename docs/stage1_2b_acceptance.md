# Stage 1.2-B: Flash 错误处理与数据校验补充修复验收清单

> **项目**: `stm32f103-gc9a01-w25qxx-spi-bus`  
> **分支**: `codex/stage1_2-flash-reliability`  
> **验收时间**: 2026-11-15  
> **验收结论**: **全项通过 (ALL PASS)**  

---

## 1. 详细验收对照表

| 序号 | 需求整改项 | 验收标准 | 实施位置 | 验收结果 |
|:---:|---|---|---|:---:|
| **1** | **CS 释放失败不被忽略** | 检查所有 `W25_CS_HIGH` 调用，失败时返回 `W25QXX_ERROR_BUS`，禁止误报 `W25QXX_OK` | `BSP/Src/w25qxx.c` | **PASS** |
| **2** | **ReadStatusReg 状态保护** | 释放失败时不得将状态读取结果作为有效成功结果提交 | `BSP/Src/w25qxx.c` | **PASS** |
| **3** | **写/擦除 CS 释放拦截** | Page Program、Sector Erase、Chip Erase 的 CS 释放失败必须被识别并阻断 | `BSP/Src/w25qxx.c` | **PASS** |
| **4** | **保留首个故障原因** | 错误发生时清理操作不覆盖原始故障状态 | `BSP/Src/w25qxx.c` | **PASS** |
| **5** | **禁止篡改总线结构** | 严格遵从 `spi_bus_release` 契约，不篡改 `owner` 或 `depth` | `BSP/Src/w25qxx_port.c` | **PASS** |
| **6** | **T01~T08 CS 故障注入** | Host Mock 覆盖状态读取、数据读取、写使能、页写、擦除释放失败及恢复 | `tests/host/test_flash.c` | **PASS** (T01~T08 全过) |
| **7** | **非破坏性数据完整性校验** | 重复读取对比完整缓冲区（不仅查返回值）；覆盖 1~480B 长度矩阵 | `BSP/Src/app_test.c` | **PASS** (10 长度全比对) |
| **8** | **独立统计指标** | 连续 1000 次读取独立统计 `read_ok` 与 `data_match_ok`，支持 `data_mismatch` 诊断 | `BSP/Src/app_test.c` | **PASS** (1000/1000 匹配) |
| **9** | **数据不一致注入检测** | 能敏锐捕捉到模拟 Flash 数据被篡改，准确增加 `data_mismatch` 并记录偏移 | `tests/host/test_flash.c` | **PASS** |
| **10** | **已知设备 ID 检查** | 检查并断言 `id == W25Q128` (0xEF17) 与 `capacity == 16MB` | `BSP/Src/app_test.c` | **PASS** |
| **11** | **默认零擦除安全启动** | 默认上电严禁执行任何物理擦除或写入，实际擦除次数严格保持为 0 | `Core/Src/main.c`<br>`BSP/Src/app_test.c` | **PASS** (`erase_count=0`) |
| **12** | **真机实测数据核验** | STM32F103 实测 18 项契约全过、10 轮抢占通过、1000 次事务通过、`rec_ok=1`、`pass=1` | GDB / SWD 实测 | **PASS** |

---

## 2. 验收指标数据总表

| 维度 | 指标项 | 预期目标 | 实测结果 | 判定 |
|:---|---|:---:|:---:|:---:|
| **Host Mock** | `test_bus` 契约断言 | 26 / 26 PASS | 26 / 26 PASS | **PASS** |
| **Host Mock** | `test_ll` 异常注入 | 10 / 10 PASS | 10 / 10 PASS | **PASS** |
| **Host Mock** | `test_flash` 全量断言 | 72 / 72 PASS | 72 / 72 PASS | **PASS** |
| **编译器质量** | GCC 编译警告数 (Debug/Release) | 0 Warnings | 0 Warnings | **PASS** |
| **固件资源** | Debug Flash / SRAM | $\le$ 64KB / 20KB | 40,032 B / 5,016 B | **PASS** |
| **固件资源** | Release Flash / SRAM | $\le$ 64KB / 20KB | 29,812 B / 5,008 B | **PASS** |
| **HIL 契约** | `ct_pass` (总线契约测试) | 18 / 18 PASS | 18 / 18 PASS | **PASS** |
| **HIL 抢占** | 20kHz 抢占中断 `probe.granted` | 严格为 0 | 0 | **PASS** |
| **HIL 数据** | 1000 次连续读取通信成功率 | 1000 / 1000 | 1000 / 1000 PASS | **PASS** |
| **HIL 数据** | 1000 次连续读取数据一致率 | 1000 / 1000 | 1000 / 1000 PASS | **PASS** |
| **HIL 安全** | 默认上电物理扇区擦除次数 | 严格为 0 | 0 | **PASS** |
| **HIL 设备** | W25Q128 识别 ID / 容量 | 0xEF17 / 16MB | 0xEF17 / 16MB | **PASS** |

---

## 3. 最终验收结论

Stage 1.2-B 针对片选释放失败识别、故障注入测试及只读数据完整性校验的三个修复任务已全部高质量达成。系统在保持默认零写入、零擦除安全原则的前提下，实现了从接口返回值到数据一致性的工业级防御闭环。

**Stage 1.2-B 验收结论：全项通过 (ALL PASS)**。
