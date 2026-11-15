/**
 * @file    w25qxx.c
 * @brief   W25Qxx SPI NOR Flash 协议实现（与 MCU 无关）
 * @details 仅通过 W25QXX_Bus 回调访问总线/片选，不直接操作任何寄存器。
 *          写/擦除返回 W25QXX_Result，忙等待带超时（需 port 提供 millis 回调）。
 */
#include "w25qxx.h"
#include "delay.h"
#include <stddef.h>

/* ====================================================================
 * 内部辅助函数
 * ==================================================================== */

/**
 * @brief  内部函数：片选拉低
 */
static inline void W25_CS_LOW(W25QXX_Device *dev) {
    dev->bus->cs_low(dev->bus->ctx);
}

/**
 * @brief  内部函数：片选拉高
 */
static inline void W25_CS_HIGH(W25QXX_Device *dev) {
    dev->bus->cs_high(dev->bus->ctx);
}

/**
 * @brief  内部函数：SPI 读写一个字节
 */
static inline uint8_t W25_SPI_Swap(W25QXX_Device *dev, uint8_t data) {
    return dev->bus->swap(dev->bus->ctx, data);
}

/**
 * @brief  内部函数：按芯片 ID 换算容量（字节），未知返回 0
 */
static uint32_t W25QXX_CapacityFromID(uint16_t id)
{
    switch (id) {
        case W25Q80:  return 1u * 1024u * 1024u;
        case W25Q16:  return 2u * 1024u * 1024u;
        case W25Q32:  return 4u * 1024u * 1024u;
        case W25Q64:  return 8u * 1024u * 1024u;
        case W25Q128: return 16u * 1024u * 1024u;
        default:      return 0;
    }
}

/**
 * @brief  内部函数：等待 BUSY 位清零（带超时）
 * @param  timeout_ms 超时时间（毫秒）；总线未提供 millis 回调时不超时
 */
static W25QXX_Result W25QXX_WaitBusy(W25QXX_Device *dev, uint32_t timeout_ms)
{
    uint32_t start_ms = 0;

    if (dev->bus->millis != NULL) {
        start_ms = dev->bus->millis(dev->bus->ctx);
    }

    for (;;) {
        uint8_t status;

        W25_CS_LOW(dev);
        W25_SPI_Swap(dev, W25X_ReadStatusReg);
        status = W25_SPI_Swap(dev, 0xFF);
        W25_CS_HIGH(dev);

        if ((status & 0x01U) == 0U) {
            return W25QXX_OK;
        }

        /* 无符号相减，天然处理计数回绕 */
        if (dev->bus->millis != NULL &&
            (dev->bus->millis(dev->bus->ctx) - start_ms) > timeout_ms) {
            W25_LOG("[W25Qxx] WaitBusy timeout (%lu ms)\r\n", (unsigned long)timeout_ms);
            return W25QXX_ERROR_TIMEOUT;
        }

        Delay_us(50);
    }
}

/**
 * @brief  内部函数：写使能 (WEL)
 */
static void W25QXX_WriteEnable(W25QXX_Device *dev) {
    for (uint8_t retries = 0; retries < 5; retries++) {
        W25_CS_LOW(dev);
        W25_SPI_Swap(dev, W25X_WriteEnable);
        W25_CS_HIGH(dev);

        /* 验证 WEL 是否已锁存 */
        W25_CS_LOW(dev);
        W25_SPI_Swap(dev, W25X_ReadStatusReg);
        uint8_t st = W25_SPI_Swap(dev, 0xFF);
        W25_CS_HIGH(dev);

        if ((st & 0x02U) != 0U) {
            break;
        }
        Delay_us(20);
    }
}

/**
 * @brief  内部函数：结果记录
 */
static W25QXX_Result W25_Return(W25QXX_Device *dev, W25QXX_Result result)
{
    dev->last_result = result;
    return result;
}

/* ====================================================================
 * 外部接口函数 implementation
 * ==================================================================== */

/**
 * @brief  绑定总线并读取芯片 ID
 * @param  dev: Flash 设备句柄
 * @param  bus: port 层实现好的总线回调表（需已完成总线/片选初始化）
 * @return 1 识别到已知型号；0 参数非法或 ID 未识别
 */
uint8_t W25QXX_Init(W25QXX_Device *dev, const W25QXX_Bus *bus)
{
    if (dev == NULL || bus == NULL || bus->cs_low == NULL ||
        bus->cs_high == NULL || bus->swap == NULL) {
        return 0;
    }

    /* 1. 绑定总线 */
    dev->bus = bus;
    dev->last_result = W25QXX_OK;

    /* 2. 默认不选中，等待上电稳定后读 ID */
    W25_CS_HIGH(dev);
    Delay_ms(20);
    W25QXX_RefreshID(dev);

    W25_LOG("[W25Qxx] Init Done. ID: 0x%04X, %lu bytes\r\n",
            dev->id, (unsigned long)dev->capacity);

    return (dev->capacity > 0) ? 1 : 0;
}

/**
 * @brief  读取芯片 ID
 * @return 16位 ID (如 0xEF17)
 */
