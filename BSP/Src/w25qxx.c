/**
 * @file    w25qxx.c
 * @brief   W25Qxx SPI NOR Flash 协议实现（与 MCU 无关）
 * @details 仅通过 W25QXX_Bus 回调访问总线/片选，不直接操作任何寄存器。
 *          写/擦除返回 W25QXX_Result，忙等待带超时（需 port 提供 millis 回调）。
 *          Stage 1.2-B: 严格检查片选释放 (W25_CS_HIGH) 返回值，确保释放失败不误报成功。
 */
#include "w25qxx.h"
#include "delay.h"
#include <stddef.h>

/* ====================================================================
 * 内部辅助函数
 * ==================================================================== */

/**
 * @brief  内部函数：结果记录
 */
static inline W25QXX_Result W25_Return(W25QXX_Device *dev, W25QXX_Result result)
{
    if (dev != NULL) {
        dev->last_result = result;
    }
    return result;
}

/**
 * @brief  内部函数：片选拉低（检查底层返回值）
 */
static inline int W25_CS_LOW(W25QXX_Device *dev)
{
    if (dev == NULL || dev->bus == NULL || dev->bus->cs_low == NULL) {
        return -1;
    }
    return dev->bus->cs_low(dev->bus->ctx);
}

/**
 * @brief  内部函数：片选拉高（检查底层返回值）
 */
static inline int W25_CS_HIGH(W25QXX_Device *dev)
{
    if (dev == NULL || dev->bus == NULL || dev->bus->cs_high == NULL) {
        return -1;
    }
    return dev->bus->cs_high(dev->bus->ctx);
}

/**
 * @brief  内部函数：SPI 读写一个字节（全双工交换）
 */
static inline int W25_SPI_Swap(W25QXX_Device *dev, uint8_t tx, uint8_t *rx)
{
    if (dev == NULL || dev->bus == NULL || dev->bus->swap == NULL) {
        return -1;
    }
    return dev->bus->swap(dev->bus->ctx, tx, rx);
}

/**
 * @brief  统一地址与参数合法性检查
 * @param  dev: Flash 设备
 * @param  addr: 起始地址
 * @param  len: 访问长度
 * @param  check_sector_align: 1 要求 4KB 扇区对齐（擦除用），0 不要求
 */
static W25QXX_Result W25QXX_CheckAddress(const W25QXX_Device *dev, uint32_t addr, uint32_t len, uint8_t check_sector_align)
{
    if (dev == NULL || dev->bus == NULL) {
        return W25QXX_ERROR_PARAM;
    }
    /* 设备已成功识别，capacity 不为 0 */
    if (dev->capacity == 0U) {
        return W25QXX_ERROR_PARAM;
    }
    /* 长度不能为 0 */
    if (len == 0U) {
        return W25QXX_ERROR_PARAM;
    }
    /* 地址小于实际 Flash 容量，且地址加长度不能超过容量（防无符号整数溢出） */
    if (addr >= dev->capacity || len > (dev->capacity - addr)) {
        return W25QXX_ERROR_PARAM;
    }
    /* 24 位地址防隐式截断：W25Q128 为 16MB，地址范围严格小于 0x01000000 */
    if (addr >= 0x01000000U || (addr + len) > 0x01000000U) {
        return W25QXX_ERROR_PARAM;
    }
    /* 扇区擦除对齐检查 */
    if (check_sector_align && (addr % W25QXX_SECTOR_SIZE) != 0U) {
        return W25QXX_ERROR_PARAM;
    }
    return W25QXX_OK;
}

/**
 * @brief  内部函数：按芯片 ID 换算容量（字节），未知返回 0
 */
static uint32_t W25QXX_CapacityFromID(uint16_t id)
{
    switch (id) {
        case W25Q80:  return 1U * 1024U * 1024U;
        case W25Q16:  return 2U * 1024U * 1024U;
        case W25Q32:  return 4U * 1024U * 1024U;
        case W25Q64:  return 8U * 1024U * 1024U;
        case W25Q128: return 16U * 1024U * 1024U;
        default:      return 0;
    }
}

/**
 * @brief  读取 Flash 状态寄存器 1 (05h)
 */
