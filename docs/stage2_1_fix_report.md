# Stage 2.1-Fix：GC9A01 错误传播与验收收尾报告

## 一、任务背景与整改目标

在 Stage 2.1 完成 GC9A01 驱动规范化与分块发送的基础上，经代码审查与测试审计发现以下 3 项关键可靠性缺口：
1. **驱动全链路错误未完整传播**：部分高级绘图 API（如 `LCD_DrawLine`、`LCD_DrawRectangle`、`Draw_Circle`、`LCD_ShowFloatNum1` 等）忽略了内部底层绘图函数与字符绘制的返回值，发生底层 SPI 写入失败或 CS 释放失败时未立即中止，且容易误报成功。
2. **测试统计口径与初始化判断不严谨**：`app_test.c` 中 `tft_update_count` 在调用 `LCD_Fill` 后直接自增，未核实操作是否返回 `LCD_OK`；`GC9A01_PortInit()` 返回 `void`，导致上层无法感知初始化失败，存在屏幕失效时仍误报整体 PASS 的风险。
3. **分块测试名称与输入参数不匹配**：测试函数命名为 `test_65536b_chunking`，但实际传入的图像尺寸为 $240 \times 140$（67,200 字节），缺少对 65,536 B 精确边界以及合法国标尺寸 70,000 B 的精确断言与覆盖。

本轮 Stage 2.1-Fix 针对上述问题开展集中整改，严格执行“Host 优先、真机安全只读验证”原则，补齐全链路错误传播与专项故障注入测试（T01 ~ T08），并完成 HIL 真实硬件验收。

---

## 二、修改内容与技术实现

### 2.1 Task 1：补齐 GC9A01 全链路错误传播

涉及文件：
- `BSP/Src/gc9a01.c`
- `BSP/Inc/gc9a01.h`
- `BSP/Src/gc9a01_port.c`
- `BSP/Inc/gc9a01_port.h`

#### 详细改动点：
1. **`LCD_DrawLine`**：每次循环绘制像素点时，检查 `LCD_DrawPoint` 返回值，若非 `LCD_OK` 则立即中断循环并返回错误码。
2. **`LCD_DrawRectangle`**：依次绘制 4 条边线，每条边线调用 `LCD_DrawLine` 后均检查返回值，失败立即中止后续绘制并返回错误。
3. **`Draw_Circle`**：Bresenham 画圆算法中，8 个对称点的 `LCD_DrawPoint` 均加入返回值判断，失败立即退出并返回底层错误。
4. **`LCD_ShowString`**：逐字符绘制循环中，只要任一字符 `LCD_ShowChar` 返回非 `LCD_OK`，立即终止后续字符绘制并返回该错误码。
5. **`LCD_ShowIntNum`**：格式化高位补空格或数字字符绘制时，检查 `LCD_ShowChar` 返回值，中途失败立即返回。
6. **`LCD_ShowFloatNum1`**：符号位、整数部分、小数点和小数部分的所有子绘制调用全面检查返回值，发生错误立即返回，完整传播错误。
7. **`GC9A01_PortInit`**：修改函数签名使其返回 `LCD_Result`，透传 `GC9A01_Init(&g_lcd, &s_lcd_port)` 的初始化执行结果。
8. **CS 释放与错误保留**：所有含 CS 申请/释放的函数（如 `LCD_Fill`、`LCD_ShowChar`、`LCD_ShowPicture` 等）均确保在数据写入失败时释放 CS，且若写入已失败，则优先保留写入的原始错误码，避免后续清理操作覆盖根因。

### 2.2 Task 2：修复 TFT 测试统计与初始化结果判定

涉及文件：
- `BSP/Src/app_test.c`
- `Core/Src/main.c`

#### 详细改动点：
1. **`AppFlash_Test` 统计修复**：
   ```c
   /* 步骤 4: TFT 更新局部图形块（交替占用 SPI1） */
   LCD_Result fill_res = LCD_Fill(&g_lcd, 24 + (r % 10) * 19, 140 + (r / 10) * 16,
                                  24 + (r % 10) * 19 + 15, 140 + (r / 10) * 16 + 12, GREEN);
   if (fill_res == LCD_OK) {
       tft_update_count++;
   } else {
       break; /* 失败立即中止循环，保留失败上下文 */
   }
   ```
2. **`g_app_dual_result.pass` 综合判定增强**：
   严格要求 `id_ok_count == 20`、`dma256_ok_count == 20`、`dma480_ok_count == 20`、`tft_update_count == 20`、`post_read_ok == 20`、`owner == NULL`、`depth == 0`，任何一项不满足则整体判定为 FAIL。
