# Stage 1.2: W25Q128 Flash 数据可靠性与安全保护整改方案

> **项目**: `stm32f103-gc9a01-w25qxx-spi-bus`  
> **分支**: `codex/stage1_2-flash-reliability`  
> **基准提交**: `8015820a38f78eb4c590b89f39692674d7cb3fce`  
> **目标外设**: Winbond W25Q128 (16 MB NOR Flash, SPI1 共享总线)  

---

## 1. 核心问题根因剖析与改进目标

在 Stage 1 建立了可靠的 SPI 同步总线仲裁、DMA 异常恢复与片选生命周期管理后，Stage 1.2 专注于 Flash 协议层（`BSP/Src/w25qxx.c`）和移植层（`BSP/Src/w25qxx_port.c`）的数据可靠性与安全保护，彻底消除如下六大隐患：

| 编号 | 核心缺陷与隐患 | 根因机制剖析 | Stage 1.2 改进目标 |
|:---:|---|---|---|
| **F01** | **Flash 访问无完整边界检查** | 原驱动仅假定传入地址和长度合法，缺少 `addr >= capacity`、`len > capacity - addr` 的溢出与越界校验；无 24 位地址隐式截断防护；扇区擦除未对齐时直接触发错误扇区擦除。 | 建立统一 `W25QXX_CheckAddress` 边界与参数防线，强约束 24 位地址空间与 4KB 扇区对齐，拒绝非法访问。 |
| **F02** | **WriteEnable 无状态返回与失败拦截** | 原 `W25QXX_WriteEnable` 为 `void` 函数，虽有 5 次重试，但若 Flash 处于写保护或硬件故障时，WREN 失败后仍会盲目下发 Page Program 或 Erase 命令，破坏状态机。 | 改造为返回 `W25QXX_Result`，每次验证状态寄存器 WEL 位（bit 1），重试耗尽返回 `W25QXX_ERROR_WEL` 并阻断后续所有破坏性操作。 |
| **F03** | **单字节 SPI 交换失败被静默吞掉** | 原总线接口 `uint8_t (*swap)(void *ctx, uint8_t tx)` 只能返回数据，无法区分“总线故障返回 0xFF”与“真实读到 0xFF”；CS 申请被拒时驱动层无感知。 | 调整为 `int (*swap)(void *ctx, uint8_t tx, uint8_t *rx)`，`cs_low/cs_high` 均返回状态码；全链路检查返回值，故障立即释放 CS 并上报 `W25QXX_ERROR_BUS`。 |
| **F04** | **WaitBusy 易将通信失败误判为完成** | 若读取状态寄存器时总线发生故障读回 `0x00`（或浮空态），原逻辑 `(status & 0x01) == 0` 会误判为“操作已完成”，导致提前退出并打断物理写入。 | `WaitBusy` 严格校验状态寄存器读取的 SPI 传输结果，底层通信错误立即退出并返回 `W25QXX_ERROR_BUS`，杜绝误判。 |
| **F05** | **上电自动擦写固定物理地址** | 原系统在 `AppFlash_Test` 与 `AppBusTest` 中上电即擦写 `0x000000` 与 `0x00100000`，造成未知数据破坏与寿命损耗。 | 引入测试模式分级体系：默认上电为非破坏性安全模式（零擦除、零写入、只读识别自测）；破坏性测试需显式启用且禁止使用 0 地址。 |
| **F06** | **固定延时缺少诊断依据** | 早期为规避页尾截断加入了 3ms 与 40ms 延时，但缺乏软件诊断数据与时序监控。 | 保留必要规格延时作为硬件防护，补充状态机与软件时间戳监测，明确区分软件时序观测与物理电荷泵推论。 |

---

## 2. 接口设计与架构整改

### 2.1 抽象总线接口重构 (`W25QXX_Bus`)

将抽象总线接口由“无状态反馈”升级为“全状态感知”：

```c
typedef struct {
    int      (*cs_low)(void *ctx);                              /* 0 成功，负值失败 */
    int      (*cs_high)(void *ctx);                             /* 0 成功，负值失败 */
    int      (*swap)(void *ctx, uint8_t tx, uint8_t *rx);       /* 0 成功，负值失败 */
    uint32_t (*millis)(void *ctx);                              /* 毫秒时间戳 */
    void    *ctx;                                               /* 上下文 */
    int      (*read_bulk)(void *ctx, uint8_t *rx, uint32_t len);        /* DMA 批量读 */
    int      (*write_bulk)(void *ctx, const uint8_t *tx, uint32_t len); /* DMA 批量写 */
} W25QXX_Bus;
```

