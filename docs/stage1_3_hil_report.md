# Stage 1.3-HIL：SPI DMA 与 TFT 真机联合验收测试报告

## 一、测试基本信息

| 项目 | 内容 |
|---|---|
| 仓库地址 | `https://github.com/Crazypsy92/stm32f103-gc9a01-w25qxx-spi-bus` |
| 测试分支 | `codex/stage1_3-dma-tft-check` |
| 基准 Commit | `de3c48e148106d208b90b5b3375f6855092532ea` |
| 目标 MCU | STM32F103C8T6 (64 KB Flash, 20 KB SRAM) |
| 显示屏 | GC9A01 (240×240 圆形 IPS LCD, 16-bit RGB565) |
| 外部 Flash | W25Q128 (16 MB / 128 Mbit SPI NOR Flash) |
| 通信接口 | SPI1 共享总线 (TFT: 18 MHz Mode 0; Flash: 9 MHz Mode 0) |
| 调试工具 | ST-Link V2 (SWD 1000 kHz) + OpenOCD 0.12.0 + gdb-multiarch |
| 测试模式 | 默认非破坏性安全模式 (`CONFIG_FLASH_DESTRUCTIVE_TEST = 0`) |
| 测试时间 | 2026-11-15 |

---

## 二、固件构建与资源占用

代码经 `arm-none-eabi-gcc` (CMake + Ninja) 编译，零警告、零错误：

| 构建配置 | Flash 占用 | Flash 占比 (64KB) | RAM 占用 | RAM 占比 (20KB) | 编译告警 |
|---|---|---|---|---|---|
| **Debug** | 39,588 字节 | 60.57% | 6,288 字节 | 30.70% | 0 |
| **Release** | 29,484 字节 | 45.15% | 6,280 字节 | 30.66% | 0 |

---

## 三、测试分层与验收结果分类说明

根据测试规范，严格区分四类测试结果：

1. **Host PASS**：主机仿真/Mock 环境下验证通过的项目（含 70KB 超长分包传输、TEIF 故障注入、CS 释放错误拦截等）。
2. **HIL Non-destructive PASS**：实机无损验证通过的项目（含 TFT 显示、Flash 读校验、DMA 传输、双设备共享总线并发）。
3. **NOT TESTED (Hardware Boundary)**：受限于物理资源或安全考量，未在实机执行的项目（70KB 内存超限、硬件级 TEIF 注入）。
4. **BLOCKED (Safety Policy)**：受安全配置保护阻断执行的项目（外部 Flash 物理扇区擦除和改写）。

---

## 四、测试任务执行详情

### Task 1：代码与硬件环境检查

- **Git 分支与状态**：位于 `codex/stage1_3-dma-tft-check`，基准 Commit 与远程对齐。
- **安全配置确认**：`bsp_config.h` 中 `CONFIG_FLASH_DESTRUCTIVE_TEST = 0`，全路径杜绝外部 Flash 擦写。
- **硬件连接**：
  * ST-Link V2 探测目标板电压 3.13V，SWD 接口稳定识别 `Cortex-M3 r1p1`。
  * SPI1 引脚：PA5 (SCK), PA7 (MOSI), PA6 (MISO)。
  * 片选与控制引脚：PA4 (TFT_CS), PA2 (TFT_DC), PA1 (TFT_RES), PA3 (TFT_BLK), PB0 (FLASH_CS)。

### Task 2：TFT 图形显示验收

- **引导页 (Splash Screen)**：
  * 显示外圈深蓝圆环 (R=113)。
  * 文本居中呈现：`SPI1 LAB`、`STM32F103C8`、`GC9A01 W25Q128`、`DMA + FLASH`。
  * RGB 三基色校准条各 30×8 px，色彩饱满无断线。
  * 底部显示 `SELF TEST`，延时 1000ms 供人工观测。
- **双设备测试页**：
  * 显示 `DUAL BUS TEST`，动态绘制 20 个绿色色块反馈交替传输进度。
- **最终诊断页 (SPI1 HEALTH)**：
  * 显示关键运行指标：`SPI1 HEALTH`、`W25Q128 16M`、`CT:18/18 IRQ:OK`、`LEN:10/10`、`RD:1000/1000`、`EQ:1000/1000`、`REC:OK REF:OK`、`SAFE ERASE:0`、`PASS`。
  * 字符完全处于圆形屏可视范围内，无边缘裁剪。
- **周期心跳**：
  * 底部方块 (108..132, 222..234) 每 500ms 翻转红/黑，系统无假死，主循环调度正常。

### Task 3：SPI DMA 与 Flash 数据读取实机验证 (SWD 提取)

通过 OpenOCD + GDB 读取目标板 `g_app_test_result`、`g_flash` 与 LL 层变量：

