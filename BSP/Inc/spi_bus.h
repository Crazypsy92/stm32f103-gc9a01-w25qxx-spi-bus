/**
 * @file    spi_bus.h
 * @brief   SPI 总线抽象接口（MCU 无关）
 * @details 纯接口层：不包含任何 MCU / LL / HAL 头文件。
 *          驱动与移植层只依赖本文件；具体实现（如 spi_stm32_ll.c）负责
 *          产生真实 SPI 时序。换 MCU 时替换实现文件即可。
 *
 *          总线按"物理外设"建模：同一 SPI 外设上的多个设备共享一个 spi_bus_t，
 *          每次传输前通过 spi_bus_acquire() 占用总线（切速率/模式），
 *          事务结束后 spi_bus_release() 释放，避免两个设备互相打断。
 *
 *          并发模型（使用前务必理解）：
 *          1. 独占式：一次 acquire..release 之间，总线只属于一个设备；
 *             期间 transfer() 是阻塞的（DMA 也等 TC/BSY 才返回），
 *             所以同一时刻只有一条 SPI 事务在跑，不存在交叉改写。
 *          2. 中断安全：ISR 内调用 acquire() 会被直接拒绝（实现方通过 in_isr
 *             回调上报中断上下文）。原因：ISR 若插入主循环事务中间，
 *             会改掉 ll->current 的速率/模式并重配 DMA 通道。
 *             若 ISR 确实要访问 SPI，应把请求排入队列，由主循环统一执行。
 *          3. 异步（中断/DMA 完成回调）尚未支持：当前 release 必须在
 *             transfer() 返回后立即执行；改成非阻塞 DMA 时，release 需要
 *             挪到 "传输完成" 回调里，并给 acquire 增加忙等待/超时语义。
 */
#ifndef __SPI_BUS_H__
#define __SPI_BUS_H__

#include <stdint.h>
#include <stddef.h>

/** @brief SPI 时钟模式（极性与相位） */
typedef enum {
    SPI_BUS_MODE_0 = 0,   /* CPOL=0, CPHA=0 */
    SPI_BUS_MODE_1,       /* CPOL=0, CPHA=1 */
    SPI_BUS_MODE_2,       /* CPOL=1, CPHA=0 */
    SPI_BUS_MODE_3        /* CPOL=1, CPHA=1 */
} spi_bus_mode_t;

/** @brief 设备级总线参数 */
typedef struct {
    uint32_t       clock_hz;   /* 目标 SCLK 频率（Hz），实现方可向下取整到可达分频 */
    spi_bus_mode_t mode;       /* 时钟模式 */
    uint8_t        use_dma;    /* 1: 大块传输使用 DMA；0: 全部轮询。实现方对小块仍走轮询 */
} spi_bus_config_t;

/** @brief 总线对象（不透明） */
typedef struct spi_bus spi_bus_t;

/** @brief 总线操作表：由具体实现填充 */
typedef struct {
    /**
     * @brief 切换总线参数
     * @return 0 成功，负值失败
     * @note  实现方应缓存当前参数，参数未变化时直接返回，避免重复配置外设
     */
    int (*configure)(spi_bus_t *bus, const spi_bus_config_t *cfg);

    /**
     * @brief 全双工传输 len 字节
     * @param tx  发送缓冲区，不能为 NULL
     * @param rx  接收缓冲区，可为 NULL（只发不收）
     * @return 0 成功，负值失败（超时等）
     * @note  片选由设备层管理，本函数不操作 CS
     */
    int (*transfer)(spi_bus_t *bus, const uint8_t *tx, uint8_t *rx, uint32_t len);

    /**
     * @brief 可选：返回 1 表示当前处于中断上下文（ISR）
     * @note  NULL 表示不做上下文检查。提供后 ISR 中的 acquire() 一律返回 -1，
     *        用于防止中断抢占正在进行的 DMA 事务
     */
    int (*in_isr)(spi_bus_t *bus);
} spi_bus_ops_t;

struct spi_bus {
    const spi_bus_ops_t *ops;          /* 操作表 */
    void                *ctx;          /* 实现方私有上下文 */
    const void          *owner;        /* 当前事务占用者（设备传自己的上下文指针），NULL=空闲 */
    uint16_t             depth;        /* 事务嵌套深度：同一 owner 可重入 */
    volatile uint32_t    rejects;      /* 被拒绝的占用请求计数（ISR / 他设备占用），调试与测试用 */
    spi_bus_config_t     current_cfg;  /* 当前事务生效的设备配置，用于嵌套一致性比较 */
};

/** @brief 切换总线参数（空指针安全）
 *  @note  仅允许总线空闲（owner==NULL）时调用，避免改动正在执行的事务参数；
 *         同 owner 嵌套由 acquire() 内联处理，不走到这里。
 */