### 2.2 结果状态码细化 (`W25QXX_Result`)

```c
typedef enum {
    W25QXX_OK            = 0,
    W25QXX_ERROR_PARAM   = -1,     /* 参数非法（空指针、长度0、越界、溢出、未对齐、未识别） */
    W25QXX_ERROR_TIMEOUT = -2,     /* 等待 BUSY 清零超时 */
    W25QXX_ERROR_BUS     = -3,     /* 底层 SPI 通信失败 / CS 申请拒绝 / 传输故障 */
    W25QXX_ERROR_WEL     = -4      /* 写使能 WEL 锁存失败，已拦截破坏性操作 */
} W25QXX_Result;
```

### 2.3 边界防御算法实现

```c
static W25QXX_Result W25QXX_CheckAddress(const W25QXX_Device *dev, uint32_t addr, uint32_t len, uint8_t check_sector_align)
{
    if (dev == NULL || dev->bus == NULL || dev->capacity == 0U || len == 0U) {
        return W25QXX_ERROR_PARAM;
    }
    /* 1. 防无符号回绕与越界：addr >= capacity 或 len > capacity - addr */
    if (addr >= dev->capacity || len > (dev->capacity - addr)) {
        return W25QXX_ERROR_PARAM;
    }
    /* 2. 24位寻址边界防截断（W25Q128 为 16MB = 0x01000000） */
    if (addr >= 0x01000000U || (addr + len) > 0x01000000U) {
        return W25QXX_ERROR_PARAM;
    }
    /* 3. 扇区擦除必须严格 4096 字节对齐 */
    if (check_sector_align && (addr % W25QXX_SECTOR_SIZE) != 0U) {
        return W25QXX_ERROR_PARAM;
    }
    return W25QXX_OK;
}
```

---

## 3. 非破坏性安全模式架构设计

为了实现“默认启动零擦除、零写入、零破坏”，系统通过模式开关进行解耦：

```
                    +------------------------------------+
                    |        系统上电初始化 main()       |
                    +------------------------------------+
                                      |
                     [CONFIG_FLASH_DESTRUCTIVE_TEST]
                                     / \
                        (默认: 0)   /   \  (显式指定: 1)
                                   /     \
                                  v       v
           +-------------------------+   +-------------------------------+
           |    非破坏性安全模式     |   |       破坏性测试模式          |
           |-------------------------|   |-------------------------------|
           | • 读取 JEDEC ID / 容量  |   | • 校验安全测试扇区合法性      |
           | • 扇区只读自测 (DMA)    |   | • 执行 100 轮擦写读压测       |
           | • 18 项总线契约断言     |   | • 1~480B 长度矩阵擦写验证     |
           | • 20kHz 抢占纯读压力    |   | • 严格记录并累加擦除次数      |
           | • 1000 次高频读事务     |   | • 结束后验证数据与总线状态    |
           | • 擦除次数严格保持为 0  |   +-------------------------------+
           +-------------------------+
```

---

## 4. 时序诊断与固定延时评估 (F06)

在 W25Q128 数据手册规格中：
- 页编程时间 $t_{\text{PP}}$：典型值 $0.7\text{ ms}$，最大值 $3.0\text{ ms}$。
- 扇区擦除时间 $t_{\text{SE}}$：典型值 $45\text{ ms}$，最大值 $400\text{ ms}$。
- 片选拉高后高电平恢复时间 $t_{\text{SHSL}}$：写操作后最小为 $50\text{ ns} \sim 100\text{ ns}$。

**诊断分析与代码定位**：
1. **`Delay_ms(3)` 与 `Delay_ms(40)` 的作用**：在软件发起物理写入后提供初始等待窗口，减少在 CPU 高频轮询下对 SPI 总线与 Flash 内部控制器总线的争用。
2. **严谨归因声明**：在没有外部硬件逻辑分析仪探头直接抓取 Flash 硅片内部电荷泵电压的情况下，本工程仅从**软件状态机观测**证实：写入后若立即密集读取状态寄存器，存在偶发通信争用；加入该保护延时能使软件读写闭环 100% 稳定。本报告不妄断硬件物理层微观机理。
