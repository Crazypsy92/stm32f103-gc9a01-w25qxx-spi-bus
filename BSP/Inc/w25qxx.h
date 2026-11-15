/**
 * @file    w25qxx.h
 * @brief   W25Qxx 系列 SPI NOR Flash 驱动头文件
 * @details 纯协议层：不包含任何 MCU/LL/HAL 头文件，只依赖下面定义的
 *          W25QXX_Bus 回调表 + Delay_ms。移植时在 port 层实现回调即可。
 */
#ifndef __W25QXX_H
#define __W25QXX_H

#include <stdint.h>

/* 调试打印开关：1 时经 printf 输出日志（需先接好串口重定向），默认关闭 */
#ifndef W25QXX_LOG_ENABLE
#define W25QXX_LOG_ENABLE 0
#endif
#if W25QXX_LOG_ENABLE
#include <stdio.h>
#define W25_LOG(...) printf(__VA_ARGS__)
#else
#define W25_LOG(...)
#endif

/* W25Qxx 芯片 ID 定义 */
#define W25Q80 	0XEF13
#define W25Q16 	0XEF14
#define W25Q32 	0XEF15
#define W25Q64 	0XEF16
#define W25Q128	0XEF17
#define W25Q10BW 0xFF5E

#define W25QXX_SECTOR_SIZE   4096U
#define W25QXX_PAGE_SIZE     256U

/* 忙等待超时（毫秒，可由总线 millis 回调计时；未提供 millis 时不做超时） */
#ifndef W25QXX_PROGRAM_TIMEOUT_MS
#define W25QXX_PROGRAM_TIMEOUT_MS       100U
#endif
#ifndef W25QXX_ERASE_TIMEOUT_MS
#define W25QXX_ERASE_TIMEOUT_MS        3000U
#endif
#ifndef W25QXX_CHIP_ERASE_TIMEOUT_MS
#define W25QXX_CHIP_ERASE_TIMEOUT_MS 200000U
#endif

/* 指令集定义 */
#define W25X_WriteEnable          0x06
#define W25X_WriteDisable         0x04
#define W25X_ReadStatusReg        0x05
#define W25X_WriteStatusReg       0x01
#define W25X_ReadData             0x03
#define W25X_FastReadData         0x0B
#define W25X_PageProgram          0x02
#define W25X_SectorErase          0x20
#define W25X_ChipErase            0xC7
#define W25X_PowerDown            0xB9
#define W25X_ReleasePowerDown     0xAB
#define W25X_ManufactDeviceID     0x90
#define W25X_JedecDeviceID        0x9F

/**
 * @brief 抽象 SPI 总线接口（MCU 无关）
 * @note  由 port 层实现并填充，驱动只通过回调访问总线与片选。
 *        所有带通信操作的回调均返回状态码：0 成功，负值表示失败。
 */
typedef struct {
    int      (*cs_low)(void *ctx);                              /* 片选有效（拉低），返回 0 成功，负值失败 */
    int      (*cs_high)(void *ctx);                             /* 片选释放（拉高），返回 0 成功，负值失败 */
    int      (*swap)(void *ctx, uint8_t tx, uint8_t *rx);       /* 全双工交换 1 字节，返回 0 成功，负值失败 */
    uint32_t (*millis)(void *ctx);                              /* 可选：毫秒计时，用于忙等待超时；NULL 表示不超时 */
    void    *ctx;                                               /* 传给上述回调的上下文 */

    /* 以下为可选批量回调：
       非 NULL 时命令后的数据段走批量传输（可用 DMA），NULL 则退回逐字节 swap。
       返回 0 成功；负值表示总线传输失败。 */
    int      (*read_bulk)(void *ctx, uint8_t *rx, uint32_t len);        /* 发送 0xFF 同时接收 */
    int      (*write_bulk)(void *ctx, const uint8_t *tx, uint32_t len); /* 只发不收 */
} W25QXX_Bus;

/** @brief 操作结果 */
typedef enum {
    W25QXX_OK            = 0,
    W25QXX_ERROR_PARAM   = -1,     /* 参数非法（如空指针、长度为 0、越界、溢出、未识别） */
    W25QXX_ERROR_TIMEOUT = -2,     /* 等待 BUSY 清零超时 */
    W25QXX_ERROR_BUS     = -3,     /* 总线传输失败（底层 SPI 错误/CS 失败/通信中断） */
    W25QXX_ERROR_WEL     = -4      /* 写使能 (WEL) 失败（多次重试未锁存，禁止破坏性操作） */
} W25QXX_Result;

/** @brief Flash 设备对象：只持有抽象总线与芯片信息 */
typedef struct {
    const W25QXX_Bus *bus;         /* 绑定的总线（port 层提供，生命周期需覆盖设备） */
    uint16_t          id;          /* 读取到的芯片 ID（0x0000/0xFFFF 表示未识别） */
    uint32_t          capacity;    /* 容量（字节），0 表示未知型号 */
    W25QXX_Result     last_result; /* 最近一次操作的结果 */
} W25QXX_Device;

/* ---------------- 函数声明 (全部带 Device 指针) ---------------- */

/**
 * @brief 绑定总线并读取芯片 ID
 * @param dev  设备句柄
 * @param bus  已实现回调的总线（内部不负责其初始化）
 * @return 1 识别到已知型号；0 参数非法或 ID 未识别（读写仍可尝试）
 */
uint8_t W25QXX_Init(W25QXX_Device *dev, const W25QXX_Bus *bus);

/* 基础信息 */
uint16_t      W25QXX_ReadID(W25QXX_Device *dev);
void          W25QXX_RefreshID(W25QXX_Device *dev);   /* 重读 ID 并同步容量 */
W25QXX_Result W25QXX_ReadStatusReg(W25QXX_Device *dev, uint8_t *status);
W25QXX_Result W25QXX_WriteEnable(W25QXX_Device *dev);

/* 读取操作 */
W25QXX_Result W25QXX_Read(W25QXX_Device *dev, uint8_t *pBuffer, uint32_t ReadAddr, uint32_t NumByteToRead);

/* 注意：破坏性覆盖写！此函数先擦除涉及的全部 4KB 扇区，再仅写入指定缓冲；
 * 该扇区其他地址的旧数据不会保留。若要安全局部更新，请自行先备份并
 * read-modify-erase-write，且应考虑掉电恢复。本工程 Demo 不调用此 API。 */
W25QXX_Result W25QXX_Write(W25QXX_Device *dev, const uint8_t *pBuffer, uint32_t WriteAddr, uint32_t NumByteToWrite);

/* 写入操作 (仅页写，不带擦除) */
W25QXX_Result W25QXX_Write_NoCheck(W25QXX_Device *dev, const uint8_t *pBuffer, uint32_t WriteAddr, uint32_t NumByteToWrite);

/* 擦除操作 */
W25QXX_Result W25QXX_EraseSector(W25QXX_Device *dev, uint32_t SectorAddr);
W25QXX_Result W25QXX_EraseChip(W25QXX_Device *dev);

/* 低功耗控制 */
W25QXX_Result W25QXX_PowerDown(W25QXX_Device *dev);
W25QXX_Result W25QXX_WakeUp(W25QXX_Device *dev);

#endif /* __W25QXX_H */
