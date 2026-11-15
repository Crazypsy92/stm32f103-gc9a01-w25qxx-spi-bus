# STM32F103 双 SPI 驱动与交互式 Demo 使用指南 (User Guide)

本文档面向首次接触本项目的开发者，指导如何搭建工具链、编译固件、烧录到目标板以及使用板载单按键体验全部交互功能。

---

## 一、开发环境准备

### 1. 必备工具链与软件 (Linux / Ubuntu 示例)

```bash
# 1. 安装 ARM GCC 交叉编译器与 GDB
sudo apt-get update
sudo apt-get install -y gcc-arm-none-eabi binutils-arm-none-eabi gdb-multiarch

# 2. 安装构建工具 CMake 与 Ninja
sudo apt-get install -y cmake ninja-build

# 3. 安装开源烧录与调试工具 OpenOCD
sudo apt-get install -y openocd
```

### 2. 工具链版本推荐

- **arm-none-eabi-gcc**: 10.3 及以上（开发验证环境为 14.2.1）
- **CMake**: 3.22 及以上
- **OpenOCD**: 0.11 及以上（开发验证环境为 0.12.0）

---

## 二、硬件接线与上电

详细接线表格与说明请参阅 [hardware_wiring.md](file:///home/mythepsy92/program/Cube_MX/Cmake/STM32F103C8T6/External_peripherals/UI/GC9A01/doc/hardware_wiring.md)。

**核心接线概览：**
- **GC9A01 屏幕 (SPI1)**: SCL→PA5, SDA→PA7, CS→PA9, RST→PA10, **DC→PA4**, VCC→3.3V, GND→GND
- **W25Q128 Flash (SPI2)**: CLK→PB13, DO→PB14, DI→PB15, CS→PA8, VCC→3.3V, GND→GND
- **用户按键**: KEY1→PB12 (另一端接 GND)
- **ST-Link 调试器**: SWDIO→PA13, SWCLK→PA14, 3.3V→3.3V, GND→GND

---

## 三、编译固件

工程内置标准的 `CMakePresets.json` 预设，支持快速配置与多构建模式切换：

### 1. 编译默认交互式 Demo (Debug 版)

```bash
# 配置工程
cmake --preset dual-debug

# 编译生成 ELF、BIN 与 HEX 文件
cmake --build --preset dual-debug
```

### 2. 编译生产优化版 (Release 版)

```bash
cmake --preset dual-release
cmake --build --preset dual-release
```

### 3. 可选预设说明

| 预设名称 | 编译模式 | 用途说明 | 产物目录 |
|:---|:---:|:---|:---|
| `dual-debug` | Debug (-O0 -g3) | **默认推荐**。双 SPI 交互式 Demo，包含完整调试符号 | `build/dual-debug/` |
| `dual-release` | Release (-Os -DNDEBUG) | 体积极致优化，Flash 占用约 33KB | `build/dual-release/` |
| `hil-debug` | Debug | 自动化回归测试（包含 SPI 契约测试与中断抢占测试） | `build/hil-debug/` |
| `tft-diagnostic` | Debug | 极简 TFT 硬件诊断模式（仅测屏幕与纯色填充） | `build/tft-diagnostic/` |

编译完成后，构建目录将自动生成同名的 `.elf`、`.bin` 和 `.hex` 文件，并输出内存占用摘要。

---

## 四、固件烧录

通过 ST-Link V2 和 OpenOCD 烧录固件：

```bash
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "program build/dual-debug/stm32f103_gc9a01_w25qxx.elf verify reset exit"
```

如果使用 Release 固件，只需替换为对应路径：
```bash
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "program build/dual-release/stm32f103_gc9a01_w25qxx.elf verify reset exit"
```

---

## 五、在线调试与状态观察

可以通过 GDB 连接 OpenOCD 实时观察系统变量与外设运行状态：

```bash
# 启动 GDB 并直接连接 MCU
gdb-multiarch build/dual-debug/stm32f103_gc9a01_w25qxx.elf \
  -ex "target extended-remote | openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c 'gdb_port pipe'"
```

常用调试命令：
```gdb
(gdb) monitor halt
(gdb) print g_tft_init_result   # 查看屏幕初始化返回值 (LCD_OK = 0)
(gdb) print g_flash             # 查看 Flash 识别信息 (id=0xEF17, capacity=16777216)
(gdb) monitor resume
(gdb) quit
```

---

## 六、交互式 Demo 操作指南

本工程采用 **KEY1 (PB12) 单按键多功能交互**，专为 240×240 圆形屏幕优化。

### 1. 按键操作语义

- **短按（单击，50~300ms）**：
  - **主菜单**：切换至下一个菜单项（高亮框下移并循环）。
  - **功能页面**：切换显示模式、触发单次重读或刷新数据。
- **长按（按住约 1 秒，>1000ms）**：
  - **主菜单**：确认并进入当前选中的功能页面。
  - **功能页面**：退出当前功能页，返回主菜单导航。
- **视觉反馈**：每次按键触发动作时，板载 LED (PC13) 会自动翻转一次状态。

---

### 2. 四大功能页面介绍

#### 菜单页：MAIN MENU
- 居中显示 4 项演示功能。
- 底部显示按键指引：`[KEY1] Click:Next  Hold:Enter`。

#### 页面 1：1. DISPLAY DEMO
- 演示 GC9A01 圆屏的图形几何与色彩渲染能力。
- **短按操作**：循环切换 3 种显示图案：
  1. `Shapes & Geometry`：矩形边框、对角十字交叉线、同心圆与局部填充色块。
  2. `RGB565 Color Bars`：红、绿、蓝、黄、青、洋红六阶横向色彩条。
  3. `Radial Target`：同心靶心多层圆环与坐标中心线。
- **长按操作**：返回主菜单。

#### 页面 2：2. FLASH READ ONLY
- 演示 W25Q128 在 SPI2 上的通信与数据完整性校验。
- **安全保障**：本 Demo 属于**严格只读模式**，绝对不执行任何擦除（Sector Erase / Chip Erase）或写入（Page Program）操作，绝不磨损或破坏用户 Flash 中已有的数据。
- **功能特性**：
  - 读取并展示 JEDEC ID（预期 `0xEF17`，对应 W25Q128）。
  - 展示芯片物理容量（16 MB）。
  - 在安全偏移地址 `0x00100000` 连续执行两次 32 字节读取并进行逐字节比对，展示校验结果（PASS）。
  - 记录读取轮次计数器。
- **短按操作**：重新发起一次 Flash 校验读取，计数器递增。
- **长按操作**：返回主菜单。

#### 页面 3：3. DUAL SPI STATUS
- 实时展示微控制器当前双 SPI 外设的硬件与总线运行状态。
- **状态内容**：
  - `SPI1 (GC9A01 TFT)`：运行频率 (18 MHz)、时钟模式 (Mode 0)、引脚分配、DMA1 通道 (Ch3/Ch2)、总线占用状态。
  - `SPI2 (W25Q128 Flash)`：运行频率 (9 MHz)、时钟模式 (Mode 0)、引脚分配、DMA1 通道 (Ch5/Ch4)、总线占用状态。
- **短按操作**：重新采样并刷新状态。
- **长按操作**：返回主菜单。

#### 页面 4：4. KEY1 INPUT TEST
- 硬件级按键输入与消抖测试。
- **展示内容**：
  - 引脚分配：KEY1 (PB12 上拉) 与 LED (PC13 低有效)。
  - 物理实时电平：按下时显示 `PRESSED (0)`，释放时显示 `RELEASED (1)`。
  - 单击累计次数与长按累计次数。
  - 屏幕正中央图形指示器：按键按下时变为绿色实心圆，释放时变为空心圆环。
- **短按操作**：递增测试计数器。
- **长按操作**：返回主菜单。
