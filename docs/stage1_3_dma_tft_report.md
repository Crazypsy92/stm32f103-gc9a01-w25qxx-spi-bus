# Stage 1.3：SPI DMA 分块与 TFT 诊断页（开发阶段）

> 基线提交：`d7640d0`；分支：`codex/stage1_3-dma-tft-check`

## 本轮变更

| 范围 | 变更 | 验证性质 |
|---|---|---|
| SPI DMA | 每块 DMA TCIF 后分别等待 TXE=1、BSY=0，再停 DMA 进入下一块；错误仍走统一清理 | Host Mock 边界和故障注入 |
| Host 测试 | 使用真实 70,000 B TX/RX 缓冲，覆盖 65,535/65,536/70,000 B、TX-only、BSY 卡死；记录 DMA 内存地址与 BSY 检查次数 | CTest；不模拟真实数据搬运 |
| GC9A01 测试画面 | 引导页与最终圆屏诊断页，展示 CT、IRQ、LEN、READ、EQ、REC、REF、SAFE、PASS/FAIL | 尚需真实屏幕肉眼确认 |
| CI | GitHub Actions Host 标准/ASan+UBSan 回归 | 根据实际工作流运行结果填写 |

## 安全边界

- 未更改 W25QXX 数据、引脚、SPI 9/18 MHz 时钟或实际 DMA 通道。
- 未执行外部 Flash 擦写，`CONFIG_FLASH_DESTRUCTIVE_TEST` 默认保持 0。
- Host SPI Mock 不模拟真实逐字节 DMA 数据搬运，因此无法以此证明 70KB 物理数据内容正确；仅验证分块长度、内存地址和控制时序。
- 真机 DMA/GC9A01 屏幕画面尚未在本轮独立验证，不得标记 HIL PASS。
- GC9A01 的 `LCD_ShowPicture` 大于 65535B 截断与 Port 错误传播留待 Stage 1.4。

## 计划验收

1. Host `test_bus`、`test_flash` 与扩展 `test_ll` 通过，无 sanitizer 错误。
2. DMA 每一块必须先确认 TXE/BSY 后再配置下一块；BSY 卡死不能开启第二块。
3. 真实 STM32F103C8T6 在非破坏性模式复验：TFT 页面可读、SPI 共享互不抢占、Flash 读一致。
4. 若 HIL 无设备接入，标记 BLOCKED，不重写旧报告为新证据。
