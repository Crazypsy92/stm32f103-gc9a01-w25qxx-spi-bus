# Stage 1.2-B: Flash 错误处理与数据校验补充修复报告

> **日期**: 2026-11-15  
> **分支**: `codex/stage1_2-flash-reliability`  
> **基准 Commit**: `5f40a009144063e6e8f62e585cf960ac98a965c2`  
> **测试环境**: Host Mock (Ubuntu x86_64, GCC, CTest) + STM32F103C8T6 HIL 实机 (SWD / OpenOCD 0.12.0)  

---

## 1. 任务背景与修复内容

在 Stage 1.2 完成 W25Q128 基础可靠性整改的基础上，针对潜在的三个隐蔽缺口进行了小范围的精准修复与增强：

### 1.1 Task 1: 修复 CS 释放失败被忽略的问题
- **问题分析**：在原 `w25qxx.c` 中，通信流程结束时的 `W25_CS_HIGH(dev)` 调用全部被强转为 `(void)`，若底层因参数错误、非持有者释放或硬件引脚故障导致片选释放失败，上层仍会继续返回 `W25QXX_OK`，甚至在写操作中继续等待物理写入，造成状态错误与误报。
- **修复方案**：
  1. 全面检查并更新所有 `W25_CS_HIGH(dev)` 调用点：
     - `W25QXX_ReadStatusReg`：若释放失败，立即返回 `W25QXX_ERROR_BUS`，且**严禁提交读取到的状态值**。
     - `W25QXX_WriteEnable`：发送 WREN 命令后若 CS 释放失败，立即返回 `W25QXX_ERROR_BUS`，**阻断后续任何写操作**。
     - `W25QXX_Read`：正常读取完毕后检查 CS 释放，失败则返回 `W25QXX_ERROR_BUS`。
     - `W25QXX_Write_Page`：正常数据写入后若 CS 释放失败，立即返回 `W25QXX_ERROR_BUS`，**不得进入延时或 WaitBusy，更不得报告编程成功**。
     - `W25QXX_EraseSector` / `W25QXX_EraseChip`：命令发送后检查 CS 释放，失败则终止并返回 `W25QXX_ERROR_BUS`。
     - `W25QXX_ReadID` / `W25QXX_PowerDown` / `W25QXX_WakeUp`：同理严格检查返回值。
  2. 错误清理与首要错误保留：在数据阶段出现 `swap` 或 `bulk` 错误时，调用 `(void)W25_CS_HIGH(dev)` 尽力清理，并保留首个通信错误原因。
  3. 不篡改总线结构：严格遵从 `spi_bus_release` 契约，绝不直接篡改 `spi_bus.owner` 或 `depth`。

### 1.2 Task 2: 增加 CS 释放故障注入测试 (T01 ~ T08)
- 在 `tests/host/test_flash.c` 中完善 `mock_bus_cs_high()` 逻辑：当注入 `inject_cs_high_fail = 1` 时，模拟片选释放失败，引脚维持低电平（`cs_asserted = 1`），并返回 -1。
- 覆盖 8 项针对性测试：
  - **T01**: `ReadStatusReg` 结束时 CS 释放失败 $\rightarrow$ 返回 `ERROR_BUS`，且未提交状态结果。
  - **T02**: `Read` 数据结束时 CS 释放失败 $\rightarrow$ 禁止返回 `OK`（返回 `ERROR_BUS`）。
  - **T03**: `WriteEnable` 结束时 CS 释放失败 $\rightarrow$ 拦截后续操作，返回 `ERROR_BUS`。
  - **T04**: `Page Program` 结束时 CS 释放失败 $\rightarrow$ 不得报告成功，返回 `ERROR_BUS`。
  - **T05**: `Sector Erase` 结束时 CS 释放失败 $\rightarrow$ 不得报告成功，返回 `ERROR_BUS`。
  - **T06**: 故障后引脚状态检查 $\rightarrow$ 确认 `cs_asserted == 1`，未出现无声泄漏。
  - **T07**: 故障恢复后重新访问 $\rightarrow$ 状态正常时允许通信。
  - **T08**: 正常功能回归 $\rightarrow$ 原有读写擦除功能完全无影响。

### 1.3 Task 3: 增强非破坏性读取数据校验
- **问题分析**：原非破坏性安全模式仅检查 `W25QXX_Read(...) == W25QXX_OK` 返回值，没有深入校验读取到的数据内容是否在多次读取间保持一致。
- **增强方案**：
  1. 在 `BSP/Src/app_test.c` 中引入基准缓冲区读取机制（`ref_buf`）。
  2. 抢占测试（20kHz 中断）：10 轮 DMA 读取中，每轮读回的 256 字节均与基准数据执行 `memcmp` 完整比对。
  3. 长度矩阵测试：覆盖 1, 4, 8, 15, 16, 17, 32, 64, 256, 480 字节，逐项与基准前缀比对。
  4. 1,000 次连续读取：独立统计通信成功次数（`read_ok`）与数据一致次数（`data_match_ok`），新增 `data_mismatch`、`first_mismatch_offset` 与 `last_flash_error` 诊断字段。
  5. 明确数据判定语义：测试区域内容未知时，判定为“重复读取一致”，不妄称“实际内容绝对正确”。
  6. 校验已知设备 ID：验证 `g_flash.id == W25Q128` (0xEF17) 与 `capacity == 16MB`。