W25QXX_Result W25QXX_ReadStatusReg(W25QXX_Device *dev, uint8_t *status)
{
    uint8_t dummy = 0xFFU;
    uint8_t st = 0;

    if (dev == NULL || dev->bus == NULL || status == NULL) {
        return W25_Return(dev, W25QXX_ERROR_PARAM);
    }

    if (W25_CS_LOW(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    if (W25_SPI_Swap(dev, W25X_ReadStatusReg, &dummy) != 0 ||
        W25_SPI_Swap(dev, 0xFFU, &st) != 0) {
        (void)W25_CS_HIGH(dev);
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    if (W25_CS_HIGH(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    *status = st;
    return W25QXX_OK;
}

/**
 * @brief  内部函数：等待 BUSY 位清零（带超时与底层错误检测）
 * @param  timeout_ms 超时时间（毫秒）；总线未提供 millis 回调时不超时
 */
static W25QXX_Result W25QXX_WaitBusy(W25QXX_Device *dev, uint32_t timeout_ms)
{
    uint32_t start_ms = 0;

    if (dev == NULL || dev->bus == NULL) {
        return W25_Return(dev, W25QXX_ERROR_PARAM);
    }

    if (dev->bus->millis != NULL) {
        start_ms = dev->bus->millis(dev->bus->ctx);
    }

    for (;;) {
        uint8_t status = 0;
        W25QXX_Result res = W25QXX_ReadStatusReg(dev, &status);
        if (res != W25QXX_OK) {
            /* 底层总线/SPI 错误，绝不能误判为完成！直接返回错误 */
            return W25_Return(dev, res);
        }

        if ((status & 0x01U) == 0U) {
            return W25QXX_OK;
        }

        /* 无符号相减，天然处理计数回绕 */
        if (dev->bus->millis != NULL &&
            (dev->bus->millis(dev->bus->ctx) - start_ms) > timeout_ms) {
            W25_LOG("[W25Qxx] WaitBusy timeout (%lu ms)\r\n", (unsigned long)timeout_ms);
            return W25_Return(dev, W25QXX_ERROR_TIMEOUT);
        }

        Delay_us(50);
    }
}

/**
 * @brief  写使能 (WEL) 并验证状态寄存器锁存
 * @return W25QXX_OK 成功；W25QXX_ERROR_WEL 重试失败；W25QXX_ERROR_BUS 通信异常
 */
W25QXX_Result W25QXX_WriteEnable(W25QXX_Device *dev)
{
    if (dev == NULL || dev->bus == NULL) {
        return W25_Return(dev, W25QXX_ERROR_PARAM);
    }

    for (uint8_t retries = 0; retries < 5; retries++) {
        uint8_t dummy = 0xFFU;
        uint8_t st = 0;

        if (W25_CS_LOW(dev) != 0) {
            return W25_Return(dev, W25QXX_ERROR_BUS);
        }
        if (W25_SPI_Swap(dev, W25X_WriteEnable, &dummy) != 0) {
            (void)W25_CS_HIGH(dev);
            return W25_Return(dev, W25QXX_ERROR_BUS);
        }
        if (W25_CS_HIGH(dev) != 0) {
            return W25_Return(dev, W25QXX_ERROR_BUS);
        }

        /* 验证 WEL 是否已锁存 (bit 1) */
        W25QXX_Result res = W25QXX_ReadStatusReg(dev, &st);
        if (res != W25QXX_OK) {
            return W25_Return(dev, res);
        }

        if ((st & 0x02U) != 0U) {
            return W25QXX_OK;
        }
        Delay_us(20);
    }

    return W25_Return(dev, W25QXX_ERROR_WEL);
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
    (void)W25_CS_HIGH(dev);
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
    uint8_t dummy = 0xFFU;
    uint8_t id_hi = 0;
    uint8_t id_lo = 0;

    if (dev == NULL || dev->bus == NULL) return 0;

    if (W25_CS_LOW(dev) != 0) return 0;

    if (W25_SPI_Swap(dev, W25X_ManufactDeviceID, &dummy) != 0 ||
        W25_SPI_Swap(dev, 0x00U, &dummy) != 0 ||
        W25_SPI_Swap(dev, 0x00U, &dummy) != 0 ||
        W25_SPI_Swap(dev, 0x00U, &dummy) != 0 ||
        W25_SPI_Swap(dev, 0xFFU, &id_hi) != 0 ||
        W25_SPI_Swap(dev, 0xFFU, &id_lo) != 0) {
        (void)W25_CS_HIGH(dev);
        return 0;
    }

    if (W25_CS_HIGH(dev) != 0) {
        return 0;
    }
    return ((uint16_t)id_hi << 8) | id_lo;
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
W25QXX_Result W25QXX_Read(W25QXX_Device *dev, uint8_t *pBuffer, uint32_t ReadAddr, uint32_t NumByteToRead)
{
    uint8_t dummy = 0xFFU;
    W25QXX_Result chk;

    if (pBuffer == NULL) {
        return W25_Return(dev, W25QXX_ERROR_PARAM);
    }
    chk = W25QXX_CheckAddress(dev, ReadAddr, NumByteToRead, 0);
    if (chk != W25QXX_OK) {
        return W25_Return(dev, chk);
    }

    if (W25_CS_LOW(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    if (W25_SPI_Swap(dev, W25X_ReadData, &dummy) != 0 ||
        W25_SPI_Swap(dev, (uint8_t)(ReadAddr >> 16), &dummy) != 0 ||
        W25_SPI_Swap(dev, (uint8_t)(ReadAddr >> 8), &dummy) != 0 ||
        W25_SPI_Swap(dev, (uint8_t)ReadAddr, &dummy) != 0) {
        (void)W25_CS_HIGH(dev);
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    if (dev->bus->read_bulk != NULL) {
        /* 批量路径（DMA）：一次读完，避免逐字节中断/轮询开销 */
        if (dev->bus->read_bulk(dev->bus->ctx, pBuffer, NumByteToRead) != 0) {
            (void)W25_CS_HIGH(dev);
            return W25_Return(dev, W25QXX_ERROR_BUS);
        }
    } else {
        for (uint32_t i = 0; i < NumByteToRead; i++) {
            if (W25_SPI_Swap(dev, 0xFFU, &pBuffer[i]) != 0) {
                (void)W25_CS_HIGH(dev);
                return W25_Return(dev, W25QXX_ERROR_BUS);
            }
        }
    }

    if (W25_CS_HIGH(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    return W25_Return(dev, W25QXX_OK);
}

/**
 * @brief  页写入 (内部函数，最大 256 字节，不可跨页)
 */
static W25QXX_Result W25QXX_Write_Page(W25QXX_Device *dev, const uint8_t *pBuffer,
                                       uint32_t WriteAddr, uint32_t NumByteToWrite)
{
    uint8_t dummy = 0xFFU;
    W25QXX_Result res;

    if (pBuffer == NULL) {
        return W25_Return(dev, W25QXX_ERROR_PARAM);
    }
    res = W25QXX_CheckAddress(dev, WriteAddr, NumByteToWrite, 0);
    if (res != W25QXX_OK) {
        return W25_Return(dev, res);
    }

    /* 页写入不可跨页 */
    if (NumByteToWrite > W25QXX_PAGE_SIZE) {
        return W25_Return(dev, W25QXX_ERROR_PARAM);
    }
    uint32_t page_offset = WriteAddr % W25QXX_PAGE_SIZE;
    if (page_offset + NumByteToWrite > W25QXX_PAGE_SIZE) {
        return W25_Return(dev, W25QXX_ERROR_PARAM);
    }

    /* 必须先验证写使能 WEL */
    res = W25QXX_WriteEnable(dev);
    if (res != W25QXX_OK) {
        return W25_Return(dev, res);
    }

    if (W25_CS_LOW(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    if (W25_SPI_Swap(dev, W25X_PageProgram, &dummy) != 0 ||
        W25_SPI_Swap(dev, (uint8_t)(WriteAddr >> 16), &dummy) != 0 ||
        W25_SPI_Swap(dev, (uint8_t)(WriteAddr >> 8), &dummy) != 0 ||
        W25_SPI_Swap(dev, (uint8_t)WriteAddr, &dummy) != 0) {
        (void)W25_CS_HIGH(dev);
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    if (dev->bus->write_bulk != NULL) {
        /* 批量路径（DMA）：页内数据一次发出 */
        if (dev->bus->write_bulk(dev->bus->ctx, pBuffer, NumByteToWrite) != 0) {
            (void)W25_CS_HIGH(dev);
            return W25_Return(dev, W25QXX_ERROR_BUS);
        }
    } else {
        for (uint32_t i = 0; i < NumByteToWrite; i++) {
            if (W25_SPI_Swap(dev, pBuffer[i], &dummy) != 0) {
                (void)W25_CS_HIGH(dev);
                return W25_Return(dev, W25QXX_ERROR_BUS);
            }
        }
    }

    /* 正常传输完毕，严格检查片选释放 */
    if (W25_CS_HIGH(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    /* 等待页编程周期（tPP 典型 0.7ms，最大 3ms），避免过早拉低 CS 破坏编程 */
    Delay_ms(3);

    return W25_Return(dev, W25QXX_WaitBusy(dev, W25QXX_PROGRAM_TIMEOUT_MS));
}

/**
 * @brief  无检验写 (自动处理翻页，但不处理擦除)
 * 必须确保写入地址范围内的数据是 0xFF
 */
W25QXX_Result W25QXX_Write_NoCheck(W25QXX_Device *dev, const uint8_t *pBuffer,
                                   uint32_t WriteAddr, uint32_t NumByteToWrite)
{
    W25QXX_Result chk;

    if (pBuffer == NULL) {
        return W25_Return(dev, W25QXX_ERROR_PARAM);
    }
    chk = W25QXX_CheckAddress(dev, WriteAddr, NumByteToWrite, 0);
    if (chk != W25QXX_OK) {
        return W25_Return(dev, chk);
    }

    uint32_t pageremain = W25QXX_PAGE_SIZE - (WriteAddr % W25QXX_PAGE_SIZE);
    if (NumByteToWrite <= pageremain) {
        pageremain = NumByteToWrite;
    }

    for (;;) {
        W25QXX_Result result = W25QXX_Write_Page(dev, pBuffer, WriteAddr, (uint16_t)pageremain);
        if (result != W25QXX_OK) {
            return W25_Return(dev, result);
        }

        if (pageremain == NumByteToWrite) break;

        pBuffer += pageremain;
        WriteAddr += pageremain;
        NumByteToWrite -= pageremain;
        pageremain = (NumByteToWrite > W25QXX_PAGE_SIZE) ? W25QXX_PAGE_SIZE : NumByteToWrite;
    }

    return W25_Return(dev, W25QXX_OK);
}

/**
 * @brief  擦除一个扇区 (4KB)
 */
W25QXX_Result W25QXX_EraseSector(W25QXX_Device *dev, uint32_t SectorAddr)
{
    uint8_t dummy = 0xFFU;
    W25QXX_Result chk = W25QXX_CheckAddress(dev, SectorAddr, W25QXX_SECTOR_SIZE, 1);
    if (chk != W25QXX_OK) {
        return W25_Return(dev, chk);
    }

    W25_LOG("[W25Qxx] Erase Sector: 0x%lX\r\n", (unsigned long)SectorAddr);

    /* 先等上一次操作结束，避免 WEL 被清除 */
    W25QXX_Result busy_res = W25QXX_WaitBusy(dev, W25QXX_ERASE_TIMEOUT_MS);
    if (busy_res != W25QXX_OK) {
        return W25_Return(dev, busy_res);
    }

    /* 必须先验证写使能 WEL */
    W25QXX_Result wel_res = W25QXX_WriteEnable(dev);
    if (wel_res != W25QXX_OK) {
        return W25_Return(dev, wel_res);
    }

    if (W25_CS_LOW(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    if (W25_SPI_Swap(dev, W25X_SectorErase, &dummy) != 0 ||
        W25_SPI_Swap(dev, (uint8_t)(SectorAddr >> 16), &dummy) != 0 ||
        W25_SPI_Swap(dev, (uint8_t)(SectorAddr >> 8), &dummy) != 0 ||
        W25_SPI_Swap(dev, (uint8_t)SectorAddr, &dummy) != 0) {
        (void)W25_CS_HIGH(dev);
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    if (W25_CS_HIGH(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    /* 等待扇区擦除主要周期（典型 45ms，最大 400ms），避免过早轮询 */
    Delay_ms(40);

    return W25_Return(dev, W25QXX_WaitBusy(dev, W25QXX_ERASE_TIMEOUT_MS));
}

/**
 * @brief  全片擦除
 */
W25QXX_Result W25QXX_EraseChip(W25QXX_Device *dev)
{
    uint8_t dummy = 0xFFU;

    if (dev == NULL || dev->bus == NULL || dev->capacity == 0U) {
        return W25_Return(dev, W25QXX_ERROR_PARAM);
    }

    W25_LOG("[W25Qxx] Erase Chip Start...\r\n");

    W25QXX_Result busy_res = W25QXX_WaitBusy(dev, W25QXX_ERASE_TIMEOUT_MS);
    if (busy_res != W25QXX_OK) {
        return W25_Return(dev, busy_res);
    }

    /* 必须先验证写使能 WEL */
    W25QXX_Result wel_res = W25QXX_WriteEnable(dev);
    if (wel_res != W25QXX_OK) {
        return W25_Return(dev, wel_res);
    }

    if (W25_CS_LOW(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    if (W25_SPI_Swap(dev, W25X_ChipErase, &dummy) != 0) {
        (void)W25_CS_HIGH(dev);
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    if (W25_CS_HIGH(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }

    W25QXX_Result result = W25QXX_WaitBusy(dev, W25QXX_CHIP_ERASE_TIMEOUT_MS);
    if (result == W25QXX_OK) {
        W25_LOG("[W25Qxx] Erase Chip Done.\r\n");
    }
    return W25_Return(dev, result);
}

/**
 * @brief  破坏性写操作：先擦除涉及的完整扇区，再写入给定数据。
 * @warning 同扇区不在本次写入范围内的原始数据将丢失！
 */
W25QXX_Result W25QXX_Write(W25QXX_Device *dev, const uint8_t *pBuffer,
                           uint32_t WriteAddr, uint32_t NumByteToWrite)
{
    if (pBuffer == NULL) {
        return W25_Return(dev, W25QXX_ERROR_PARAM);
    }
    W25QXX_Result chk = W25QXX_CheckAddress(dev, WriteAddr, NumByteToWrite, 0);
    if (chk != W25QXX_OK) {
        return W25_Return(dev, chk);
    }

    uint32_t secStart = WriteAddr / W25QXX_SECTOR_SIZE;
    uint32_t secEnd   = (WriteAddr + NumByteToWrite - 1U) / W25QXX_SECTOR_SIZE;

    for (uint32_t sec = secStart; sec <= secEnd; sec++) {
        W25QXX_Result result = W25QXX_EraseSector(dev, sec * W25QXX_SECTOR_SIZE);
        if (result != W25QXX_OK) {
            return W25_Return(dev, result);
        }
    }

    return W25QXX_Write_NoCheck(dev, pBuffer, WriteAddr, NumByteToWrite);
}

/**
 * @brief  进入掉电模式
 */
W25QXX_Result W25QXX_PowerDown(W25QXX_Device *dev)
{
    uint8_t dummy = 0xFFU;
    if (dev == NULL || dev->bus == NULL) return W25_Return(dev, W25QXX_ERROR_PARAM);

    if (W25_CS_LOW(dev) != 0) return W25_Return(dev, W25QXX_ERROR_BUS);
    if (W25_SPI_Swap(dev, W25X_PowerDown, &dummy) != 0) {
        (void)W25_CS_HIGH(dev);
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }
    if (W25_CS_HIGH(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }
    Delay_ms(1);
    return W25_Return(dev, W25QXX_OK);
}

/**
 * @brief  唤醒
 */
W25QXX_Result W25QXX_WakeUp(W25QXX_Device *dev)
{
    uint8_t dummy = 0xFFU;
    if (dev == NULL || dev->bus == NULL) return W25_Return(dev, W25QXX_ERROR_PARAM);

    if (W25_CS_LOW(dev) != 0) return W25_Return(dev, W25QXX_ERROR_BUS);
    if (W25_SPI_Swap(dev, W25X_ReleasePowerDown, &dummy) != 0) {
        (void)W25_CS_HIGH(dev);
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }
    if (W25_CS_HIGH(dev) != 0) {
        return W25_Return(dev, W25QXX_ERROR_BUS);
    }
    Delay_ms(1);
    return W25_Return(dev, W25QXX_OK);
}
