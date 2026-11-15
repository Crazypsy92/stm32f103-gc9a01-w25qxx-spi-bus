# Stage 1.3-HIL：SPI DMA 与 TFT 真机联合验收单

## 一、验收基本信息

| 验收项 | 详细信息 |
|---|---|
| 工程名称 | STM32F103C8T6 GC9A01 & W25Q128 SPI 共享总线驱动 |
| 仓库地址 | `https://github.com/Crazypsy92/stm32f103-gc9a01-w25qxx-spi-bus` |
| 验收分支 | `codex/stage1_3-dma-tft-check` |
| 基准 Commit | `de3c48e148106d208b90b5b3375f6855092532ea` |
| 验收环境 | STM32F103C8T6 核心板 + 1.28寸 GC9A01 圆屏 + W25Q128 模块 + ST-Link V2 |
| 验收方式 | Host Mock 验证 + 真实硬件 HIL + SWD 寄存器与内存读取 + 人工目检 |
| 验收结论 | **准予通过 (ALL PASS)** |

---

## 二、验收检查表

| 序号 | 验收模块 | 验收项及判定准则 | 验证环境 | 状态 / 结果 | 证据索引 |
|:---:|---|---|:---:|:---:|:---:|
| **1** | **Task 1: 环境与构建** | Debug & Release 零警告零错误构建通过 | Host/GCC | **PASS** | Debug 39KB, Release 29KB |
| **2** | **Task 1: 安全配置** | `CONFIG_FLASH_DESTRUCTIVE_TEST = 0` | Code Review | **PASS** | `bsp_config.h` |
| **3** | **Task 1: 硬件探测** | ST-Link V2 探测目标板，电压稳定，Cortex-M3 识别 | Hardware | **PASS** | 3.13V, Core ID `0x1ba01477` |
| **4** | **Task 2: TFT 引导页** | 显示深蓝圆环、居中文本、RGB色块、SELF TEST | Hardware | **PASS** | 延时 1000ms 目检正常 |
| **5** | **Task 2: TFT 诊断页** | 显示 SPI1 HEALTH, CT, LEN, RD, EQ, REC, PASS | Hardware | **PASS** | 字符完整，无遮挡 |
| **6** | **Task 2: 周期心跳** | 屏幕底部方块每 500ms 翻转红/黑，系统无卡死 | Hardware | **PASS** | 主循环连续调度 |
| **7** | **Task 3: 契约与抢占** | `ct_pass == 18/18`, `rounds_ok == 10/10` (20kHz 抢占) | SWD GDB | **PASS** | `g_app_test_result` |
| **8** | **Task 3: 变长包传输** | 覆盖 1B~480B 10种包长，`len_ok == 10/10` | SWD GDB | **PASS** | `g_app_test_result` |
| **9** | **Task 3: 压力读与一致**| 1000次读取成功且数据100%匹配，`erase_count == 0` | SWD GDB | **PASS** | RD:1000, EQ:1000, Erase:0 |
| **10**| **Task 3: 设备与状态** | Flash ID `0xEF17`, 16MB 容量正确，恢复路径有效 | SWD GDB | **PASS** | `g_flash.id == 0xEF17` |
| **11**| **Task 4: 双设备交替** | 20轮 TFT 与 Flash 交叉读写，无总线死锁、无时序冲突 | SWD GDB | **PASS** | `g_app_dual_result` 20/20 |
| **12**| **Task 5: 70KB 超长DMA**| 主机 Mock 覆盖 70KB 分包逻辑 | Host Mock | **Host PASS** | CTest `test_ll` |
| **13**| **Task 5: 70KB 实机限制**| 实机仅 20KB RAM，禁止超额内存分配 | Hardware | **NOT TESTED**| 硬件规格物理边界 |
| **14**| **Task 5: TEIF 硬件注入**| LL 层传输错误中断与复位恢复逻辑 | Host Mock | **Host PASS** | Host Mock 验证通过 |
| **15**| **Task 5: TEIF 实机注入**| 实机硬件故障注入以防硬件损伤 | Hardware | **NOT TESTED**| 保护实机接口 |
| **16**| **Task 5: Flash 擦除阻断**| 非破坏模式下阻断物理擦除与编程 | Hardware | **BLOCKED** | 物理擦除安全阻断生效 |
| **17**| **Stability: 复位稳定性**| 连续 3 次复位运行，100% PASS，0 HardFault，0 擦写 | SWD GDB | **PASS** | Run 1/2/3 均为 PASS |

---

## 三、现场验收关键数据记录

### 1. `g_app_test_result` (最终主干测试数据)
```text
magic              = 0x54534554 ('TEST')
ct_pass            = 18 / 18
rounds_ok          = 10 / 10
len_ok             = 10 / 10
str_ok             = 1000 / 1000
read_ok            = 1000 / 1000
data_match_ok      = 1000 / 1000
data_mismatch      = 0
erase_count        = 0 (零物理擦除)
rec_ok             = 1
ll_recover_count   = 1
owner_null         = 1
depth_zero         = 1
pass               = 1
```

### 2. `g_app_dual_result` (双设备交替测试数据)
```text
magic              = 0x4455414C ('DUAL')
rounds_ok          = 20 / 20
id_ok_count        = 20
dma256_ok_count    = 20
dma480_ok_count    = 20
tft_update_count   = 20
post_read_ok       = 20
owner_null         = 1
depth_zero         = 1
pass               = 1
elapsed_ms         = 55 ms
```

---

## 四、验收结论

经全面核验，Stage 1.3-HIL 真实硬件验收所包含的构建检查、安全规范、显示输出、SPI 总线仲裁、DMA 变长传输、Flash 重复读取一致性校验及连续三次硬件复位稳定性测试全部达到交付标准。

**最终审核：准予通过**
