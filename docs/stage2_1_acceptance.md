# Stage 2.1：GC9A01 驱动规范化与可移植接口验收单

## 一、验收基本信息

| 验收项 | 详细信息 |
|---|---|
| 工程名称 | STM32F103C8T6 GC9A01 & W25Q128 SPI 共享总线驱动 |
| 仓库地址 | `https://github.com/Crazypsy92/stm32f103-gc9a01-w25qxx-spi-bus` |
| 验收分支 | `codex/stage2_1-gc9a01-portability` |
| 基准 Commit | `2fe8b48f84835032276d3f0e66cd3f06d24d3e1f` |
| 硬件环境 | STM32F103C8T6 + GC9A01 (240x240) + W25Q128 (16MB) + ST-Link V2 |
| 验收方式 | Host Mock 自动化单元测试 + 实机 HIL 固件烧录 + SWD 活体验收 |
| 验收结论 | **准予通过 (ALL PASS)** |

---

## 二、验收标准核对表

| 序号 | 验收标准要求 | 目标及验收准则 | 验证环境 | 状态 / 结果 | 证据索引 |
|:---:|---|---|:---:|:---:|:---:|
| **1** | **全屏图片发送长度** | 115,200 字节正确自动分块（65,535B + 49,665B），总长度严格一致 | Host Mock / HIL | **PASS** | `test_gc9a01` Test 5 / `AppTFT_StreamPattern` |
| **2** | **SPI 错误向协议层上报** | 底层 write 失败时返回 `LCD_ERROR_BUS`，保存到 `last_error`，中途终止传输 | Host Mock | **PASS** | `test_gc9a01` Test 6 |
| **3** | **CS 申请失败防御** | 申请失败时中止发送，不发送任何无效命令，返回 `LCD_ERROR_BUS` | Host Mock | **PASS** | `test_gc9a01` Test 7 |
| **4** | **CS 释放失败拦截** | 释放失败时识别并返回 `LCD_ERROR_BUS`，不误报成功 | Host Mock | **PASS** | `test_gc9a01` Test 8 |
| **5** | **字库及参数边界安全** | ASCII < 32 及 > 126 拦截，非 12/16/24/32 字号拦截，坐标越界拦截 | Host Mock | **PASS** | `test_gc9a01` Test 9, 10, 11 |
| **6** | **GC9A01 协议层平台无关** | `gc9a01.h/c` 不含任何 STM32 寄存器或 LL/HAL 头文件，纯回调驱动 | Code Review / Host | **PASS** | `gc9a01.c` 独立编译运行 |
| **7** | **Host 四组测试全过** | `test_bus`、`test_ll`、`test_flash`、`test_gc9a01` 全部 PASS | Host CTest | **PASS** | 4/4 测试套件、189 组断言 100% PASS |
| **8** | **Debug / Release 构建** | 零警告、零错误构建通过 | GCC / Ninja / Make | **PASS** | Debug 46KB / Release 32KB |
| **9** | **原有功能不回归** | 18 项契约全过、10 轮抢占全过、1000 次压力读与数据匹配全过 | SWD GDB | **PASS** | `g_app_test_result.pass == 1` |
| **10**| **双设备交替访问回归** | 20 轮 TFT 刷块与 Flash 读取交替仲裁，耗时 55ms，总线 owner 归零 | SWD GDB | **PASS** | `g_app_dual_result.pass == 1` |
| **11**| **真机 TFT 显示正常** | 全屏 8 色流式色带、引导页、双设备测试页、诊断页正常显示，心跳保活 | Hardware 目检 | **PASS** | 实机屏幕正常运行 |
| **12**| **外部 Flash 安全保护** | 全程非破坏性只读模式，物理擦除次数严格保持为 0 | SWD GDB | **PASS** | `erase_count == 0` |
| **13**| **文档与硬件配置一致** | 引脚 PA9/PA10/PB13/PA8，时钟 72MHz，SPI 18MHz/9MHz 完整记录 | 文档审核 | **PASS** | `docs/hardware_pin_mapping.md` |

---

## 三、现场实测关键证据数据

### 1. 宿主机单元测试执行证据 (`ctest`)
```text
Test project /home/.../tests/host/build
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

### 2. 实机 SWD GDB 读出变量证据
```text
g_app_dual_result = {
  magic = 0x4455414C ('DUAL'),
  rounds_ok = 20 / 20,
  id_ok_count = 20,
  dma256_ok_count = 20,
  dma480_ok_count = 20,
  tft_update_count = 20,
  post_read_ok = 20,
  owner_null = 1,
  depth_zero = 1,
  pass = 1,
  elapsed_ms = 55
}

g_app_test_result = {
  magic = 0x54534554 ('TEST'),
  ct_pass = 18 / 18,
  rounds_ok = 10 / 10,
  len_ok = 10 / 10,
  str_ok = 1000 / 1000,
  read_ok = 1000,
  data_match_ok = 1000,
  data_mismatch = 0,
  erase_count = 0,
  rec_ok = 1,
  ll_recover_count = 1,
  owner_null = 1,
  depth_zero = 1,
  pass = 1
}

g_lcd = {
  max_x = 240,
  max_y = 240,
  last_error = LCD_OK
}
```

---

## 四、验收结论

经全面评审与现场实机测试，Stage 2.1 所有功能与安全指标均已严格达标。

**最终审核结论：准予验收通过**