uint16_t W25QXX_ReadID(W25QXX_Device *dev)
{
    uint16_t id = 0;

    if (dev == NULL || dev->bus == NULL) return 0;

    W25_CS_LOW(dev);
    W25_SPI_Swap(dev, W25X_ManufactDeviceID);
    W25_SPI_Swap(dev, 0x00); // Dummy
    W25_SPI_Swap(dev, 0x00); // Dummy
    W25_SPI_Swap(dev, 0x00); // Dummy

    id |= (uint16_t)W25_SPI_Swap(dev, 0xFF) << 8;
    id |= (uint16_t)W25_SPI_Swap(dev, 0xFF);

    W25_CS_HIGH(dev);
    return id;
}

/**
 * @brief  重读芯片 ID，并同步容量（换片/热插拔后调用）
 */
void W25QXX_RefreshID(W25QXX_Device *dev)
{
    if (dev == NULL || dev->bus == NULL) return;

    dev->id = W25QXX_ReadID(dev);
    dev->capacity = W25QXX_CapacityFromID(dev->id);
}

/**
 * @brief  读取数据
 * @param  dev: Flash 设备句柄
 * @param  pBuffer: 接收缓冲区
 * @param  ReadAddr: 读取地址
 * @param  NumByteToRead: 读取长度
 */
W25QXX_Result W25QXX_Read(W25QXX_Device *dev, uint8_t *pBuffer, uint32_t ReadAddr, uint16_t NumByteToRead)
{
    int bulk_ret = 0;

    if (dev == NULL || dev->bus == NULL || pBuffer == NULL || NumByteToRead == 0) {
        return W25QXX_ERROR_PARAM;
    }

    W25_CS_LOW(dev);
    W25_SPI_Swap(dev, W25X_ReadData);
    W25_SPI_Swap(dev, (uint8_t)((ReadAddr) >> 16));
    W25_SPI_Swap(dev, (uint8_t)((ReadAddr) >> 8));
    W25_SPI_Swap(dev, (uint8_t)ReadAddr);

    if (dev->bus->read_bulk != NULL) {
        /* 批量路径（DMA）：一次读完，避免逐字节中断/轮询开销 */
        bulk_ret = dev->bus->read_bulk(dev->bus->ctx, pBuffer, NumByteToRead);
    } else {
        for (uint16_t i = 0; i < NumByteToRead; i++) {
            pBuffer[i] = W25_SPI_Swap(dev, 0xFF);
        }
    }
    W25_CS_HIGH(dev);

    return W25_Return(dev, (bulk_ret == 0) ? W25QXX_OK : W25QXX_ERROR_BUS);
}

/**
 * @brief  页写入 (内部函数，最大 256 字节，不可跨页)
 */
static W25QXX_Result W25QXX_Write_Page(W25QXX_Device *dev, const uint8_t *pBuffer,
                                       uint32_t WriteAddr, uint16_t NumByteToWrite)
{
    int bulk_ret = 0;

    if (NumByteToWrite > W25QXX_PAGE_SIZE) NumByteToWrite = W25QXX_PAGE_SIZE;

    W25QXX_WriteEnable(dev);

    W25_CS_LOW(dev);
    W25_SPI_Swap(dev, W25X_PageProgram);
    W25_SPI_Swap(dev, (uint8_t)((WriteAddr) >> 16));
    W25_SPI_Swap(dev, (uint8_t)((WriteAddr) >> 8));
    W25_SPI_Swap(dev, (uint8_t)WriteAddr);

    if (dev->bus->write_bulk != NULL) {
        /* 批量路径（DMA）：页内数据一次发出 */
        bulk_ret = dev->bus->write_bulk(dev->bus->ctx, pBuffer, NumByteToWrite);
    } else {
        for (uint16_t i = 0; i < NumByteToWrite; i++) {
            W25_SPI_Swap(dev, pBuffer[i]);
        }
    }

    W25_CS_HIGH(dev);
    /* 等待页编程周期（tPP 典型 0.7ms，最大 3ms），避免过早拉低 CS 破坏编程 */
    Delay_ms(3);

    if (bulk_ret != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }
    return W25_Return(dev, W25QXX_WaitBusy(dev, W25QXX_PROGRAM_TIMEOUT_MS));
}

/**
 * @brief  无检验写 (自动处理翻页，但不处理擦除)
 * 必须确保写入地址范围内的数据是 0xFF
 */
W25QXX_Result W25QXX_Write_NoCheck(W25QXX_Device *dev, const uint8_t *pBuffer,
                                   uint32_t WriteAddr, uint16_t NumByteToWrite)
{
    uint16_t pageremain;
    W25QXX_Result result;

    if (dev == NULL || dev->bus == NULL || pBuffer == NULL || NumByteToWrite == 0) {
        return dev ? W25_Return(dev, W25QXX_ERROR_PARAM) : W25QXX_ERROR_PARAM;
    }

    pageremain = W25QXX_PAGE_SIZE - (WriteAddr % W25QXX_PAGE_SIZE); // 单页剩余的字节数
    if (NumByteToWrite <= pageremain) pageremain = NumByteToWrite;

    for (;;) {
        result = W25QXX_Write_Page(dev, pBuffer, WriteAddr, pageremain);
        if (result != W25QXX_OK) return result;

        if (pageremain == NumByteToWrite) break;

        pBuffer += pageremain;
        WriteAddr += pageremain;
        NumByteToWrite -= pageremain;
        pageremain = (NumByteToWrite > W25QXX_PAGE_SIZE) ? W25QXX_PAGE_SIZE : NumByteToWrite;
    }

    return W25QXX_OK;
}

