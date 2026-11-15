# Stage 1: SPI 同步总线与 DMA 异常处理整改方案与设计

> **项目**: `stm32f103-gc9a01-w25qxx-spi-bus`  
> **分支**: `codex/stage1-spi-reliability`  
> **目标硬件**: STM32F103C8T6 (72MHz, 20KB SRAM, 64KB Flash)  
> **外设分配**: SPI1 (PA5/SCK, PA6/MISO, PA7/MOSI), GC9A01 LCD (PA9/CS, 18MHz, 模式0), W25Q128 Flash (PA8/CS, 9MHz, 模式0), DMA1_Ch3 (TX), DMA1_Ch2 (RX).

---

## 1. 整改背景与核心问题剖析

在原始工程中，GC9A01 与 W25Q128 共享 SPI1，已经验证了基础的互斥访问与分时复用。但在工业级可靠性、边界异常恢复以及多层嵌套所有权管理上，存在若干潜在漏洞与隐患：

### 1.1 总线所有权与接口契约隐患
1. **Transfer 缺乏调用者身份校验**：原始 `spi_bus_transfer(bus, tx, rx, len)` 仅校验 `bus->owner != NULL`，任意模块（甚至非当前持有者或第三方代码）均可在总线被占用时向外设注入数据，破坏当前设备时序。
2. **NULL Owner 越权漏洞**：若传入 `owner == NULL`，原始逻辑可能导致未占用总线时被意外释放或将内部状态置乱。
3. **嵌套深度溢出无保护**：`depth` 为无符号整型，若在递归或深层调用中超过 `UINT16_MAX` 会回绕为 0，导致总线所有权判定彻底混乱。
4. **嵌套期间总线参数重配**：同一持有者嵌套调用 `acquire` 时，若传入不同通信参数（如时钟频率或工作模式改变），原始代码未做校验直接拒绝或直接覆盖，导致嵌套内外上下文冲突。
5. **片选物理电平与逻辑嵌套脱节**：设备 Port 层若在单次嵌套释放时就拉高物理 CS 引脚，会导致外设事务（如 Flash 页写入、连续命令序列）被意外截断。

### 1.2 SPI 配置缓存漏洞
1. **`use_dma` 标志未纳进有效缓存覆盖**：旧配置缓存仅比对了 `clock_hz` 和 `mode`。当设备切换传输模式（例如从 DMA 模式降级到轮询模式），由于外设寄存器无需重新初始化，驱动忽略了配置更新，导致后续传输仍按旧配置走 DMA。
2. **正在传输期间修改外设配置**：若 SPI 正在忙或 DMA 通道尚在使能运行，重设分频系数或模式会引起 SPI 外设硬件死锁或产生非法毛刺。

### 1.3 DMA 错误与完成判定不完整
1. **未区分完成（TCIF）与传输错误（TEIF）**：原 `dma_wait_tc()` 仅查询 TCIF，忽略了总线访问错误或配置非法产生的 TEIF。发生 TEIF 时等待循环陷入死锁直至超时。
2. **BSY 与 TXE 结束判定时序缺陷**：DMA 发送完成标志（TCIF）仅表示内存数据已搬入 SPI 发送数据寄存器（DR），此时移位寄存器中仍有 1~2 个字节在传输。若此时立即拉高 CS 或禁用 SPI，最后一个字节的数据位会被截断。
3. **DMA 超时无时基保障**：原实现采用简单的 CPU `for` 循环空转计数，极易受编译器优化级别（-O0/-O2）、中断嵌套打扰而导致实际超时预算严重偏移。
4. **DMA 16位传输长度截断**：STM32F1 DMA 的 `CNDTR` 寄存器为 16 位（最大 65535）。当传输长度超过 65535 时若未分块，会发生隐式截断。

### 1.4 全双工残留与 OVR 溢出
1. **TX-only 传输产生的 OVR 锁死**：SPI 外设为全双工，在纯发送（如屏幕刷屏、Flash 指令）时，MOSI 移出的同时 MISO 不断移入数据填满 RX 缓冲区。若不及时读取 DR，RXNE 置位并最终触发 OVR（Overrun Error）。OVR 标志在 STM32F1 SPI 内部会阻塞后续正常通信。

