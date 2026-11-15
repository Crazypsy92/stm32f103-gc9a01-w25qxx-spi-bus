# Stage 2.1-Fix：GC9A01 错误传播与验收收尾清单

## 一、验收信息

| 项目 | 内容 |
|---|---|
| GitHub 仓库 | https://github.com/Crazypsy92/stm32f103-gc9a01-w25qxx-spi-bus |
| 基准分支 | `codex/stage2_1-gc9a01-portability` |
| 交付分支 | `codex/stage2_1-fix` |
| 基准 Commit | `e78d21f16b97b807da2f1bc8d8d7540609996d4e` |
| MCU | STM32F103C8T6 |
| TFT | GC9A01，240×240，RGB565 |
| Flash | W25Q128，16 MB |
| 总线 | SPI1 共享总线（TFT 18 MHz / Flash 9 MHz） |
| 阶段状态 | **验收通过（READY FOR SIGN-OFF）** |

---

## 二、验收项目逐项核对

### 1. Task 1：GC9A01 全链路错误传播

| 编号 | 检查项目 | 验收标准 | 实施情况 | 状态 |
|---|---|---|---|:---:|
| 1.1 | `LCD_DrawLine` 错误传播 | 内部 `LCD_DrawPoint` 失败立即退出并返回底层错误码 | 每次像素点写入均核对返回值，失败立即退出 | **PASS** |
| 1.2 | `LCD_DrawRectangle` 错误传播 | 内部 `LCD_DrawLine` 失败立即中止后续边线绘制 | 4 条边线依次判断，失败立即中止 | **PASS** |
| 1.3 | `Draw_Circle` 错误传播 | 内部 8 点对称绘制失败立即退出，不误报成功 | 8 个点调用全部检查，失败退出 | **PASS** |
| 1.4 | `LCD_ShowFloatNum1` 错误传播 | 符号、整数、小数点、小数绘制失败完整传播 | 每一个子绘制调用均检查并返回非 OK 结果 | **PASS** |
| 1.5 | `LCD_ShowString` 失败中止 | 任一字符绘制失败立即中止，不继续处理后续字符 | 循环内字符失败立即返回并停止 | **PASS** |
| 1.6 | `LCD_ShowIntNum` 错误传播 | 数字或填充空格绘制失败立即返回 | 空格与数字字符失败均立即中止返回 | **PASS** |
| 1.7 | `GC9A01_PortInit` 初始化结果 | 函数修改为返回 `LCD_Result`，透传 `GC9A01_Init` 结果 | 函数签名更新为返回 `LCD_Result` | **PASS** |
| 1.8 | CS 释放与错误保留机制 | 清理时的 CS 释放错误不得覆盖数据写入的首个原始错误 | 优先返回数据写入错误，后行 CS 释放不覆盖根因 | **PASS** |
| 1.9 | `last_error` 机制 | 保持首个错误码记录机制，总线所有权维持规范 | 遵循 `LCD_SetError` 机制，不破坏 owner/depth | **PASS** |

### 2. Task 2：TFT 测试统计与初始化判断

| 编号 | 检查项目 | 验收标准 | 实施情况 | 状态 |
|---|---|---|---|:---:|
| 2.1 | TFT 局部更新成功计数 | 必须在 `res == LCD_OK` 时方可自增，失败立即退出 | 检查 `LCD_Fill` 返回值，仅 OK 自增，失败 `break` | **PASS** |
| 2.2 | `g_app_dual_result.pass` 判据 | 必须满足 20/20 全部 5 个子项且总线归零 | 严格核对 ID、DMA256、DMA480、TFT、PostRead 均为 20 | **PASS** |
| 2.3 | `main.c` 初始化安全防护 | 检查 `GC9A01_PortInit()` 返回值，失败禁止执行自测 | 声明 `g_tft_init_result`，仅在 OK 时执行 `AppTFT_Test` | **PASS** |
| 2.4 | `AppTFT_StreamPattern` 错误处理 | 逐行检查写入结果，检查 CS 释放，失败中止 | 逐行判断并中断，检查释放，保留首个错误码 | **PASS** |

### 3. Task 3：测试长度边界校正与专项故障注入 (T01 ~ T08)

