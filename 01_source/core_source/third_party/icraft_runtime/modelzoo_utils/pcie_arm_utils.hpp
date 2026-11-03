#pragma once
#ifndef _WIN32
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <mutex>
#include "spdlog/spdlog.h"

#include "icraft-xir/core/network.h"
#include "icraft-xir/core/data.h"
#include "icraft-xrt/dev/host_device.h"
#include "icraft-xrt/dev/buyi_device.h"
#include "icraft-xrt/dev/zg330_device.h"

#include "et_device.hpp"

struct init_info
{
    int buffer_num;
};

#define REG_BASE 0x80040C00 // gp1    // 3.0b ootbin

int irq_cnt = 0;

#define USLEE_TIME 50 // - ns

constexpr uint64_t REG_INIT = REG_BASE + 0x5C;  // init
constexpr uint64_t REG_INPUT = REG_BASE + 0x5C; // init

constexpr uint64_t REG_RES = REG_BASE + 0x60;
constexpr uint64_t REG_ADDR_LOW = REG_BASE + 0x64;
constexpr uint64_t REG_ADDR_HIGH = REG_BASE + 0x68;

// input regaster
constexpr uint64_t REG_0 = REG_BASE + 0x70;
constexpr uint64_t REG_1 = REG_BASE + 0x74;
constexpr uint64_t REG_2 = REG_BASE + 0x78;
constexpr uint64_t REG_3 = REG_BASE + 0x7c;
constexpr uint64_t REG_4 = REG_BASE + 0x80;
constexpr uint64_t REG_5 = REG_BASE + 0x84;
constexpr uint64_t REG_6 = REG_BASE + 0x88;
constexpr uint64_t REG_7 = REG_BASE + 0x8c;
constexpr uint64_t REG_8 = REG_BASE + 0x90;
constexpr uint64_t REG_9 = REG_BASE + 0x94;
constexpr uint64_t REG_10 = REG_BASE + 0x98;
constexpr uint64_t REG_11 = REG_BASE + 0x9c;

constexpr uint64_t REG_IRQ_0 = REG_BASE + 0xcc;
constexpr uint64_t REG_IRQ_1 = REG_BASE + 0xd0;
constexpr uint64_t REG_IRQ_2 = REG_BASE + 0xd4;
constexpr uint64_t REG_IRQ_3 = REG_BASE + 0xd8;
constexpr uint64_t REG_IRQ_4 = REG_BASE + 0xdc;
constexpr uint64_t REG_IRQ_5 = REG_BASE + 0xe0;
constexpr uint64_t REG_IRQ_6 = REG_BASE + 0xe4;
constexpr uint64_t REG_IRQ_7 = REG_BASE + 0xe8;

constexpr uint64_t REG_IRQ_8 = REG_BASE + 0xec;
constexpr uint64_t REG_IRQ_9 = REG_BASE + 0xf0;
constexpr uint64_t REG_IRQ_10 = REG_BASE + 0xf4;
constexpr uint64_t REG_IRQ_11 = REG_BASE + 0xf8;
constexpr uint64_t REG_IRQ_12 = REG_BASE + 0xfc;
constexpr uint64_t REG_IRQ_13 = REG_BASE + 0x100;
constexpr uint64_t REG_IRQ_14 = REG_BASE + 0x104;
constexpr uint64_t REG_IRQ_15 = REG_BASE + 0x108;

constexpr uint64_t REG_IRQ_RES = REG_BASE + 0xf8;

// sync flag
#define RESULT_FLAG_MASK 0x0FFFFFFF
#define HOST_RDY 0x10000000
#define HOST_DONE 0x20000000
#define ARM_RDY 0x40000000
#define ARM_DONE 0x80000000