### 1.5 Flash 状态机与物理电荷泵建立延迟
1. **W25Q128 页编程与状态寄存器锁存时序**：W25Q128 在页编程（02h）拉高 CS 后，内部电荷泵启动需要典型 ~200-300µs。若在 CS 拉高后立即频繁轮询状态寄存器（05h），前几次轮询读到的状态可能出现过渡态或瞬间 `0x00`，使 `WaitBusy` 误判为已完成并立即退出，随后读操作或下一次操作拉低 CS，直接中断了 Flash 正在进行的物理编程，导致页末尾数据未被写入（表现为 0xFF）。

---

## 2. 总体架构与接口设计调整

### 2.1 架构分层

```
+-----------------------------------------------------------------------+
|                    应用层 / 测试层 (AppBusTest)                        |
+-----------------------------------------------------------------------+
         |                                           |
         v (驱动协议API)                             v (直接总线契约测试)
+------------------------+                  +---------------------------+
| GC9A01 LCD 驱动协议     |                  | W25QXX Flash 驱动协议     |
+------------------------+                  +---------------------------+
         | (LCD_Port)                                | (W25QXX_Bus)
         v                                           v
+------------------------+                  +---------------------------+
| gc9a01_port.c 适配层    |                  | w25qxx_port.c 适配层      |
+------------------------+                  +---------------------------+
         \                                           /
          +-------------------+---------------------+
                              | (spi_bus_acquire/release/transfer)
                              v
       +-----------------------------------------------+
       | BSP/Inc/spi_bus.h (MCU无关的同步总线互斥抽象)  |
       | - 强类型所有权校验 (owner 参数绑定)            |
       | - 嵌套深度溢出防御 (UINT16_MAX)               |
       | - 配置一致性保护 (spi_bus_cfg_equal)          |
       | - 状态不变式保障 (owner==NULL <=> depth==0)   |
       +-----------------------------------------------+
                              | (bus->configure / bus->transfer)
                              v
       +-----------------------------------------------+
       | BSP/Src/spi_stm32_ll.c (STM32F1 LL / DMA 实现)|
       | - DWT 绝对时钟周期超时检测                     |
       | - DMA TCIF / TEIF 双重判定                    |
       | - TXE + BSY 硬件时钟结束保障                  |
       | - >65535 字节自动分块                         |
       | - RX 排空与 OVR 自动清除                      |
       | - spi_recover 统一异常复位与恢复机制          |
       +-----------------------------------------------+
                              |
                     [ STM32F103 硬件外设 ]
              (SPI1 / DMA1_Ch2 / DMA1_Ch3 / GPIO)
```

### 2.2 核心接口调整说明

#### 1. `spi_bus_transfer` 调用者校验
```c
/* 旧接口：缺乏所有权约束，易受旁路调用篡改 */
int spi_bus_transfer(spi_bus_t *bus, const uint8_t *tx, uint8_t *rx, uint32_t len);

/* 新接口：必须传入调用方 owner，严格校验身份与嵌套状态 */
int spi_bus_transfer(spi_bus_t *bus, const void *owner, const uint8_t *tx, uint8_t *rx, uint32_t len);
```
**校验规则**：
- `bus != NULL && owner != NULL && tx != NULL && len > 0`。
- `bus->owner == owner`（必须为当前成功 acquire 的设备）。
- `bus->depth > 0`（持有深度必须大于 0）。
- 不变量校验：`(bus->owner == NULL) == (bus->depth == 0)`。

#### 2. LL 层统一状态码 `spi_ll_status_t`
```c
typedef enum {
    SPI_LL_OK = 0,           /**< 传输成功完成 */
    SPI_LL_ERROR_PARAM,      /**< 入参非法（空指针、长度为0、非法时钟/模式） */
    SPI_LL_ERROR_TIMEOUT,    /**< 传输或等待超时（DWT周期超出预算） */
    SPI_LL_ERROR_DMA,        /**< DMA 传输错误（硬件置位 TEIF） */
    SPI_LL_ERROR_BUSY,       /**< 外设忙或已被其他事务锁定 */
    SPI_LL_ERROR_RECOVERY    /**< 恢复流程失败 */
} spi_ll_status_t;
```