---

## 2. Host 单元测试验证 (tests/host/)

### 2.1 CTest 套件测试结果
```text
Test project /home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/tests/host/build
    Start 1: bus
1/3 Test #1: bus ..............................   Passed    0.02 sec
    Start 2: ll
2/3 Test #2: ll ...............................   Passed    0.01 sec
    Start 3: flash
3/3 Test #3: flash ............................   Passed    0.06 sec

100% tests passed, 0 tests failed out of 3
Total Test time (real) = 0.10 sec
```

### 2.2 `test_flash` 详细断言输出
- 原 Stage 1.2 断言：51 / 51 PASS。
- Stage 1.2-B 新增断言：21 项全通过（涵盖 T01~T08 CS 释放故障注入、1000 次数据一致性统计、数据篡改异常检测等）。
- **总断言数**：**72 / 72 PASS (100% 通过)**。

---

## 3. 固件构建与资源占用

使用 `arm-none-eabi-gcc 13.2.1` 构建：

| 固件版本 | Flash (text + data) | SRAM (data + bss) | 优化等级 | 编译器警告 |
|---|---:|---:|:---:|:---:|
| **Debug** | 40,032 B (61.08%) | 5,016 B (24.49%) | -O0 | 0 Warnings |
| **Release** | **29,812 B (45.49%)** | **5,008 B (24.45%)** | -Os | **0 Warnings** |

Flash 与 SRAM 资源均在 STM32F103C8T6（64KB Flash, 20KB SRAM）规格上限的安全范围之内。

---

## 4. HIL 真实硬件在板验证 (STM32F103C8T6 + W25Q128)

通过 OpenOCD 0.12.0 烧录 Debug 固件，目标板运行后由 GDB / SWD 直接读取导出的 RAM 结构体：

```gdb
$1 = {
  magic = 1414743380,                 /* 0x54534554 ('TEST') */
  ct_pass = 18,                       /* 18 项总线契约测试全部通过 */
  ct_total = 18, 
  ct_fail = 0, 
  rounds_ok = 10,                     /* 10 轮 20kHz 抢占测试全部通过 */
  rounds_total = 10, 
  len_ok = 10,                        /* 10 长度矩阵读校验全部通过 */
  len_count = 10, 
  str_ok = 1000, 
  str_total = 1000, 
  fl_ok = 0,                          /* 安全模式：未执行物理写入 */
  fl_total = 0, 
  fl_last_result = 0, 
  fl_fail_round = 0, 
  erase_count = 0,                    /* ⭐ 物理擦除次数严格为 0（零磨损零破坏） */
  rec_ok = 1,                         /* 异常恢复通过且数据校验一致 */
  ll_recover_count = 1, 
  ll_last_error = 0, 
  owner_null = 1,                     /* 总线归还：owner == NULL */
  depth_zero = 1,                     /* 嵌套归零：depth == 0 */
  pass = 1,                           /* ⭐ 综合判定 PASS */
  elapsed_ms = 115, 
  test_mode = 0,                      /* 0: SAFE 模式 */
  test_sector_addr = 1048576,         /* 候选安全测试扇区 1MB 偏移处 */
  rounds_fail_stage = 0, 
  rounds_fail_tx0 = 0, 
  rounds_fail_rx0 = 0, 
  read_ok = 1000,                     /* ⭐ 1000 次通信全部成功 */
  data_match_ok = 1000,               /* ⭐ 1000 次数据 100% 重复读取一致 */
  data_mismatch = 0,                  /* ⭐ 0 数据不一致 */
  first_mismatch_offset = 4294967295, /* 0xFFFFFFFF (无不一致发生) */
  last_flash_error = 0
}
$2 = {
  bus = 0x8009b6c <s_w25qxx_bus>, 
  id = 61207,                         /* 0xEF17 (W25Q128) 成功识别 */
  capacity = 16777216,                /* 16 MB 准确识别 */
  last_result = W25QXX_OK
}
```

---

## 5. 遗留问题与已知限制说明

1. **硬件破坏性擦写测试受控**:
   - 按照安全约束规范，在未获得用户显式确认的安全扇区前，禁止对物理 Flash 进行破坏性擦写测试。
   - 物理擦写与跨页跨扇区功能已在 Host Mock 单元测试中完成了 100% 覆盖。
2. **测试数据语义**:
   - 由于测试扇区（1MB 偏移处）内容属于未知用户数据，当前非破坏性测试证明了**数据在多轮读取、高频抢占与异常恢复下的强一致性**，不作“内容绝对符合某业务值”的过度断言。
