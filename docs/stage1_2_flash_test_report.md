# Stage 1.2: W25Q128 Flash 数据可靠性与安全保护测试报告

> **测试日期**: 2026-11-15  
> **测试环境**: Host Mock (Ubuntu x86_64, GCC, CTest) + HIL 真实硬件 (STM32F103C8T6, ST-Link v2, OpenOCD 0.12.0)  
> **测试固件**: Debug / Release (arm-none-eabi-gcc 13.2.1)  

---

## 1. 测试体系概览

Stage 1.2 构建了双重验证闭环：
1. **Host 离线 Mock 单元测试 (`tests/host/test_flash.c`)**：
   - 纯 C 内存模拟 16 MB W25Q128 Flash 存储体与 SPI 协议状态机。
   - 注入地址越界、24 位溢出回绕、未对齐擦除、WEL 锁存失效、CS 申请拒绝、单字节 Swap 故障、WaitBusy 超时与状态读错等边界。
2. **HIL 在板硬件测试 (`Core/Src/main.c`, `BSP/Src/app_test.c`)**：
   - 默认模式：全面执行非破坏性安全测试，验证 ID、容量识别、256B 只读自测、18 项契约、20kHz 抢占压测、1000 次事务压力与统一恢复，**物理擦除次数严格保持为 0**。
   - 通过 SWD 直接读取导出的结构体 `g_app_test_result` 进行断言校验。

---

## 2. Host Mock 单元测试结果

### 2.1 CTest 执行汇总

```text
Test project /home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/tests/host/build
    Start 1: bus
1/3 Test #1: bus ..............................   Passed    0.01 sec
    Start 2: ll
2/3 Test #2: ll ...............................   Passed    0.01 sec
    Start 3: flash
3/3 Test #3: flash ............................   Passed    0.05 sec

100% tests passed, 0 tests failed out of 3
```

### 2.2 `test_flash` 用例明细 (51 项断言全通过)

| 测试场景 | 注入与验证内容 | 预期结果 | 实际结果 |
|---|---|:---:|:---:|
| **未识别设备保护** | `capacity == 0` 时调用 Read、Write、EraseSector | 返回 `ERROR_PARAM` | **PASS** |
| **设备初始化** | 绑定总线并识别 W25Q128 (0xEF17) 16MB | `init_ok=1, cap=16MB` | **PASS** |
| **非法入参检查** | `pBuffer == NULL` 或 `len == 0` 时调用 Read / Write | 返回 `ERROR_PARAM` | **PASS** |
| **起始地址越界** | `addr == capacity` 及 `addr > capacity` 时读写 | 返回 `ERROR_PARAM` | **PASS** |
| **末尾边界保护** | 访问最后一个有效字节（`capacity - 1`）允许；`addr + len > capacity` 拒绝 | 边界拒绝并返回 `ERROR_PARAM` | **PASS** |
| **24位地址回绕** | `addr = 0x00FFFFFF, len = 256` 跨入 16MB 外空间 | 阻断并返回 `ERROR_PARAM` | **PASS** |
| **32位整数溢出** | `addr = 0xFFFFFFFF, len = 10` 触发无符号加法回绕 | 阻断并返回 `ERROR_PARAM` | **PASS** |
| **扇区对齐强制检查** | 地址为 1、4095、4097（非 4KB 对齐）擦除拒绝；4096 及末尾扇区允许 | 返回 `ERROR_PARAM` / `OK` | **PASS** |
| **WEL 硬件失效拦截** | 模拟写使能后 WEL 位始终为 0，调用 Write / Erase | 返回 `ERROR_WEL` 且物理命令零下发 | **PASS** |
| **CS 申请失败传播** | 模拟 `cs_low` 返回 -1（总线繁忙或被抢占） | 返回 `ERROR_BUS` | **PASS** |
| **SPI Swap 故障传播** | 模拟单字节传输时底层返回 -1 | 返回 `ERROR_BUS` 且安全拉高 CS | **PASS** |
| **WaitBusy 错误误判防御**| 模拟读状态寄存器时总线故障 | 立即返回 `ERROR_BUS`，不误判完成 | **PASS** |
| **WaitBusy 超时检测** | 模拟 BUSY 位长期置 1 不清除 | 达到预算返回 `ERROR_TIMEOUT` | **PASS** |
| **跨页写入一致性** | 跨越 256 字节页边界写入 300 字节，回读比对 | 逐字节 100% 匹配 | **PASS** |
| **跨扇区擦写一致性** | 跨越 4KB 扇区边界擦写 64 字节，检查擦除计数与数据 | 精确擦除 2 个扇区且数据完全一致 | **PASS** |