3. **`main.c` 初始化防护**：
   在 `Core/Src/main.c` 中定义 `volatile LCD_Result g_tft_init_result`，捕获 `GC9A01_PortInit()` 返回值；仅在初始化成功（`g_tft_init_result == LCD_OK`）时才执行 `AppTFT_Test()`，杜绝初始化失败仍盲目跑测试的隐患。
4. **`AppTFT_StreamPattern` 错误处理**：
   逐行发送时检查底层写入结果，任何一行失败立即 `break` 退出循环；统一检查 CS 释放结果，若数据写入失败则返回写入错误码，若数据成功但释放失败则返回释放错误码。

### 2.3 Task 3：完善长度边界与专项故障注入测试

涉及文件：
- `tests/host/test_gc9a01.c`
- `tests/host/CMakeLists.txt`
- `BSP/Inc/gc9a01.h` / `BSP/Src/gc9a01.c`

#### 1. 修正测试长度：
- **65,280 B**：测试 $240 \times 136$ RGB565 图片（$240 \times 136 \times 2 = 65,280$ 字节 $\le 65,535$ 字节），断言单次 Port 写入。
- **65,536 B**：因 240×240 屏幕下任何能构成 32,768 像素的因数（均为 2 的整数次幂）必有至少一维 $\ge 256$ 从而超出屏幕边界，故在 `HOST_TEST` 编译模式下使用内部长数据分块测试辅助接口 `LCD_Writ_Bytes(&dev, buf, 65536)`，验证精确拆分为 $65,535 + 1$ 字节。
- **70,000 B**：采用合法国标尺寸 $200 \times 175$ RGB565 图像（$200 \times 175 \times 2 = 70,000$ 字节，未超 240×240），通过公开 API `LCD_ShowPicture` 验证精确拆分为 $65,535 + 4,465$ 字节。
- **115,200 B**：采用全屏 $240 \times 240$ RGB565 图像，通过 `LCD_ShowPicture` 验证精确拆分为 $65,535 + 49,665$ 字节。

#### 2. 新增专项故障注入单元测试（T01 ~ T08）：
- **T01** (`test_T01_draw_line_mid_fail`)：在 `LCD_DrawLine` 绘制过程中间注入 SPI 写入失败，断言返回 `LCD_ERROR_BUS`、中止后续点绘制、CS 安全释放。
- **T02** (`test_T02_draw_rectangle_mid_fail`)：在 `LCD_DrawRectangle` 第一条边线绘制时注入失败，断言返回 `LCD_ERROR_BUS`、剩余 3 条边线完全未绘制、CS 安全释放。
- **T03** (`test_T03_draw_circle_mid_fail`)：在 `Draw_Circle` 中途注入失败，断言返回 `LCD_ERROR_BUS`、画圆循环立即中断、CS 安全释放。
- **T04** (`test_T04_show_float_mid_fail`)：在 `LCD_ShowFloatNum1` 格式化输出过程中注入失败，断言错误码向上完整传播、CS 安全释放。
- **T05** (`test_T05_gc9a01_init_mid_fail`)：在 `GC9A01_Init` 面板寄存器配置序列中途注入失败，断言初始化立即中断、返回 `LCD_ERROR_BUS`、`last_error` 正确记录、CS 释放。
- **T06** (`test_T06_tft_partial_update_fail_stops_count`)：模拟 20 轮交替循环，第 6 轮注入 `LCD_Fill` 失败，断言 `tft_update_count` 严格保持为 5（不虚报 6 或 20），综合判定 `pass` 为 0。
- **T07** (`test_T07_stream_pattern_mid_fail`)：模拟全屏流式刷新，在第 10 行注入写入失败，断言总写入调用在第 17 次立即终止（未跑完全部 247 次），返回 `LCD_ERROR_BUS` 且 CS 安全释放。
- **T08** (`test_T08_normal_regression`)：全功能正常路径回归（Init、Fill、Point、Line、Rect、Circle、Char、String、Int、Float、Picture），断言全部返回 `LCD_OK` 且 `last_error == LCD_OK`。

---

## 三、测试验证与执行数据

### 3.1 Host 单元测试结果

运行命令：
```bash
cmake -S tests/host -B tests/host/build
cmake --build tests/host/build
ctest --test-dir tests/host/build --output-on-failure
```

测试执行结果：
```
Test project /home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/tests/host/build
    Start 1: bus
1/4 Test #1: bus ..............................   Passed    0.00 sec
    Start 2: ll
2/4 Test #2: ll ...............................   Passed    0.00 sec
    Start 3: flash
3/4 Test #3: flash ............................   Passed    0.02 sec
    Start 4: gc9a01
4/4 Test #4: gc9a01 ...........................   Passed    0.00 sec

100% tests passed, 0 tests failed out of 4
```

