/**
 * @file    bsp_spi.h
 * @brief   板级 SPI 总线接口（与 MCU 无关）
 * @details 同一物理 SPI 外设只在这里创建一次，多个设备通过
 *          BSP_SPI1_GetBus() 取得同一个 spi_bus_t 共享使用。
 */
#ifndef __BSP_SPI_H__
#define __BSP_SPI_H__

#include "spi_bus.h"

/**
 * @brief 获取 SPI1 抽象总线（首次调用时自动完成 GPIO/外设初始化）
 * @note  GC9A01 与 W25QXX 共享该总线，各自在传输前切换速率/模式
 */
spi_bus_t *BSP_SPI1_GetBus(void);

/* ====================================================================
 * 中断抢占探针（测试用，可整体删除，不影响总线功能）
 * 用途：用一个高优先级 TIM2 中断周期性尝试占用 SPI1，验证
 *       "中断不可能插进主循环的 DMA 事务"这一约束是否成立。
 * ==================================================================== */

/** @brief 抢占探针统计 */
typedef struct {
    uint32_t probes;    /* ISR 执行次数 */
    uint32_t saw_busy;  /* ISR 进入时总线正被主循环占用（真正的抢占点） */
    uint32_t granted;   /* ISR 成功占用总线次数，必须恒为 0 */
} bsp_spi_probe_t;

/** @brief 初始化探针：仅配置 NVIC，不启动定时器 */
void BSP_SPI1_PreemptProbe_Init(void);

/** @brief 启动探针（rate_hz 建议 10k~20k），优先级高于 SysTick；会清零统计 */
void BSP_SPI1_PreemptProbe_Start(uint32_t rate_hz);

/** @brief 停止探针（保留统计，便于读取结果） */
void BSP_SPI1_PreemptProbe_Stop(void);

/** @brief 手动触发一次探针中断（用于确定性契约测试） */
void BSP_SPI1_PreemptProbe_Inject(void);

/** @brief 读取统计 */
void BSP_SPI1_PreemptProbe_Get(bsp_spi_probe_t *out);

/**
 * @brief  板级 SPI 恢复入口（测试/故障用）
 * @return 0 成功，负值失败
 * @note   调用 spi_stm32_ll_recover，不触碰任何 CS；供恢复测试使用。
 */
int BSP_SPI1_Recover(void);

#endif /* __BSP_SPI_H__ */