static inline int spi_bus_configure(spi_bus_t *bus, const spi_bus_config_t *cfg)
{
    if (bus == NULL || bus->ops == NULL || bus->ops->configure == NULL || cfg == NULL) {
        return -1;
    }
    if (bus->owner != NULL) {
        return -1;          /* 总线已被占用，禁止改配置 */
    }
    return bus->ops->configure(bus, cfg);
}

/** @brief 两个配置是否完全一致（clock/mode/use_dma 逐字段比较） */
static inline int spi_bus_cfg_equal(const spi_bus_config_t *a, const spi_bus_config_t *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    return (a->clock_hz == b->clock_hz) &&
           (a->mode     == b->mode) &&
           (a->use_dma  == b->use_dma);
}

/**
 * @brief 占用总线：切到本设备参数并标记占用者
 * @param bus    总线句柄
 * @param owner  设备上下文指针（唯一标识一个设备，不能为 NULL）
 * @param cfg    本设备的速率/模式配置
 * @return 0 成功；-1 参数非法 / 被其它设备占用 / ISR / depth 溢出 / 嵌套配置不一致
 * @note  - 首次占用才 configure；同 owner 嵌套不重配，且 cfg 必须与首次一致。
 *        - 片选由设备层管理，本函数不操作 CS。
 */
static inline int spi_bus_acquire(spi_bus_t *bus, const void *owner, const spi_bus_config_t *cfg)
{
    if (bus == NULL || bus->ops == NULL || owner == NULL || cfg == NULL) {
        return -1;
    }
    /* 中断上下文不允许占用总线：否则会插进主循环的 DMA 事务中间改配置 */
    if (bus->ops->in_isr != NULL && bus->ops->in_isr(bus) != 0) {
        bus->rejects++;
        return -1;
    }
    /* 不变式：owner 与 depth 必须同时为零；违反则拒绝 */
    if ((bus->owner == NULL) != (bus->depth == 0U)) {
        bus->rejects++;
        return -1;
    }
    /* 已被其它设备占用，拒绝抢占 */
    if (bus->owner != NULL && bus->owner != owner) {
        bus->rejects++;
        return -1;
    }
    /* depth 溢出保护：达到上限后不再嵌套 */
    if (bus->depth == UINT16_MAX) {
        bus->rejects++;
        return -1;
    }

    if (bus->owner == NULL) {
        /* 首次占用：切到本设备参数，失败则不占用 */
        if (spi_bus_configure(bus, cfg) != 0) {
            return -1;
        }
        bus->owner       = owner;
        bus->depth       = 1U;
        bus->current_cfg = *cfg;
    } else {
        /* 同 owner 嵌套：不重配 SPI；配置必须与首次完全一致，否则可能破坏事务 */
        if (!spi_bus_cfg_equal(&bus->current_cfg, cfg)) {
            bus->rejects++;
            return -1;
        }
        bus->depth++;
    }
    return 0;
}

/**
 * @brief 释放总线（与 acquire 成对调用）
 * @return 1 总线已被本次释放完全交还（depth 归零，可拉高 CS）；
 *         0 仍为同 owner 嵌套占用（depth>0，CS 必须保持有效）；
 *        -1 参数非法 / 非占用者 / depth 已为 0
 */
static inline int spi_bus_release(spi_bus_t *bus, const void *owner)
{
    if (bus == NULL || owner == NULL) {
        return -1;
    }
    /* 不变式违反 */
    if ((bus->owner == NULL) != (bus->depth == 0U)) {
        return -1;
    }
    if (bus->owner != owner) {
        return -1;          /* 非占用者无权释放 */
    }
    if (bus->depth == 0U) {
        return -1;          /* 没有可释放的嵌套层级 */
    }

    bus->depth--;
    if (bus->depth == 0U) {
        bus->owner = NULL;
        return 1;           /* 完全释放，port 层可拉高 CS */
    }
    return 0;               /* 仍有嵌套，CS 保持 */
}

/** @brief 查询当前占用者（无占用返回 NULL） */
static inline const void *spi_bus_owner(const spi_bus_t *bus)
{
    return (bus == NULL) ? NULL : bus->owner;
}

/** @brief 查询当前嵌套深度 */
static inline uint16_t spi_bus_depth(const spi_bus_t *bus)
{
    return (bus == NULL) ? 0U : bus->depth;
}

/**
 * @brief 总线传输（空指针安全）
 * @param owner 必须是当前总线占用者，且 depth > 0
 * @note  必须先 spi_bus_acquire() 取得总线，否则返回 -1
 */
static inline int spi_bus_transfer(spi_bus_t *bus, const void *owner,
                                   const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    if (bus == NULL || bus->ops == NULL || bus->ops->transfer == NULL ||
        owner == NULL || tx == NULL || len == 0U) {
        return -1;
    }
    /* 只有真正持有总线的设备才能传输，且必须处于某个事务层级中 */
    if (bus->owner != owner || bus->depth == 0U) {
        bus->rejects++;
        return -1;
    }
    return bus->ops->transfer(bus, tx, rx, len);
}

#endif /* __SPI_BUS_H__ */