template <typename DeviceType>
inline std::vector<icraft::xrt::MemChunk> initHostIn(
    DeviceType device,
    unsigned int buffer_num, unsigned int buffer_size)
{
    spdlog::info("[Arm] ready!");
    device.defaultRegRegion().write(REG_INIT, 0);
    spdlog::info("[Arm] Waiting connection from host!");
    spdlog::info(device.defaultRegRegion().read(REG_INIT));
    while (device.defaultRegRegion().read(REG_INIT) != HOST_RDY)
    {
        // spdlog::info(device.defaultRegRegion().read(REG_INIT));
        usleep(1000);
    }
    spdlog::info("[Arm] Start to connect with host!");

    device.defaultRegRegion().write(REG_INIT, ARM_DONE | buffer_num);

    std::vector<icraft::xrt::MemChunk> ret;
    for (int i = 0; i < buffer_num; ++i)
    {
        auto plddr_mem_chunk = device.getMemRegion("plddr").malloc(buffer_size, false);
        while ((device.defaultRegRegion().read(REG_INIT) & HOST_RDY) != HOST_RDY)
        {
            usleep(1000);
        }

        device.defaultRegRegion().write(REG_ADDR_LOW, (plddr_mem_chunk->begin.addr() & 0xFFFFFFFF));
        device.defaultRegRegion().write(REG_ADDR_HIGH, (plddr_mem_chunk->begin.addr() >> 32));
        device.defaultRegRegion().write(REG_INIT, ARM_DONE);
        ret.push_back(plddr_mem_chunk);
    }

    // usleep(1000);
    device.defaultRegRegion().write(REG_INIT, ARM_DONE);

    spdlog::info("[Arm] connection with host success, buffer num={}\n", buffer_num);

    return ret;
}

template <typename DeviceType>
inline std::vector<icraft::xrt::MemChunk> initHostIn_psddr(
    DeviceType device,
    unsigned int buffer_num, unsigned int buffer_size)
{
    spdlog::info("[Arm] Waiting connection from host!");
    // spdlog::info("{} {} {}", REG_INIT, REG_ADDR_LOW, REG_ADDR_HIGH);
    device.defaultRegRegion().write(REG_INIT, 0);
    while (device.defaultRegRegion().read(REG_INIT) != HOST_RDY)
    {
        // spdlog::info("[Arm] REG_INIT {}", device.defaultRegRegion().read(REG_INIT));
        usleep(1000);
    }
    spdlog::info("[Arm] Start to connect with host!");

    device.defaultRegRegion().write(REG_INIT, ARM_DONE | buffer_num);

    std::vector<icraft::xrt::MemChunk> ret;
    for (int i = 0; i < buffer_num; ++i)
    {
        // send psddr addr
        auto psddr_mem_chunk = device.getMemRegion("udma").malloc(buffer_size, false);
        while ((device.defaultRegRegion().read(REG_INIT) & HOST_RDY) != HOST_RDY)
        {
            usleep(1000);
        }

        device.defaultRegRegion().write(REG_ADDR_LOW, (psddr_mem_chunk->begin.addr() & 0xFFFFFFFF));
        device.defaultRegRegion().write(REG_ADDR_HIGH, (psddr_mem_chunk->begin.addr() >> 32));
        device.defaultRegRegion().write(REG_INIT, ARM_DONE);
        ret.push_back(psddr_mem_chunk);
    }

    // usleep(1000);
    device.defaultRegRegion().write(REG_INIT, ARM_DONE);

    spdlog::info("[Arm] connection with host success, buffer num={}\n", buffer_num);

    return ret;
}

template <typename DeviceType>
inline std::vector<icraft::xrt::MemChunk> initHostIn_psddr_reg(
    DeviceType device,
    unsigned int buffer_num, unsigned int buffer_size,
    const uint64_t res_reg, const uint64_t res_reg_low, const uint64_t res_reg_high)
{
    spdlog::info("[Arm] Waiting connection from host!");
    // spdlog::info("res_reg {} res_reg_low {} res_reg_high {}", res_reg, res_reg_low, res_reg_high);
    device.defaultRegRegion().write(res_reg, 0);
    while (device.defaultRegRegion().read(res_reg) != HOST_RDY)
    {
        // spdlog::info("[Arm] REG_INIT {}", device.defaultRegRegion().read(REG_INIT));
        usleep(1000);
    }
    spdlog::info("[Arm] Start to connect with host!");

    device.defaultRegRegion().write(res_reg, ARM_DONE | buffer_num);

    std::vector<icraft::xrt::MemChunk> ret;
    for (int i = 0; i < buffer_num; ++i)
    {
        // send psddr addr
        auto psddr_mem_chunk = device.getMemRegion("udma").malloc(buffer_size, false);
        while ((device.defaultRegRegion().read(res_reg) & HOST_RDY) != HOST_RDY)
        {
            usleep(1000);
        }

        device.defaultRegRegion().write(res_reg_low, (psddr_mem_chunk->begin.addr() & 0xFFFFFFFF));
        device.defaultRegRegion().write(res_reg_high, (psddr_mem_chunk->begin.addr() >> 32));
        device.defaultRegRegion().write(res_reg, ARM_DONE);
        ret.push_back(psddr_mem_chunk);
    }

    // usleep(1000);
    device.defaultRegRegion().write(res_reg, ARM_DONE);

    spdlog::info("[Arm] connection with host success, buffer num={}\n", buffer_num);

    return ret;
}


