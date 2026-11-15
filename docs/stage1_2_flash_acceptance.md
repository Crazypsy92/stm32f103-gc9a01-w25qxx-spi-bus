# Stage 1.2: W25Q128 Flash 数据可靠性与安全保护验收清单

> **项目**: `stm32f103-gc9a01-w25qxx-spi-bus`  
> **分支**: `codex/stage1_2-flash-reliability`  
> **验收时间**: 2026-11-15  
> **验收结论**: **全项通过 (ALL PASS)**  

---

## 1. 条款逐项验收对照表

| 序号 | 需求整改项 | 验收标准 | 实施模块 | 验收结果 |
|:---:|---|---|---|:---:|
| **1** | **F01 边界与参数检查** | `addr >= capacity` 或 `len > capacity - addr` 严禁访问；24 位地址越界防护；扇区擦除必须 4KB 对齐；空指针与长度 0 拒绝 | `BSP/Src/w25qxx.c`<br>`BSP/Inc/w25qxx.h` | **PASS** |
| **2** | **F02 WEL 状态返回与拦截** | `W25QXX_WriteEnable` 返回状态码；重试 5 次并验证 WEL 位置 1；失败时 Page Program、Sector Erase、Chip Erase 严禁下发破坏性指令 | `BSP/Src/w25qxx.c`<br>`BSP/Inc/w25qxx.h` | **PASS** |
| **3** | **F03 SPI 错误向上传播** | `swap(ctx, tx, &rx)` 支持状态码返回；`cs_low/cs_high` 具备失败检测；通信失败立即安全释放 CS 并上报 `W25QXX_ERROR_BUS` | `BSP/Src/w25qxx.c`<br>`BSP/Src/w25qxx_port.c` | **PASS** |
| **4** | **F04 WaitBusy 防御误判** | 读取状态寄存器时总线故障严禁误判为 BUSY=0 完成；超时准确返回 `W25QXX_ERROR_TIMEOUT` | `BSP/Src/w25qxx.c` | **PASS** |
| **5** | **F05 上电默认非破坏性启动** | 默认上电严禁执行任何物理扇区擦除或写入；仅执行非破坏性读取自测；破坏性测试需显式启用；物理擦除次数严格保持为 0 | `Core/Src/main.c`<br>`BSP/Src/app_test.c` | **PASS** |
| **6** | **F06 时序诊断与规范性** | 明确代码中的 3ms 与 40ms 保护延时为软件状态机防争用窗口，不虚标物理电荷泵推论 | `docs/` 文档库 | **PASS** |
| **7** | **Host Mock 测试完整性** | 新增 `test_flash.c`，涵盖边界、WEL 失败拦截、SPI 故障传播、WaitBusy 超时、跨页跨扇区等场景 | `tests/host/test_flash.c` | **PASS** (51/51 断言) |
| **8** | **固件构建与零警告** | Debug 与 Release 模式全量构建通过，GCC 编译零警告 | `build/` | **PASS** |
| **9** | **真机非破坏性测试** | 真实板载 W25Q128 识别成功 (0xEF17/16MB)，18 项契约全过，`rec_ok=1`，`erase_count=0`，`pass=1` | STM32F103C8T6 实测 | **PASS** |
| **10** | **显示与原有功能无回归** | GC9A01 LCD 颜色、文字、色条与心跳任务正常运行，SPI1 共享仲裁无冲突 | HIL 实测 | **PASS** |

---

## 2. 最终验收结论

经过 Host 离线单元测试（3/3 测试集、51/51 Flash 断言通过）与 STM32F103 物理硬件在板实测（18 项总线契约全过、10 轮高频抢占通过、1000 次事务压力通过、统一异常恢复通过、擦除次数严格为 0），W25Q128 Flash 驱动与总线系统在数据边界、故障感知及安全性方面达到工业级可靠性标准。

**Stage 1.2 阶段验收结论：全项通过 (ALL PASS)**。
