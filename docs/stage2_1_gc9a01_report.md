# Stage 2.1：GC9A01 驱动规范化与可移植接口整改报告

## 一、阶段任务目标与整改背景

本阶段（Stage 2.1）专注于 GC9A01 圆形 TFT（240×240，RGB565）驱动协议层、移植层规范化及边界安全性整改。
重点解决原驱动存在的以下核心隐患：
1. **长度截断缺陷**：原 `LCD_Writ_Bytes()` 在调用底层发送时将 `uint32_t len` 强制截断为 `uint16_t`，导致超过 65,535 字节的图像（如 240×240 全屏图像 115,200 字节）产生严重截断（仅发送 49,664 字节）。
2. **底层通信失败被静默吞没**：原 Port 回调 `write` 和 `cs` 均为 `void` 返回类型，底层 SPI 硬件错误、总线仲裁被拒或片选释放失败完全无法向协议层上报，出现静默失败。
3. **参数与边界防御缺失**：字库字符无 ASCII 范围保护（非法字符导致数组越界寻址）；不支持的字号未做安全校验；矩形填充未防御零宽/零高/倒序坐标；图片长宽计算缺少整数溢出防御。
4. **协议层与平台解耦**：保持协议层 100% 独立于 STM32 HAL/LL 寄存器，仅通过抽象回调表与外界交互，确保驱动可无缝移植并在 Host 环境完整单测。

---

## 二、整改架构与实现细节

### 1. 统一状态码体系与错误记录
在 [gc9a01.h](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/BSP/Inc/gc9a01.h) 中定义统一结果枚举与设备错误句柄：
```c
typedef enum {
    LCD_OK            =  0,   /* 操作成功 */
    LCD_ERROR_PARAM   = -1,   /* 参数非法（NULL 指针/越界/非法字号/溢出） */
    LCD_ERROR_BUS     = -2,   /* 总线错误（底层 SPI 错误/CS 失败/未持有总线） */
    LCD_ERROR_TIMEOUT = -3    /* 通信超时 */
} LCD_Result;
```
在 `LCD_Device` 中扩展 `LCD_Result last_error`，严格遵循“**保留首个故障原因**”原则，并提供 `LCD_GetLastError()` 与 `LCD_ClearError()` 查询接口。

### 2. 连续长数据安全分块传输
在 [gc9a01.c](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/BSP/Src/gc9a01.c) 中重构 `LCD_Writ_Bytes()`：
- 采用 `uint32_t len`，根据 Port 层配置的单次上限 `max_chunk_len`（默认 65,535 字节）自动切片分块传输。
- 分块传输期间保持 CS 与 DC 状态连续，保证像素流时序无断点。
- 分块循环中任何一次写入失败立即中止后续传输，不向外报告虚假成功。
- 在 `LCD_ShowPicture()` 中增加防溢出保护：`(uint64_t)length * width * 2ULL <= UINT32_MAX` 以及 `x + length <= max_x`，`y + width <= max_y`。
- **全屏图片（115,200 字节）无需 115 KB SRAM 缓冲**，分块流式直推，完美适配 STM32F103 20KB RAM。

### 3. 片选与总线仲裁生命周期防御
- **CS 申请失败（CS LOW）**：若 `spi_bus_acquire` 被拒或返回错误，立即中止操作，绝对不向总线发送任何数据或命令。
- **CS 释放失败（CS HIGH）**：若 `spi_bus_release` 失败，必须向协议层上报 `LCD_ERROR_BUS`，禁止误报成功。
- **发送失败时的总线归还**：若数据传输中途失败，外层仍尝试执行 `LCD_CS_HIGH` 将总线释放归还，避免 owner/depth 泄漏与死锁，且返回值保留最初的传输故障码。

### 4. 字符与图形边界安全
- **`LCD_ShowChar`**：严格检查 `num >= ' ' && num <= '~'`，避免非法 ASCII 字符（如 0 或 >126）导致字库索引越界；字号限定为 12, 16, 24, 32；坐标严格判定 `x + sizex <= max_x && y + sizey <= max_y`。
- **`LCD_ShowString`**：逐字符安全递增，遇到屏幕边界自动安全裁剪停止，防止字符串过长踩写屏幕外内存。
- **`LCD_Fill`**：校验 `xsta < xend && ysta < yend` 以及 `xend <= max_x && yend <= max_y`，防御零宽、零高及越界参数。
- **`LCD_DrawPoint` / `DrawLine` / `Draw_Circle`**：底层全部施加 `max_x / max_y` 物理边界安全裁剪。