/**
 * @brief  擦除一个扇区 (4KB)
 */
W25QXX_Result W25QXX_EraseSector(W25QXX_Device *dev, uint32_t SectorAddr)
{
    if (dev == NULL || dev->bus == NULL) {
        return dev ? W25_Return(dev, W25QXX_ERROR_PARAM) : W25QXX_ERROR_PARAM;
    }

    W25_LOG("[W25Qxx] Erase Sector: 0x%X\r\n", SectorAddr);

    /* 先等上一次操作结束，避免 WEL 被清除 */
    if (W25QXX_WaitBusy(dev, W25QXX_ERASE_TIMEOUT_MS) != W25QXX_OK) {
        return W25_Return(dev, W25QXX_ERROR_TIMEOUT);
    }
    W25QXX_WriteEnable(dev);

    W25_CS_LOW(dev);
    W25_SPI_Swap(dev, W25X_SectorErase);
    W25_SPI_Swap(dev, (uint8_t)((SectorAddr) >> 16));
    W25_SPI_Swap(dev, (uint8_t)((SectorAddr) >> 8));
    W25_SPI_Swap(dev, (uint8_t)SectorAddr);
    W25_CS_HIGH(dev);
    /* 等待扇区擦除主要周期（典型 45ms，最大 400ms），避免过早轮询 */
    Delay_ms(40);

    return W25_Return(dev, W25QXX_WaitBusy(dev, W25QXX_ERASE_TIMEOUT_MS));
}

/**
 * @brief  全片擦除
 */
W25QXX_Result W25QXX_EraseChip(W25QXX_Device *dev)
{
    if (dev == NULL || dev->bus == NULL) {
        return dev ? W25_Return(dev, W25QXX_ERROR_PARAM) : W25QXX_ERROR_PARAM;
    }

    W25_LOG("[W25Qxx] Erase Chip Start...\r\n");

    if (W25QXX_WaitBusy(dev, W25QXX_ERASE_TIMEOUT_MS) != W25QXX_OK) {
        return W25_Return(dev, W25QXX_ERROR_TIMEOUT);
    }
    W25QXX_WriteEnable(dev);

    W25_CS_LOW(dev);
    W25_SPI_Swap(dev, W25X_ChipErase);
    W25_CS_HIGH(dev);

    W25QXX_Result result = W25QXX_WaitBusy(dev, W25QXX_CHIP_ERASE_TIMEOUT_MS);
    if (result == W25QXX_OK) {
        W25_LOG("[W25Qxx] Erase Chip Done.\r\n");
    }
    return W25_Return(dev, result);
}

/**
 * @brief  带擦除的写操作 (模拟 EEPROM 行为)
 * 1. 计算需要擦除的扇区
 * 2. 擦除扇区
 * 3. 写入数据
 * 注意：这种暴力擦除会丢失同扇区其他数据，严谨做法是
 * 读出扇区->修改->擦除->回写。
 */
W25QXX_Result W25QXX_Write(W25QXX_Device *dev, const uint8_t *pBuffer,
                           uint32_t WriteAddr, uint16_t NumByteToWrite)
{
    uint32_t addr = WriteAddr;
    uint32_t len = NumByteToWrite;

    if (dev == NULL || dev->bus == NULL || pBuffer == NULL || len == 0) {
        return dev ? W25_Return(dev, W25QXX_ERROR_PARAM) : W25QXX_ERROR_PARAM;
    }

    uint32_t secStart = addr / W25QXX_SECTOR_SIZE;
    uint32_t secEnd   = (addr + len - 1) / W25QXX_SECTOR_SIZE;

    for (uint32_t sec = secStart; sec <= secEnd; sec++) {
        W25QXX_Result result = W25QXX_EraseSector(dev, sec * W25QXX_SECTOR_SIZE);
        if (result != W25QXX_OK) return result;
    }

    return W25QXX_Write_NoCheck(dev, pBuffer, addr, NumByteToWrite);
}

/**
 * @brief  进入掉电模式
 */
void W25QXX_PowerDown(W25QXX_Device *dev)
{
    if (dev == NULL || dev->bus == NULL) return;

    W25_CS_LOW(dev);
    W25_SPI_Swap(dev, W25X_PowerDown);
    W25_CS_HIGH(dev);
    Delay_ms(1);
}

/**
 * @brief  唤醒
 */
void W25QXX_WakeUp(W25QXX_Device *dev)
{
    if (dev == NULL || dev->bus == NULL) return;

    W25_CS_LOW(dev);
    W25_SPI_Swap(dev, W25X_ReleasePowerDown);
    W25_CS_HIGH(dev);
    Delay_ms(1);
}