| 校验字段 | 期望值 | 实机测量值 | 判定 |
|---|---|---|---|
| `magic` | `0x54534554` ('TEST') | `0x54534554` | **PASS** |
| `ct_pass / ct_total` | 18 / 18 (契约检查) | 18 / 18 | **PASS** |
| `rounds_ok / rounds_total` | 10 / 10 (20kHz 抢占) | 10 / 10 | **PASS** |
| `len_ok / len_count` | 10 / 10 (变长包) | 10 / 10 | **PASS** |
| `str_ok / str_total` | 1000 / 1000 (压力读取) | 1000 / 1000 | **PASS** |
| `read_ok` | 1000 次接口成功 | 1000 | **PASS** |
| `data_match_ok` | 1000 次数据一致 | 1000 | **PASS** |
| `data_mismatch` | 0 次数据错误 | 0 | **PASS** |
| `erase_count` | 严格为 0 (零擦除) | 0 | **PASS** |
| `rec_ok` | 1 (恢复路径有效) | 1 | **PASS** |
| `reference_valid` | 0x03 (基准有效) | 3 | **PASS** |
| `owner_null` | 1 (总线所有权释放) | 1 | **PASS** |
| `depth_zero` | 1 (重入深度归零) | 1 | **PASS** |
| `pass` | 1 | 1 | **PASS** |
| `g_flash.id` | `0xEF17` (W25Q128) | `0xEF17` | **PASS** |
| `g_flash.capacity` | 16,777,216 字节 | 16,777,216 | **PASS** |
| `g_ll_recover_count` | $\ge 1$ | 1 | **PASS** |
| `g_ll_last_error` | 0 | 0 | **PASS** |

### Task 4：SPI 双设备交替访问测试 (20 轮全并发)

在 `AppFlash_Test` 中执行 20 轮 TFT 与 Flash 交叉总线仲裁：
1. Flash 读 ID (0xEF17)。
2. Flash DMA 读 256B 并校验。
3. Flash DMA 读 480B 并校验。
4. TFT `LCD_Fill` 更新局部图形块（交替占用总线）。
5. Flash 再次读取 256B 并校验。

SWD 读取 `g_app_dual_result` 实机结果：
- `magic`: `0x4455414C` ('DUAL')
- `rounds_ok`: 20 / 20
- `id_ok_count`: 20
- `dma256_ok_count`: 20
- `dma480_ok_count`: 20
- `tft_update_count`: 20
- `post_read_ok`: 20
- `owner_null`: 1, `depth_zero`: 1
- `pass`: 1
- `elapsed_ms`: 55 ms
- 判定：**PASS (总线仲裁无冲突、无死锁、无时序竞争)**。

### Task 5：DMA 与恢复边界核验

| 测试项目 | 测试环境 | 结果状态 | 详细说明 |
|---|---|---|---|
| **70KB 超长 DMA 分包** | Host Mock | **Host PASS** | 主机 Mock 完整覆盖 70KB 分包逻辑与分段传输 |
| **70KB 超长 DMA 分包** | 实机 HIL | **NOT TESTED** | STM32F103C8T6 物理 SRAM 仅 20KB，禁止在实机分配超额内存 |
| **LL TEIF 硬件故障注入** | Host Mock | **Host PASS** | 模拟 DMA 传输错误中断触发，验证总线复位与恢复 |
| **LL TEIF 硬件故障注入** | 实机 HIL | **NOT TESTED** | 避免对实机外设产生未受控的硬件总线损伤 |
| **外部 Flash 擦除/编程** | 实机 HIL | **BLOCKED** | 安全只读保护生效，实机物理擦除次数严格保持为 0 |

---

## 五、连续三次复位稳定性测试

执行 3 次独立硬件复位与重新运行，由 GDB 抓取每次执行结果：

| 测试运行轮次 | 运行状态 | `ct_pass` | `rounds_ok` | `data_match_ok` | `dual_rounds` | `erase_count` | `pass` |
|---|---|---|---|---|---|---|---|
| **Run 1 (冷启动/复位)** | 正常完成 | 18/18 | 10/10 | 1000/1000 | 20/20 | 0 | 1 (PASS) |
| **Run 2 (热复位 1)** | 正常完成 | 18/18 | 10/10 | 1000/1000 | 20/20 | 0 | 1 (PASS) |
| **Run 3 (热复位 2)** | 正常完成 | 18/18 | 10/10 | 1000/1000 | 20/20 | 0 | 1 (PASS) |

**稳定性统计**：
- 连续 3 次复位成功率：100% (3/3)。
- HardFault 发生次数：0 次。
- 总线泄漏或残留持有：0 次 (`owner == NULL`, `depth == 0`)。
- 数据校验不匹配次数：0 次。
- 物理擦除发生次数：0 次。

---

## 六、证据文件索引

- GDB SWD 实时提取日志：[gdb_hil_results.txt](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/evidence/stage1_3_hil/gdb_hil_results.txt)
- 屏幕布局与显示说明：[tft_screen_layout.txt](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/evidence/stage1_3_hil/tft_screen_layout.txt)
- 测试代码实现：[app_test.c](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/BSP/Src/app_test.c)

---

## 七、结论

Stage 1.3-HIL 真实硬件验收全部项目测试通过。SPI1 共享总线在 TFT (18MHz) 与 W25Q128 (9MHz) 之间调度可靠，DMA 传输及状态恢复机制运行健全，Flash 处于零磨损安全状态。