inline std::vector<icraft::xrt::MemChunk> initHostIn_chunk(
    const icraft::xrt::Device &device,
    unsigned int buffer_num, unsigned int buffer_size,
    const std::string region = "udma",
    const uint64_t res_reg = REG_INIT,
    const uint64_t res_reg_low = REG_ADDR_LOW,
    const uint64_t res_reg_high = REG_ADDR_HIGH)
{
    spdlog::info("[Arm] Waiting connection from host!");
    // spdlog::info("res_reg {} res_reg_low {} res_reg_high {}", res_reg, res_reg_low, res_reg_high);
    device.defaultRegRegion().write(res_reg, 0);
    while (device.defaultRegRegion().read(res_reg) != HOST_RDY)
    {
        // spdlog::info("[Arm] REG_INIT {}", device.defaultRegRegion().read(REG_INIT));
        usleep(1000);
    }
    spdlog::info("[Arm] Start to connect with host!");

    device.defaultRegRegion().write(res_reg, ARM_DONE | buffer_num);

    std::vector<icraft::xrt::MemChunk> ret;
    for (int i = 0; i < buffer_num; ++i)
    {
        // send psddr addr
        auto ddr_mem_chunk = device.getMemRegion(region).malloc(buffer_size, false);
        while ((device.defaultRegRegion().read(res_reg) & HOST_RDY) != HOST_RDY)
        {
            usleep(1000);
        }

        device.defaultRegRegion().write(res_reg_low, (ddr_mem_chunk->begin.addr() & 0xFFFFFFFF));
        device.defaultRegRegion().write(res_reg_high, (ddr_mem_chunk->begin.addr() >> 32));
        device.defaultRegRegion().write(res_reg, ARM_DONE);
        ret.push_back(ddr_mem_chunk);
    }

    // usleep(1000);
    device.defaultRegRegion().write(res_reg, ARM_DONE);

    spdlog::info("[Arm] connection with host success, buffer num={}\n", buffer_num);

    return ret;
}

void assignHostIn_chunk(
    const icraft::xrt::Device &device,
    unsigned int buffer_num, unsigned int buffer_size,
    std::vector<icraft::xrt::MemChunk> ret,
    const std::string region = "plddr",
    const uint64_t res_reg = REG_INIT,
    const uint64_t res_reg_low = REG_ADDR_LOW,
    const uint64_t res_reg_high = REG_ADDR_HIGH)
{
    spdlog::info("[Arm] Waiting connection from host!");
    // spdlog::info("res_reg {} res_reg_low {} res_reg_high {}", res_reg, res_reg_low, res_reg_high);
    device.defaultRegRegion().write(res_reg, 0);
    while (device.defaultRegRegion().read(res_reg) != HOST_RDY)
    {
        // spdlog::info("[Arm] REG_INIT {}", device.defaultRegRegion().read(REG_INIT));
        usleep(1000);
    }
    spdlog::info("[Arm] Start to connect with host!");

    device.defaultRegRegion().write(res_reg, ARM_DONE | buffer_num);

    for (int i = 0; i < buffer_num; ++i)
    {
        // send chunk addr
        auto ddr_mem_chunk = ret[i];
        spdlog::info("assignHostIn_chunk addr {}", ddr_mem_chunk->begin.addr());
        while ((device.defaultRegRegion().read(res_reg) & HOST_RDY) != HOST_RDY)
        {
            usleep(1000);
        }

        device.defaultRegRegion().write(res_reg_low, (ddr_mem_chunk->begin.addr() & 0xFFFFFFFF));
        device.defaultRegRegion().write(res_reg_high, (ddr_mem_chunk->begin.addr() >> 32));
        device.defaultRegRegion().write(res_reg, ARM_DONE);
        ret.push_back(ddr_mem_chunk);
    }

    // usleep(1000);
    device.defaultRegRegion().write(res_reg, ARM_DONE);

    spdlog::info("[Arm] connection with host success, buffer num={}\n", buffer_num);

}