| 编号 | 测试用例 | 目标场景 | 预期断言 | 实际结果 | 状态 |
|---|---|---|---|---|:---:|
| 3.1 | `test_65280b_boundary` | 65,280 B 边界 | 单次 Port 写入 (65,280 B) | 1 个数据分块写入 | **PASS** |
| 3.2 | `test_65536b_chunking` | 65,536 B 边界 | 拆分为 65,535 + 1 B | 2 块：65,535 B + 1 B | **PASS** |
| 3.3 | `test_70000b_chunking` | 70,000 B (200x175) | 拆分为 65,535 + 4,465 B | 2 块：65,535 B + 4,465 B | **PASS** |
| 3.4 | `test_115200b_fullscreen_send` | 115,200 B 全屏 | 拆分为 65,535 + 49,665 B | 2 块：65,535 B + 49,665 B | **PASS** |
| 3.5 | **T01** | `LCD_DrawLine` 中途 SPI 失败 | 返回 BUS 错误且停止绘制 | 写入第 12 次中止，CS 释放 | **PASS** |
| 3.6 | **T02** | `LCD_DrawRectangle` 中途失败 | 不继续后续边线绘制 | 写入第 10 次中止，剩余 3 线未画 | **PASS** |
| 3.7 | **T03** | `Draw_Circle` 中途失败 | 退出循环不误报成功 | 写入第 25 次中止，CS 释放 | **PASS** |
| 3.8 | **T04** | `LCD_ShowFloatNum1` 绘制失败 | 错误完整向上传播 | 写入第 15 次中止，返回 BUS 错误 | **PASS** |
| 3.9 | **T05** | `GC9A01_Init` 中途失败 | 初始化失败清晰可见 | 写入第 10 次中止，返回 BUS 错误 | **PASS** |
| 3.10 | **T06** | TFT 局部刷新失败 | 成功计数不虚增 | 5 轮成功第 6 轮失败，计数严为 5 | **PASS** |
| 3.11 | **T07** | 流式全屏中途失败 | 中止并安全释放 CS | 第 10 行中止，总写 17 次，CS=HIGH | **PASS** |
| 3.12 | **T08** | 全功能正常路径回归 | 所有 API 均正常通过 | 11 项图文 API 全通，`last_error=OK` | **PASS** |

### 4. Task 4：构建与 HIL 实机验收

| 类别 | 检查指标 | 验收要求 | 实际测试数据 | 状态 |
|---|---|---|---|:---:|
| **Host 测试** | 4 大套件 | 全部 PASS | `test_bus`, `test_ll`, `test_flash`, `test_gc9a01` 全过 (136/136) | **PASS** |
| **内存安全** | ASan / UBSan | 无越界/无未定义行为 | 0 error, 0 leak, 0 UB | **PASS** |
| **Debug 固件** | Flash / SRAM | 不超 MCU 限制 | Flash: 46,428 B (70.84%), SRAM: 6,776 B (33.09%) | **PASS** |
| **Release 固件** | Flash / SRAM | 优化后资源更省 | Flash: 32,136 B (49.04%), SRAM: 6,768 B (33.05%) | **PASS** |
| **HIL TFT 初始化**| `g_tft_init_result` | 必须为 `LCD_OK` | 实测 `LCD_OK` | **PASS** |
| **HIL 双设备交替** | `g_app_dual_result` | 20 轮全部成功且 PASS=1 | `rounds=20, id=20, dma256=20, dma480=20, tft=20, post=20, pass=1` | **PASS** |
| **HIL Flash 安全** | `erase_count` | 严格零擦除 (`CONFIG=0`) | 实测 `erase_count == 0`，`test_mode == 0` | **PASS** |
| **HIL 总线释放** | `owner` & `depth` | 最终释放归零 | `owner_null == 1`，`depth_zero == 1` | **PASS** |
| **HIL 异常监控** | 系统异常 | 无 HardFault、心跳规则 | PC 处于主循环线程模式，心跳每 500ms 翻转 | **PASS** |

---

## 三、约束遵守检查

1. [x] **未修改硬件接口**：SPI1 引脚（PA5/SCK, PA6/MISO, PA7/MOSI, PA9/TFT_CS, PA10/TFT_RES, PB13/TFT_DC, PA8/FLASH_CS）完全保持不变。
2. [x] **未修改 SPI/DMA 底层架构**：未开发异步 DMA，未引入 FreeRTOS，未引入外部多实例。
3. [x] **无物理 Flash 磨损**：零物理扇区擦除，零非测试扇区写入。
4. [x] **无强制推送与破坏性 Git 操作**：未执行 `push --force`、`reset --hard`、`clean -fd`。
5. [x] **未自动合并 main**：保持在当前开发分支 `codex/stage2_1-fix`，等待用户确认。
6. [x] **阶段结项停止**：不自动进入 Stage 2.2。

---

## 四、验收结论

Stage 2.1-Fix 全链路错误传播、测试统计修复、长度边界校正及 8 项专项故障注入测试均已高标准完成。Host 单元测试、ASan/UBSan 检查、交叉编译构建及真实硬件 HIL 联机实测全部通过。

**结论：验收通过，准予结项！**
