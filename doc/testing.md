# 驱动工程测试规范与验证指南 (Testing Guide)

本项目采用 **Host 主机离线自动化测试** 与 **Target 目标板硬件在环 (HIL) 测试** 相结合的测试体系，确保驱动层的高可靠性与内存安全性。

---

## 一、Host 主机端自动化测试 (Host Tests)

在不需要连接物理开发板的情况下，通过 GCC / Clang 在 PC 本地编译并运行驱动核心逻辑的单元测试。

### 1. 测试套件清单

位于 `tests/host/` 目录下，包含 5 套独立自动化测试程序：

| 测试名称 | 测试源文件 | 覆盖范围与验证重点 |
|:---|:---|:---|
| **`bus`** | `test_bus.c` | SPI 抽象总线互斥锁、持有者管理、嵌套重入、深度递增/递减与配置隔离 |
| **`ll`** | `test_ll.c` | STM32 LL 硬件抽象模拟、DMA 传输状态机、轮询超时与硬件级异常恢复 |
| **`flash`** | `test_flash.c` | W25QXX 协议层逻辑：JEDEC ID 读取、扇区擦除/写入/读取时序、状态轮询与边界检查 |
| **`gc9a01`** | `test_gc9a01.c` | GC9A01 协议层逻辑：长数据分块、边界越界拦截、字符/几何绘制错误传播 |
| **`spi_multi_instance`** | `test_spi_multi_instance.c` | 双 SPI 独立总线实例管理、设备配置注入、多总线互斥与资源冲突防御 |

---

### 2. 运行 Host 自动化测试

```bash
# 1. 配置并生成 Host 测试工程
cmake -S tests/host -B tests/host/build

# 2. 编译全部测试程序
cmake --build tests/host/build --parallel

# 3. 运行 CTest 自动化测试套件
ctest --test-dir tests/host/build --output-on-failure
```

---

### 3. 内存安全检测 (AddressSanitizer & UBSan)

通过启用 GCC/Clang 的 ASan 和 UBSan，严格排查内存越界访问、野指针使用和未定义行为：

```bash
# 配置带 Sanitizer 的测试构建
cmake -S tests/host -B tests/host/build_asan \
  -DCMAKE_C_FLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

# 编译并运行
cmake --build tests/host/build_asan --parallel
ctest --test-dir tests/host/build_asan --output-on-failure
```

---

## 二、Target 目标板硬件在环测试 (HIL Tests)

在目标芯片 STM32F103C8T6 上运行的实机自动化回归套件，用于验证真实硬件环境下的电气时序、DMA 并发与中断抢占。

### 1. 编译与烧录 HIL 测试固件

使用 `hil-debug` 预设直接编译：

```bash
# 编译 HIL 测试固件 (自动开启 CONFIG_HIL_TESTS=1)
cmake --preset hil-debug
cmake --build --preset hil-debug

# 烧录到目标板
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "program build/hil-debug/stm32f103_gc9a01_w25qxx.elf verify reset exit"
```

### 2. HIL 测试涵盖项目

1. **GC9A01 显示自测**：纯色填充、几何形状、字符渲染与色条渐变。
2. **Flash 识别与只读一致性校验**：自动识别 `0xEF17`，在 `0x00100000` 执行 20 轮连续读取并进行全量哈希对比。
3. **SPI 接口契约检查 (20 项)**：验证未占用传输拒绝、非法释放拦截、重入深度一致性等总线契约。
4. **TIM2 高优先级中断抢占测试 (20 kHz)**：配置高优先级硬件定时器 TIM2 周期性抢占正在执行 DMA 事务的主循环，严格验证中断抢占下总线互斥锁的可靠性（`probe.granted 恒为 0`）。

### 3. Flash 安全保护机制

- 默认配置为**非破坏性安全模式**（`CONFIG_FLASH_DESTRUCTIVE_TEST=0`）：只读验证，绝不执行物理扇区擦除或写入。
- 物理擦除计数器（`erase_count`）严格保持为 0。

---

## 三、极简 TFT 硬件诊断模式 (TFT Diagnostic)

当遇到新硬件布线、杜邦线接触不良或排查花屏故障时，可编译极简诊断模式：

```bash
cmake --preset tft-diagnostic
cmake --build --preset tft-diagnostic
```

该模式不初始化 Flash、不启动 TIM2 中断、不运行复杂 Demo，仅以 1.125 MHz 基础轮询模式依次填充黑、红、绿、蓝、白 5 种纯色，用于快速确认屏幕供电与接线正常。

---

## 四、持续集成 (CI Workflow)

本项目已配置 GitHub Actions 自动化 CI 工作流（`.github/workflows/host-tests.yml`）：
- **触发条件**：对 `main`、`master` 分支的 Push 以及所有 Pull Request。
- **矩阵测试**：每次提交自动在 Ubuntu 环境下运行 **Normal 模式** 与 **Sanitizer (ASan/UBSan) 模式** 的全部 5 套 Host 自动化测试。