#### 3. 统一异常恢复接口 `spi_stm32_ll_recover`
```c
/* 对外暴露的硬件实例级恢复接口 */
spi_ll_status_t spi_stm32_ll_recover(spi_stm32_ll_t *ll);
int BSP_SPI1_Recover(void);
```

---

## 3. 状态机模型与安全不变式

### 3.1 总线所有权状态机

```
                      +-------------------+
                      |       FREE        |
                      | owner == NULL     |
                      | depth == 0        |
                      +-------------------+
                        |               ^
   acquire(owner, cfg)  |               | release(owner) [depth == 1]
   [bus->in_isr == 0]   |               | (拉高物理CS, bus->owner = NULL)
                        v               |
                      +-------------------+
         +----------->|      OWNED        |<-----------+
         |            | owner == dev_ptr  |            |
         |            | depth == 1        |            |
         |            +-------------------+            |
         |              |               ^              |
release  |      acquire |               | release      |
[depth==2]       [same  |               | [depth > 2]  |
         |        cfg]  v               |              |
         |            +-------------------+            |
         +------------|      NESTED       |------------+
                      | owner == dev_ptr  |
                      | 2 <= depth < MAX  |
                      +-------------------+
```

### 3.2 必须严格满足的安全不变式 (Invariants)

1. **状态一致性不变式 (State Invariant)**:
   $$\forall t, \quad (\text{bus}\to\text{owner} == \text{NULL}) \iff (\text{bus}\to\text{depth} == 0)$$
   任何时刻，只要 `owner` 为空，`depth` 必为 0；反之只要 `depth` 为 0，`owner` 必为空。

2. **传输鉴权不变式 (Transfer Authorization Invariant)**:
   $$\text{spi\_bus\_transfer}(\text{bus}, \text{owner}, \dots) == \text{OK} \implies (\text{bus}\to\text{owner} == \text{owner}) \land (\text{bus}\to\text{depth} > 0)$$
   未经合法 `acquire` 或持有者身份不匹配的传输请求在接口入口处被立即拒绝，绝不触碰硬件寄存器。

3. **配置不可变性不变式 (Configuration Immutability Invariant)**:
   当总线处于 `OWNED` 或 `NESTED` 状态时，总线配置参数锁死：
   - 拒绝外部调用者直接调用 `configure`。
   - 同一 owner 嵌套 `acquire` 时，若传入配置与 `active_cfg` 不一致（`clock_hz`、`mode`、`use_dma` 任一不同），立即返回错误并维持原总线状态。

4. **深度上限防御不变式 (Depth Saturation Invariant)**:
   当 `depth == UINT16_MAX` 时，再次执行 `acquire` 必须拒绝，保持 `depth` 不变，禁止无符号溢出回绕。

5. **中断隔离不变式 (ISR Exclusion Invariant)**:
   在中断上下文（`__get_IPSR() != 0`）中调用 `acquire` 恒定被拒绝，防止高优先级中断抢占导致 SPI/DMA 寄存器现场被篡改。

---

## 4. 关键可靠性机制设计

### 4.1 基于 DWT 周期计数器的绝对超时判定
取消非确定性的 CPU 空循环计数，直接利用 ARM Cortex-M3 内核的 `DWT->CYCCNT` 32 位时钟周期寄存器：
- **CPU 主频**: 72 MHz（1 周期约为 13.88 ns）。
- **回绕安全机制**: 使用 C 语言无符号 32 位减法天然处理计数器溢出：
  ```c
  static inline int ll_timeout_expired(uint32_t start, uint32_t timeout) {
      return ((DWT->CYCCNT - start) > timeout) ? 1 : 0;
  }
  ```