---

## 3. HIL 真实硬件在板验证结果

### 3.1 测试环境
- **MCU**: STM32F103C8T6 (72 MHz, 3.13V 供电)
- **Flash 芯片**: Winbond W25Q128 (16 MB, SPI1, CS=PA8)
- **LCD 屏幕**: GC9A01 (240×240, SPI1, CS=PA9)
- **工作模式**: 默认安全模式 (`CONFIG_FLASH_DESTRUCTIVE_TEST = 0`)

### 3.2 SWD 内存实测数据 (`g_app_test_result`)

```gdb
$1 = {
  magic = 1414743380,          /* 0x54534554 ('TEST') */
  ct_pass = 18,                /* 18 项总线契约测试全部通过 */
  ct_total = 18, 
  ct_fail = 0,                 /* 契约失败位图全 0 */
  rounds_ok = 10,              /* 10 轮并发抢占测试全部通过 */
  rounds_total = 10, 
  len_ok = 10,                 /* 10 长度矩阵读校验全部通过 */
  len_count = 10, 
  str_ok = 1000,               /* 1000 次高频 SPI 事务压力全部通过 */
  str_total = 1000, 
  fl_ok = 0,                   /* 安全模式：未执行破坏性写入 */
  fl_total = 0, 
  fl_last_result = 0, 
  fl_fail_round = 0, 
  erase_count = 0,             /* ⭐ 物理擦除次数严格为 0（零磨损零破坏） */
  rec_ok = 1,                  /* 异常恢复验证成功 */
  ll_recover_count = 1,        /* LL 恢复接口调用正常 */
  ll_last_error = 0, 
  owner_null = 1,              /* 总线归还：owner == NULL */
  depth_zero = 1,              /* 嵌套归零：depth == 0 */
  pass = 1,                    /* ⭐ 综合判定：1 (PASS) */
  elapsed_ms = 114,            /* 非破坏性安全测试仅耗时 114 ms */
  test_mode = 0,               /* 0: SAFE 模式 */
  test_sector_addr = 1048576,  /* 候选测试扇区 0x00100000 */
  rounds_fail_stage = 0, 
  rounds_fail_tx0 = 0, 
  rounds_fail_rx0 = 0
}
```

### 3.3 设备句柄实测 (`g_flash`)

```gdb
$2 = {
  bus = 0x8009960 <s_w25qxx_bus>, 
  id = 61207,                  /* 0xEF17 (W25Q128) */
  capacity = 16777216,         /* 16 MB (16,777,216 字节) */
  last_result = W25QXX_OK
}
```

---

## 4. 固件资源与 Footprint 变化统计

使用 `arm-none-eabi-size` 对比 Stage 1 与 Stage 1.2 的编译产物：

| 版本 | 配置 | Flash 占用 | SRAM 占用 | 优化等级 | 变动说明 |
|---|---|---:|---:|:---:|---|
| **Stage 1** | Debug | 40,768 B (62.2%) | 4,256 B (20.8%) | -O0 | 包含旧的 10 轮物理 Flash 擦写测试代码 |
| **Stage 1.2** | Debug | **39,508 B (60.3%)** | **4,008 B (19.6%)** | -O0 | 引入安全模式，减少了破坏性大静态缓冲区占用 |
| **Stage 1** | Release | 30,072 B (45.9%) | 4,248 B (20.7%) | -Os | Stage 1 发布版基线 |
| **Stage 1.2** | Release | **29,388 B (44.8%)** | **4,000 B (19.5%)** | -Os | **Stage 1.2 发布版**，Flash 占用降低 684 字节 |

---

## 5. 破坏性硬件测试受控状态说明

根据任务说明第四节：
> *“测试必须避免反复擦写未知用户数据。若未确认安全测试扇区，将破坏性硬件测试标记为 BLOCKED，不得擅自执行。”*

本报告郑重确认：
1. **非破坏性真机测试**: **PASS**（Flash 识别、256B DMA 读、18 项契约、20kHz 抢占纯读压测、1000 次事务压力全部通过，擦除次数严格为 0）。
2. **破坏性硬件擦写测试**: **CONTROLLED / BLOCKED**（已在 Host Mock 单元测试中通过 100% 跨页/跨扇区擦写验证；在真机环境下受安全模式保护，未获用户显式分配专用扇区前禁止擅自磨损物理 Flash）。