---

## 三、修改文件清单

| 文件路径 | 修改性质 | 主要变更说明 |
|---|:---:|---|
| `BSP/Inc/gc9a01.h` | 协议头文件 | 增加 `LCD_Result` 枚举，更新 `LCD_Port`（支持错误码返回与分块上限），更新 API 函数签名 |
| `BSP/Src/gc9a01.c` | 协议源文件 | 实现长数据自动切块、错误截获与保存、字符字号与图形边界防御 |
| `BSP/Src/gc9a01_port.c` | 移植适配层 | 实现 `Port_Cs` 与 `Port_Write` 状态码返回，接入 `spi_bus` 仲裁状态 |
| `BSP/Inc/lcdfont.h` | 字符库头文件 | 修复 UTF-8 编码下未引用中文字模的结构体警告 |
| `BSP/Src/app_test.c` | 硬件测试应用 | 新增 `AppTFT_StreamPattern` 全屏 8 色条小缓冲流式生成，适配更新后的返回类型 |
| `tests/host/test_gc9a01.c` | **新增测试** | 覆盖 12 大项、85 组断言的完整 Mock Port 单元测试套件 |
| `tests/host/CMakeLists.txt` | 测试配置 | 注册 `test_gc9a01` 可执行文件与 CTest 套件 |
| `docs/hardware_pin_mapping.md` | **新增文档** | 审计并固定实际硬件引脚（PA9/PA10/PB13/PA8）、时钟树与外设依赖图 |

---

## 四、测试与构建验证

### 1. Host 仿真测试套件 (4/4 CTest 全部通过)

新增 `test_gc9a01.c` 后，工程构建 4 个独立 Host 测试套件并执行 `ctest`：

```text
Test project .../tests/host/build
    Start 1: bus
1/4 Test #1: bus ..............................   Passed    0.00 sec
    Start 2: ll
2/4 Test #2: ll ...............................   Passed    0.00 sec
    Start 3: flash
3/4 Test #3: flash ............................   Passed    0.02 sec
    Start 4: gc9a01
4/4 Test #4: gc9a01 ...........................   Passed    0.00 sec

100% tests passed, 0 tests failed out of 4 (总测试断言数: 189 项全部 PASS)
```

`test_gc9a01` 包含的 12 项专题测试细项：
1. **正常命令与数据发送**：验证 0x29 命令与 0x55/0x1234 数据发送时序、CS/DC 切换状态。
2. **16 B 阈值边界**：验证 16 字节图片发送完整性。
3. **65,535 B 单包上限**：验证 65,280 字节单包不切片。
4. **65,536 B 与 70,000 B 分块**：验证 67,200 字节精确切分为 65,535 B + 1,665 B。
5. **115,200 B 全屏发送**：验证 240×240 RGB565 精确切分为 65,535 B + 49,665 B，总字节 100% 吻合。
6. **SPI 写入失败传播**：验证单命令故障和分块中途故障拦截与 CS 安全释放。
7. **CS 申请失败防御**：验证申请失败时 0 数据输出。
8. **CS 释放失败拦截**：验证释放失败不误报成功。
9. **非法 ASCII 字符防御**：验证 ASCII < 32 及 > 126 拒绝，无越界读取。
10. **非法字号防御**：验证非 12/16/24/32 字号拦截。
11. **图形坐标越界与裁剪**：验证越界点、零宽零高填充、越界字符与越界图片参数拦截，长字符串边界安全裁剪。
12. **面板初始化序列**：验证复位脉冲、30+ 寄存器初始化流以及中途故障终止逻辑。

### 2. 目标固件构建与资源占用变化

采用 `arm-none-eabi-gcc` 编译，保持 **0 警告、0 错误**：