- **超时预算设计**:
  - `SPI_BYTE_TIMEOUT_CYCLES`: 500,000 周期（约 6.94 ms），单字节发送/接收余量充足。
  - `SPI_BSY_TIMEOUT_CYCLES`: 500,000 周期（约 6.94 ms）。
  - `SPI_DMA_MARGIN_CYCLES`: 1,000,000 周期（约 13.88 ms），动态加上传输所需理论周期：
    $$\text{timeout} = \left(\text{chunk} \times 8 \times \frac{f_{\text{cpu}}}{f_{\text{spi}}}\right) + \text{MARGIN}$$

### 4.2 DMA 错误检测与原子退出
在 `dma_wait` 循环中同时监测两个硬件标志：
```c
while (1) {
    uint32_t isr = dma->ISR;
    if ((isr & te_mask) != 0U) {
        return SPI_LL_ERROR_DMA;       /* 硬件传输错误，立即终止 */
    }
    if ((isr & tc_mask) != 0U) {
        return SPI_LL_OK;              /* 传输完成 */
    }
    if (ll_timeout_expired(start, timeout_cycles)) {
        return SPI_LL_ERROR_TIMEOUT;   /* 超时 */
    }
}
```
一旦检测到 `TEIF` 或超时，立即终止等待并执行统一清理。

### 4.3 彻底的 DMA 事务结束与片选时序防护
DMA 完成仅代表数据写入 `SPI_DR`，必须串行完成两级等待以保证最后一个字节移出并安全锁存：
```c
/* 1. 等待最后一个字节从 DR 移入移位寄存器 */
while (!LL_SPI_IsActiveFlag_TXE(spi)) { ... }
/* 2. 等待最后一个字节在 MOSI 上完成所有位移出 */
while (LL_SPI_IsActiveFlag_BSY(spi)) { ... }
```
只有当 `BSY == 0` 时，外设才完全恢复空闲，此时 Port 层拉高 CS 绝不截断尾部数据。

### 4.4 接收排空与 OVR 自动清除
在发送前后强制执行清理序列，防止全双工通信时 RX 移位寄存器积压：
```c
static inline void spi_drain_rx_and_clear_ovr(SPI_TypeDef *spi)
{
    while (LL_SPI_IsActiveFlag_RXNE(spi)) {
        (void)LL_SPI_ReceiveData8(spi);
    }
    (void)LL_SPI_ReceiveData8(spi);
    LL_SPI_ClearFlag_OVR(spi);
}
```

### 4.5 DMA 长度边界分块 (>65535)
针对 STM32F1 DMA `CNDTR` 16 位限制，`ll_transfer_dma` 实现自动分块：
- 单次块大小限制为 `SPI_DMA_MAX_CHUNK = 65535`。
- 分块循环由底层统一驱动，分块间片选引脚由上层 Port 维持有效，不释放总线，不重新配置，对外呈现单一连续的大数据流传输。

### 4.6 统一异常恢复机制 `spi_recover`
当检测到参数错误、超时、DMA 硬件错误或通信异常时，统一执行恢复动作：
1. 关闭 SPI 发送与接收 DMA 请求（`LL_SPI_DisableDMAReq_TX/RX`）。
2. 禁用 DMA TX/RX 通道（`LL_DMA_DisableChannel`）。
3. 清除 DMA 通道状态标志（`dma_clear_flags`）。
4. 排空 SPI 接收残留数据并清除 OVR 标志。
5. 等待 BSY 清零；若超时则复位重启 SPI 外设。
6. 按当前已保存的通信配置重新初始化 SPI 外设，使其立即可用于下一次事务。

### 4.7 Flash 物理电荷泵与状态寄存器锁存时序防护
针对 W25Q128 在页写入后内部电荷泵启动需要时间的物理特性：
- 在 `W25QXX_Write_Page` 拉高 CS 后，加入 `Delay_ms(3)` 保证物理页编程周期完成，防止过早拉低 CS 导致内部电荷泵中断。
- 在 `W25QXX_EraseSector` 拉高 CS 后，加入 `Delay_ms(40)` 覆盖典型扇区擦除时间，防止频繁轮询导致总线电平扰动。
- `W25QXX_WaitBusy` 轮询时做超时防御，确保状态完全就绪。