`test_gc9a01` 详细统计：
- **断言总数**：136 项断言
- **失败数量**：0 项失败
- **覆盖测试**：基础功能与边界防护测试 13 项 + 专项故障注入测试 T01~T08 共 21 个独立用例全部通过。

### 3.2 AddressSanitizer / UndefinedBehaviorSanitizer 检查

使用 `-fsanitize=address,undefined -g` 重新编译运行 Host 测试：
```
100% tests passed, 0 tests failed out of 4
Total Test time (real) = 0.09 sec
```
未检出任何内存越界、野指针访问、内存泄漏或未定义行为。

### 3.3 固件构建与资源占用统计

| 构建配置 | 编译器/构建工具 | Flash 使用量 | SRAM 使用量 | Flash 占比 | SRAM 占比 |
|---|---|---|---|---|---|
| **Debug** | GCC 10.3 / Ninja | 46,428 B | 6,776 B | 70.84% (64 KB) | 33.09% (20 KB) |
| **Release** | GCC 10.3 / Make | 32,136 B | 6,768 B | 49.04% (64 KB) | 33.05% (20 KB) |

编译过程无警告、无错误，资源使用率处于安全可控范围。

---

## 四、真机 HIL 联合验收结果

在真实硬件（STM32F103C8T6 + ST-Link V2 + GC9A01 TFT + W25Q128 Flash，SPI1 共享总线）上执行烧录运行，并通过 GDB-multiarch 联机截取运行时核心数据结构：

### 4.1 SWD 实测数据

```gdb
$1 = LCD_OK (g_tft_init_result)

$2 = (g_app_dual_result) {
  magic = 1146437964,       /* 0x4455414C ('DUAL') */
  rounds_ok = 20,
  rounds_total = 20,
  id_ok_count = 20,
  dma256_ok_count = 20,
  dma480_ok_count = 20,
  tft_update_count = 20,
  post_read_ok = 20,
  owner_null = 1,
  depth_zero = 1,
  pass = 1,
  elapsed_ms = 54
}

$3 = (g_app_test_result) {
  magic = 1414743380,       /* 0x54534554 ('TEST') */
  ct_pass = 18,
  ct_total = 18,
  ct_fail = 0,
  rounds_ok = 10,
  rounds_total = 10,
  len_ok = 10,
  len_count = 10,
  str_ok = 1000,
  str_total = 1000,
  fl_ok = 0,
  fl_total = 0,
  fl_last_result = 0,
  fl_fail_round = 0,
  erase_count = 0,          /* 严格保持零物理扇区擦除 */
  rec_ok = 1,
  ll_recover_count = 1,
  ll_last_error = 0,
  owner_null = 1,
  depth_zero = 1,
  pass = 1,
  elapsed_ms = 114,
  test_mode = 0,            /* 安全只读模式 */
  test_sector_addr = 1048576,
  rounds_fail_stage = 0,
  rounds_fail_tx0 = 0,
  rounds_fail_rx0 = 0,
  read_ok = 1000,
  data_match_ok = 1000,
  data_mismatch = 0,
  first_mismatch_offset = 4294967295,
  last_flash_error = 0,
  reference_valid = 3,
  expected_256_valid = 1
}

$4 = (g_flash) {
  bus = 0x800b458 <s_w25qxx_bus>,
  id = 61207,               /* 0xEF17 (W25Q128) */
  capacity = 16777216,      /* 16 MB */
  last_result = W25QXX_OK
}

$5 = (g_lcd) {
  port = 0x800b438 <s_lcd_port>,
  max_x = 240,
  max_y = 240,
  last_error = LCD_OK
}
```

### 4.2 观察与安全指标
1. **TFT 屏幕目检**：开机 8 色全屏图案正常刷出；自测信息界面清晰无花屏；20 轮交替方块平滑刷新；右下角心跳方块每 500ms 规则翻转。
2. **Flash 零破坏**：`erase_count == 0`，`test_mode == 0`，测试前后基准完全一致。
3. **总线释放完整**：`owner_null == 1`，`depth_zero == 1`，未发生任何总线死锁或重入泄漏。
4. **系统稳定性**：无 HardFault，无超时重试，无异常复位。

---

## 五、遗留项与后续建议

- **遗留项**：无。Stage 2.1-Fix 要求的 3 项缺陷整改、8 项故障注入测试、资源统计及 HIL 回归全部达成。
- **后续建议**：
  - Stage 2.1-Fix 交付物已完全符合阶段结项要求，可由用户审阅后合并至主干。
  - 下一阶段 Stage 2.2 可基于当前经受严格验证的 GC9A01 驱动与共享总线架构，进一步引入轻量 UI 控件库或字库优化。