template <typename T, typename DeviceType>
inline void sentResultToHost(
    const std::vector<T> &results,
    DeviceType device)
{

    while ((device.defaultRegRegion().read(REG_RES) & HOST_RDY) != HOST_RDY)
    {
        usleep(USLEE_TIME);
    }

    if (results.size() == 0)
    {
        device.defaultRegRegion().write(REG_RES, ARM_DONE | 0);
        return;
    }

    auto udma_mem_chunk = device.getMemRegion("udma").malloc(sizeof(T) * results.size(), false);

    device.defaultRegRegion().write(REG_ADDR_LOW, (udma_mem_chunk->begin.addr() & 0xFFFFFFFF));
    device.defaultRegRegion().write(REG_ADDR_HIGH, (udma_mem_chunk->begin.addr() >> 32));

    udma_mem_chunk.write(0, (char *)results.data(), sizeof(T) * results.size());
    device.defaultRegRegion().write(REG_RES, ARM_DONE | results.size());

    udma_mem_chunk.free();
}

template <typename DeviceType>
inline void sentVPUresultToHost(
    void *data_ptr,
    int data_bytesize,
    DeviceType device)
{
    // spdlog::info("[arm] Waiting send connection from host!");
    while ((device.defaultRegRegion().read(REG_RES) & HOST_RDY) != HOST_RDY)
    {
        usleep(USLEE_TIME);
    }
    // spdlog::info("[arm] send connected from host!");
    if (data_bytesize == 0)
    {
        device.defaultRegRegion().write(REG_RES, ARM_DONE | 0);
        return;
    }

    auto udma_mem_chunk = device.getMemRegion("udma").malloc(data_bytesize, false);
    // spdlog::info("[arm] udma_addr: {}", udma_mem_chunk->begin.addr());
    device.defaultRegRegion().write(REG_ADDR_LOW, (udma_mem_chunk->begin.addr() & 0xFFFFFFFF));
    device.defaultRegRegion().write(REG_ADDR_HIGH, (udma_mem_chunk->begin.addr() >> 32));

    udma_mem_chunk.write(0, (char *)data_ptr, data_bytesize);
    device.defaultRegRegion().write(REG_RES, ARM_DONE | data_bytesize);

    udma_mem_chunk.free();
}

template <typename DeviceType>
inline void sentVPUresultToHost_multistream(
    void *data_ptr,
    int data_bytesize,
    DeviceType device,
    uint64_t res_reg,
    uint64_t res_reg_low,
    uint64_t res_reg_high)
{
    // spdlog::info("[arm] Waiting send connection from host!");
    while ((device.defaultRegRegion().read(res_reg) & HOST_RDY) != HOST_RDY)
    {
        usleep(USLEE_TIME);
    }
    // spdlog::info("[arm] send connected from host!");
    if (data_bytesize == 0)
    {
        device.defaultRegRegion().write(res_reg, ARM_DONE | 0);
        return;
    }

    auto udma_mem_chunk = device.getMemRegion("udma").malloc(data_bytesize, false);
    spdlog::info("[arm] udma_addr: {}, datasize: {}", udma_mem_chunk->begin.addr(), data_bytesize);
    device.defaultRegRegion().write(res_reg_low, (udma_mem_chunk->begin.addr() & 0xFFFFFFFF));
    device.defaultRegRegion().write(res_reg_high, (udma_mem_chunk->begin.addr() >> 32));

    udma_mem_chunk.write(0, (char *)data_ptr, data_bytesize);
    device.defaultRegRegion().write(res_reg, ARM_DONE | data_bytesize);

    udma_mem_chunk.free();
}

template <typename DeviceType>
inline bool waitIrqRdy(DeviceType device, int irq_index)
{
    uint32_t ret = device.defaultRegRegion().read(REG_BASE + 0x10c);

    return (ret >> irq_index) & 0x1;
}

template <typename DeviceType>
inline void sentUdmaImgToHost(
    icraft::xrt::MemChunk chunck,
    DeviceType device)
{
    while ((device.defaultRegRegion().read(REG_RES) & HOST_RDY) != HOST_RDY)
    {
        usleep(USLEE_TIME);
    }

    device.defaultRegRegion().write(REG_ADDR_LOW, (chunck->begin.addr() & 0xFFFFFFFF));
    device.defaultRegRegion().write(REG_ADDR_HIGH, (chunck->begin.addr() >> 32));

    device.defaultRegRegion().write(REG_RES, ARM_DONE | chunck->byte_size);

    while (HOST_DONE != device.defaultRegRegion().read(REG_RES))
    {
        usleep(USLEE_TIME);
    }
    return;
}
#endif