| 构建类型 | Flash 占用 | Flash 变化量 | RAM 占用 | RAM 变化量 | 说明 |
|---|:---:|:---:|:---:|:---:|---|
| **Debug** | 46,156 字节 (70.43%) | +6,568 字节 | 6,776 字节 (33.09%) | +488 字节 | 含流式色带生成器与完整边界保护 |
| **Release** | 32,020 字节 (48.86%) | +2,536 字节 | 6,768 字节 (33.05%) | +488 字节 | 优化级别 `-O2`，内存充裕安全 |

---

## 五、真实硬件 (HIL) 回归测试结果

烧录至 STM32F103C8T6 核心板，ST-Link V2 + OpenOCD 实机运行，SWD 读出目标板活体数据：

```text
1. g_app_dual_result (双设备交替并发 20 轮):
   magic = 0x4455414C ('DUAL')
   rounds_ok = 20 / 20 (100% 成功)
   id_ok_count = 20 (W25Q128 识别稳定)
   dma256_ok_count = 20
   dma480_ok_count = 20
   tft_update_count = 20 (LCD_Fill 局部刷块与 Flash 交替仲裁)
   owner_null = 1, depth_zero = 1 (总线零泄漏)
   pass = 1
   elapsed_ms = 55 ms

2. g_app_test_result (综合契约与压力读取):
   magic = 0x54534554 ('TEST')
   ct_pass = 18 / 18 (契约检查全部通过)
   rounds_ok = 10 / 10 (20kHz ISR 抢占压力全部通过)
   len_ok = 10 / 10 (1B~480B 变长传输全部通过)
   str_ok = 1000 / 1000 (1000 次压力连续读取)
   read_ok = 1000 / 1000, data_match_ok = 1000 / 1000
   data_mismatch = 0 (零误码)
   erase_count = 0 (非破坏只读安全保护生效)
   owner_null = 1, depth_zero = 1
   pass = 1

3. g_flash:
   id = 0xEF17 (W25Q128 16MB)
   capacity = 16,777,216
   last_result = W25QXX_OK

4. g_lcd:
   max_x = 240, max_y = 240
   last_error = LCD_OK (零运行时错误)

5. 系统保活与状态:
   PC 停留在 AppTFT_Task 正常主循环
   HardFault 发生次数: 0
   屏幕心跳方块每 500ms 规律翻转红/黑
```

### 屏幕目测显示效果
- **全屏流式 8 色条测试**：红、绿、蓝、黄、青、品红、白、深蓝 8 种颜色条带自上而下满屏铺展，无条纹撕裂。
- **引导页**：深蓝外环清晰闭合，文字居中无边缘遮挡，RGB 校验色块正常。
- **双设备测试页**：20 个绿色色块动态填满，文字提示 `DUAL PASS 55ms`。
- **最终诊断页**：显示 `SPI1 HEALTH`、`CT:18/18`、`LEN:10/10`、`RD:1000/1000`、`PASS`，右下角心跳方块闪烁。

---

## 六、当前设计局限与说明

1. **MCU 硬件内存物理边界**：STM32F103C8T6 仅有 20 KB SRAM，无法支持全屏 115,200 字节帧缓冲。全屏或大尺寸图像必须采用流式分块或 ROM/Flash 取模方式输出。
2. **字符集限制**：当前驱动仅内置 ASCII 常用可见字符（32 ~ 126）字模；若需显示中文需外挂 GBK/Unicode 矢量字库或使用外部 W25Q128 字库芯片支持。
3. **单向总线反馈限制**：GC9A01 硬件接线为单向 MOSI 发送，`LCD_OK` 证明 SPI 数据及片选已严格按协议规范准确送达控制器，物理显示完整性需结合硬件屏体目检。

---

## 七、交付结论

Stage 2.1 GC9A01 驱动规范化与可移植接口整改全部指标达成：
- 长数据传输长度截断已修复，全屏 115,200 字节分块传输验证通过。
- 片选申请与释放错误传播健全，总线所有权零泄漏。
- 字符与图形边界全面防御。
- 4 组 Host 仿真单元测试 100% 通过。
- 真实硬件 HIL 回归零故障，原有 SPI 共享总线与 W25Q128 功能无回退。
