#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <vector>
#include <fstream>
#include <random>
#include <bitset>

#ifdef __linux__
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/fb.h>
#endif

#if defined(x86_64) || defined(i386)
#include <immintrin.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include "modelzoo_utils.hpp"
#include "bit_masks.hpp"

#include "opencv2/opencv.hpp"
#include <spdlog/spdlog.h>

#include "icraft-xir/core/network.h"
#include "icraft-xir/core/data.h"
#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-xrt/dev/buyi_device.h>
#include <icraft-xrt/dev/zg330_device.h>
#include <icraft-backends/hostbackend/utils.h>
#include <icraft-backends/hostbackend/backend.h>
#include <icraft-backends/buyibackend/buyibackend.h>
#include <icraft-backends/zg330backend/zg330backend.h>

// 枚举类
// 表示摄像头输入的图像格式
enum camera_fmt
{
    RGB565,
    RGB,
    RGBA,
    YUV422,
};

// 枚举类
// 表示plresize模块的剪裁区域
enum crop_position
{
    top_left,
    top_right,
    bottom_left,
    bottom_right,
    center,
};

// 枚举类
// 表示resize模块的模式
enum resize_mode
{
    NO_RESIZE,
    DOWNSAMPLE
};

// template <typename predicate, typename Rep, typename Period>
// bool WaitUntil(predicate check, std::chrono::duration<Rep, Period> timeout)
// {
//     auto start = std::chrono::steady_clock::now();
//     while (!check())
//     {
//         usleep(50);
//         using namespace std::chrono_literals;
//         std::chrono::microseconds zeroms = 0ms;
//         if (timeout > zeroms && std::chrono::steady_clock::now() - start > timeout)
//         {
//             return false;
//         }
//     }
//     return true;
// }

template <typename predicate, typename Rep, typename Period>
bool WaitUntil(predicate check, std::chrono::duration<Rep, Period> timeout)
{
    auto start = std::chrono::steady_clock::now();
    int fast_poll_limit = 1000; // 前 1000 次快速轮询
    int poll_count = 0;
    while (!check())
    {
#ifndef _WIN32
        if (poll_count++ < fast_poll_limit)
        {
// 快速轮询：无延迟或极短延迟
#ifdef __aarch64__
            asm volatile("yield"); // ARM
#else
            _mm_pause(); // x86
#endif
        }
        else
        {
            // 慢速轮询：减少 CPU 占用
            usleep(50); // 实际 ~100us
        }
#else
        // Windows：快速阶段什么都不做；慢速阶段让出时间片，避免占满CPU
        if (poll_count++ >= fast_poll_limit)
        {
            Sleep(1);
        }
#endif
        using namespace std::chrono_literals;
        std::chrono::microseconds zeroms = 0ms;
        if (timeout > zeroms && std::chrono::steady_clock::now() - start > timeout)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Enables or disables the Video Timing Controller (VTC) for the camera.
 * @param device The icraft::xrt::Device object.
 * @param enable 0 to disable (default), 1 to enable.
 */
inline void enableCameraVTC(icraft::xrt::Device &device, int enable = 1)
{
    // by default, switch off VTC using real sdi camera input
    device.defaultRegRegion().write(0x40080000 + 0xB4, enable); // VTC_enable = 1; VTC_disable = 0
}

inline bool yuv2rgb(icraft::xrt::Device &dev, uint64_t wddr_base,
                    const int CAMERA_WIDTH, const int CAMERA_HEIGHT,
                    const int NET_WIDTH, const int NET_HEIGHT,
                    icraft::xrt::MemChunk &chunk,
                    uint32_t reg_base = 0x80080000,
                    uint32_t imk_reg_base = 0x80000400,
                    int wait_time_ms = 100)
{
    // 优先配置image_make数据pl放置位置
    // 对于多网络，该寄存器会被覆盖
    spdlog::debug("YUV2RGB Hard Resize setup: imk_reg_base={:#x}, wddr_base={:#x}, reg_base={:#x}", imk_reg_base, wddr_base, reg_base);
    dev.defaultRegRegion().write(imk_reg_base + 0x114, wddr_base);

    // soft reset
    dev.defaultRegRegion().write(reg_base + 0x24, 1);
    dev.defaultRegRegion().write(reg_base + 0x24, 1);
    dev.defaultRegRegion().write(reg_base + 0x24, 0);

    int ws = CAMERA_WIDTH / NET_WIDTH;
    int hs = CAMERA_HEIGHT / NET_HEIGHT;
    int IMG_W = ws * NET_WIDTH;
    int IMG_H = hs * NET_HEIGHT;
    int x0, y0, x1, y1;

    x0 = (CAMERA_WIDTH - IMG_W) / 2;
    y0 = (CAMERA_HEIGHT - IMG_H) / 2;
    x1 = CAMERA_WIDTH - x0 - 1;
    y1 = CAMERA_HEIGHT - y0 - 1;

    uint32_t y_arbase, y_last_arbase, uv_arbase, uv_last_arbase;

    y_arbase = chunk->begin.addr();
    y_last_arbase = y_arbase + CAMERA_WIDTH * CAMERA_HEIGHT - 8;
    uv_arbase = y_last_arbase + 8;
    uv_last_arbase = uv_arbase + (CAMERA_WIDTH * CAMERA_HEIGHT * 0.5) - 8;

    spdlog::debug("version reg base={:x}", dev.defaultRegRegion().read(reg_base + 0x1C));

    dev.defaultRegRegion().write(reg_base + 0x00, y_arbase);
    dev.defaultRegRegion().write(reg_base + 0x04, y_last_arbase);
    dev.defaultRegRegion().write(reg_base + 0x08, uv_arbase);
    dev.defaultRegRegion().write(reg_base + 0x0c, uv_last_arbase);

    spdlog::debug("y_arbase={}, y_last_arbase={}, uv_arbase={}, uv_last_arbase={}",
                  y_arbase, y_last_arbase, uv_arbase, uv_last_arbase);

    dev.defaultRegRegion().write(reg_base + 0x20, CAMERA_WIDTH);
    dev.defaultRegRegion().write(reg_base + 0x28, x0 << 16 | x1);
    dev.defaultRegRegion().write(reg_base + 0x2C, y0 << 16 | y1);
    dev.defaultRegRegion().write(reg_base + 0x30, CAMERA_WIDTH << 16 | CAMERA_HEIGHT);
    dev.defaultRegRegion().write(reg_base + 0x34, ws << 4 | hs);
    dev.defaultRegRegion().write(reg_base + 0x38, (x1 - x0 + 1) * (y1 - y0 + 1) / ws / hs);
    dev.defaultRegRegion().write(reg_base + 0x10, 1);

    bool yuv2rgb_done = false;
    WaitUntil(
        [&]()
        {
            auto yuv2rgb_done_regval = dev.defaultRegRegion().read(reg_base + 0x014);
            yuv2rgb_done = (yuv2rgb_done_regval == 1);
            return yuv2rgb_done;
        },
        std::chrono::duration<int, std::milli>(wait_time_ms)
        // 100ms
    );

    spdlog::debug("YUV2RGB Hard Resize, x0={}, y0={}, x1={}, y1={}, stride x={}, stride y={}, x_leng={}, y_leng={}",
                  x0, y0, x1, y1, ws, hs, CAMERA_WIDTH, CAMERA_HEIGHT);

    spdlog::debug("[yuv2rgb] yuv2rgb_done={}", yuv2rgb_done);
    spdlog::debug("[yuv2rgb] done reg={}", dev.defaultRegRegion().read(reg_base + 0x014));
    spdlog::debug("[yuv2rgb] done cnt reg={}", dev.defaultRegRegion().read(reg_base + 0x018));

    return yuv2rgb_done;
}

// nms_pre_data 一维数组包含多个框的位置信息和类别信息，按照框的置信度大小从高到低排序的,一个框的信息表示为{x1,y1,x2,y2,class}。
// nms_pre_idx 所有的框按照置信度从高到低排列后,nms_pre_idx 记录了数组中排序后框在原未排序数组中的idx
// bbox_num 为框的个数
// iou阈值
// 该模块限制最多输入框个数为5000个
inline std::vector<int> fpgaNms(icraft::xrt::Device &device, const std::vector<int16_t> &nms_pre_data, std::vector<int> nms_pre_idx,
                                int bbox_num, const float &iou, uint64_t base_addr = 0x100001C00)
{
    if (nms_pre_data.size() != bbox_num * 5 || nms_pre_idx.size() != bbox_num)
    {
        std::cout << "ERROR in FpgaNms :: The data for FpgaNms is error, Please check it!" << std::endl;
        throw std::runtime_error("ERROR in FpgaNms :: The data for FpgaNms is error, Please check it!");
    }
    std::vector<int> nms_indices;

    auto nms_data_cptr = nms_pre_data.data();
    auto uregion_ = device.getMemRegion("udma");
    auto udma_chunk_ = uregion_.malloc(10e6);
    auto mapped_base = udma_chunk_->begin.addr();
    udma_chunk_.write(0, (char *)nms_data_cptr, bbox_num * 10);
    // hard nms config
    float threshold_f = iou;
    uint64_t arbase = mapped_base;
    uint64_t awbase = mapped_base;
    // 检查硬件的版本信息是否正确，不正确会抛出错误
    if (device.defaultRegRegion().read(base_addr + 0x008, true) != 0x23110200)
    {
        std::cout << "ERROR in FpgaNms :: No NMS HardWare" << std::endl;
        throw std::runtime_error("ERROR in FpgaNms :: No NMS HardWare");
    }
    auto group_num = (uint64_t)ceilf((float)bbox_num / 16.f);
    if (group_num == 0)
    {
        std::cout << "ERROR in FpgaNms :: group_num == 0" << std::endl;
        throw std::runtime_error("ERROR in FpgaNms :: group_num == 0");
    }
    auto last_araddr = arbase + group_num * 160 - 8;
    if (last_araddr < arbase)
    {
        std::cout << "ERROR in FpgaNms :: last_araddr < arbase" << std::endl;
        throw std::runtime_error("ERROR in FpgaNms :: last_araddr < arbase");
    }
    auto anchor_hpsize = (uint64_t)ceilf((float)bbox_num / 64.f);
    if (anchor_hpsize == 0)
    {
        std::cout << "ERROR in FpgaNms :: anchor_hpsize == 0" << std::endl;
        throw std::runtime_error("ERROR in FpgaNms :: anchor_hpsize == 0");
    }
    auto last_awaddr = awbase + anchor_hpsize * 8 - 8;
    if (last_awaddr < awbase)
    {
        std::cout << "ERROR in FpgaNms :: last_awaddr < awbase" << std::endl;
        throw std::runtime_error("ERROR in FpgaNms :: last_awaddr < awbase");
    }

    auto threshold = (uint16_t)(threshold_f * pow(2, 15));
    // config reg
    device.defaultRegRegion().write(base_addr + 0x014, 1, true);
    device.defaultRegRegion().write(base_addr + 0x014, 0, true);
    device.defaultRegRegion().write(base_addr + 0x01C, arbase, true);
    device.defaultRegRegion().write(base_addr + 0x020, awbase, true);
    device.defaultRegRegion().write(base_addr + 0x024, last_araddr, true);
    device.defaultRegRegion().write(base_addr + 0x028, last_awaddr, true);
    device.defaultRegRegion().write(base_addr + 0x02C, group_num, true);
    device.defaultRegRegion().write(base_addr + 0x030, 0, true); // mode: 0同类之间筛选、1所有类之间筛选
    device.defaultRegRegion().write(base_addr + 0x034, threshold, true);
    device.defaultRegRegion().write(base_addr + 0x038, anchor_hpsize, true);

    device.defaultRegRegion().write(base_addr + 0x0, 1, true); // start
    uint64_t reg_done;
    auto start = std::chrono::steady_clock::now();
    do
    {
        reg_done = device.defaultRegRegion().read(base_addr + 0x004, true);
        std::chrono::duration<double, std::milli> duration = std::chrono::steady_clock::now() - start;
        if (duration.count() > 1000)
        {
            std::cout << "ERROR in FpgaNms :: NMS Timeout!!!" << std::endl;
            throw std::runtime_error("ERROR in FpgaNms :: NMS Timeout!!!");
        }
    } while (reg_done == 0);
    uint64_t mask_size = (uint64_t)(ceilf((float)bbox_num / 8.f));
    char *mask = new char[64000];
    udma_chunk_.read(mask, 0, mask_size);

    for (int i = 0; i < bbox_num; ++i)
    {
        const int idx = nms_pre_idx[i];
        int mask_index = i / 8;
        if (i % 8 == 0 && ((mask[mask_index] & (uint8_t)1) != 0))
            nms_indices.emplace_back(idx);
        else if (i % 8 == 1 && ((mask[mask_index] & (uint8_t)2) != 0))
            nms_indices.emplace_back(idx);
        else if (i % 8 == 2 && ((mask[mask_index] & (uint8_t)4) != 0))
            nms_indices.emplace_back(idx);
        else if (i % 8 == 3 && ((mask[mask_index] & (uint8_t)8) != 0))
            nms_indices.emplace_back(idx);
        else if (i % 8 == 4 && ((mask[mask_index] & (uint8_t)16) != 0))
            nms_indices.emplace_back(idx);
        else if (i % 8 == 5 && ((mask[mask_index] & (uint8_t)32) != 0))
            nms_indices.emplace_back(idx);
        else if (i % 8 == 6 && ((mask[mask_index] & (uint8_t)64) != 0))
            nms_indices.emplace_back(idx);
        else if (i % 8 == 7 && ((mask[mask_index] & (uint8_t)128) != 0))
            nms_indices.emplace_back(idx);
    }
    delete[] mask;
    return nms_indices;
}

// Dma&ImageMake硬件模块配置
// 参数说明:
// img_tensor – imagemake的输入tensor
// device – 输入icraft::xrt::Device，对预设的寄存器进行读写需要device
// imk_write_addr  –  ImageMake写入PLDDR的基地址，默认如果不传入该参数，将在ImageMake forward时配置该地址
// imk_base_addr  – ImageMake的寄存器基地址，默认为0x100000400，即input_port = 0对应的寄存器基地址
// dma_base_addr – fpgaDma的寄存器基地址，默认配置为当前版本下input_port = 0对应的基地址。
inline void fpgaDma(icraft::xrt::Tensor &img_tensor, icraft::xrt::Device &device,
                    uint64_t imk_write_addr = std::numeric_limits<uint64_t>::max(),
                    uint64_t imk_base_addr = 0x100000400, uint64_t dma_base_addr = 0x1000C0000)
{
    auto ImageMakeChannel = img_tensor.dtype()->shape[-1];
    auto ImageMakeWidth = img_tensor.dtype()->shape[-2];
    auto ImageMakeHeight = img_tensor.dtype()->shape[-3];
    // 获取umda的memRegion
    auto uregion_ = device.getMemRegion("udma");
    // 将host上的输出复制到udma上，并返回对应的tensor 包含了内存管理机制
    auto utensor = img_tensor.to(uregion_);
    // 获取在udma上对应的物理指针
    auto ImageMakeRddrBase = utensor.data().addr();

    // 获取input_dtype
    bool flag_8 = img_tensor.dtype()->element_dtype.getStorageType().isUInt8();
    bool flag_16 = img_tensor.dtype()->element_dtype.getStorageType().isSInt16();

    uint32_t ImageMakeRlen;
    uint32_t ImageMakeLastSft;

    // dma_imk_Init
    if (!(flag_8 || flag_16))
    {
        throw std::runtime_error("DMA only supports uint8 and int16");
    }
    if (flag_16)
    {
        auto totalBytes = ImageMakeChannel * ImageMakeWidth * ImageMakeHeight * 2;
        auto minBlocks = (totalBytes + 7) / 8;
        ImageMakeRlen = ((minBlocks + 2) / 3) * 3;
        auto pixelsPer24Bytes = 24 / (2 * ImageMakeChannel);
        auto totalPixels = ImageMakeWidth * ImageMakeHeight;
        ImageMakeLastSft = totalPixels - (ImageMakeRlen / 3 - 1) * pixelsPer24Bytes;
        device.defaultRegRegion().write(dma_base_addr + 0x20, 0x2, true);
    }
    else
    {
        ImageMakeRlen = ((ImageMakeWidth * ImageMakeHeight - 1) / (24 / ImageMakeChannel) + 1) * 3;
        ImageMakeLastSft = ImageMakeWidth * ImageMakeHeight - (ImageMakeRlen - 3) / 3 * (24 / ImageMakeChannel);
        device.defaultRegRegion().write(dma_base_addr + 0x20, 0, true);
    }

    if (imk_write_addr != std::numeric_limits<uint64_t>::max())
    {
        // 多线程psin时，需要提前配置ImageMake写入PLDDR的基地址，避免结果错位
        device.defaultRegRegion().write(imk_base_addr + 0x114, imk_write_addr, true);
    }
    device.defaultRegRegion().write(dma_base_addr + 0x4, ImageMakeRddrBase, true);
    device.defaultRegRegion().write(dma_base_addr + 0x8, ImageMakeRlen, true);
    device.defaultRegRegion().write(dma_base_addr + 0xC, ImageMakeLastSft, true);
    device.defaultRegRegion().write(dma_base_addr + 0x10, ImageMakeChannel, true);
    device.defaultRegRegion().write(dma_base_addr + 0x1C, 1, true);
    device.defaultRegRegion().write(dma_base_addr, 1, true);
}

// warpaffine寄存器配置，M_inversed: 2x3变换矩阵的逆矩阵
inline void fpgaWarpaffine(std::vector<std::vector<float>> &M_inversed, icraft::xrt::Device &device, uint64_t base_addr = 0x100002800)
{
    // 配置warpaffine寄存器
    auto coef_a = int64_t(M_inversed[0][0] * pow(2, 15));
    auto coef_b = int64_t(M_inversed[0][1] * pow(2, 15));
    auto coef_c = int64_t(M_inversed[0][2] * 2);
    auto coef_d = int64_t(M_inversed[1][0] * pow(2, 15));
    auto coef_e = int64_t(M_inversed[1][1] * pow(2, 15));
    auto coef_f = int64_t(M_inversed[1][2] * 2);

    device.defaultRegRegion().write(base_addr + 0x030, coef_a, true);
    device.defaultRegRegion().write(base_addr + 0x034, coef_c, true);
    device.defaultRegRegion().write(base_addr + 0x038, coef_e, true);
    device.defaultRegRegion().write(base_addr + 0x03C, coef_f, true);
    device.defaultRegRegion().write(base_addr + 0x044, coef_b, true);
    device.defaultRegRegion().write(base_addr + 0x048, coef_d, true);
}

inline icraft::xrt::Tensor fpgaArgmax2d(icraft::xrt::Device &dev, int wsize, int hsize, int valid_csize, int csize,
                                        uint64_t arbase, uint64_t last_araddr, uint64_t base_addr = 0x100003000)
{
    // 参数说明
    // arbase -  - 初始地址
    // last_araddr - 最后一层 ftmp 在plddr的地址

    int w = wsize;
    int h = hsize;
    int c = valid_csize;

    int csize_cal = (c > 32) ? ((c / 32 + 1) * 32) : static_cast<int>(std::pow(2, static_cast<uint32_t>(std::ceil(std::log2(c)))));
    int cu = (csize > 32) ? 32 : csize;
    int ct = csize / cu;

    int cu_araddr_num = ((w * h * cu) % 64 == 0) ? ((w * h * cu) / 64 - 1) : (w * h * cu) / 64;
    int cu_flag = std::log2(cu);
    int last_vld_cu = (c % cu == 0) ? (cu - 1) : (c % cu - 1);
    int cu_size = w * h * cu;

    const uint64_t ARGMAX2D_START = base_addr + 0x000;
    const uint64_t ARGMAX2D_DONE = base_addr + 0x004;
    const uint64_t ARGMAX2D_VER = base_addr + 0x008;
    const uint64_t ARGMAX2D_TEST = base_addr + 0x00c;
    const uint64_t ARGMAX2D_TIME_CNT = base_addr + 0x010;
    const uint64_t ARGMAX2D_SOFT_RST = base_addr + 0x014;
    const uint64_t ARGMAX2D_STATUS = base_addr + 0x018;
    const uint64_t ARGMAX2D_ARBASE = base_addr + 0x01c;
    const uint64_t ARGMAX2D_AWBASE = base_addr + 0x020;
    const uint64_t ARGMAX2D_LAST_ARADDR = base_addr + 0x024;
    const uint64_t ARGMAX2D_LAST_AWADDR = base_addr + 0x028;
    const uint64_t ARGMAX2D_CU_ARADDR_NUM = base_addr + 0x02c;
    const uint64_t ARGMAX2D_CU_FLAG = base_addr + 0x030;
    const uint64_t ARGMAX2D_LAST_VLD_CU = base_addr + 0x034;
    const uint64_t ARGMAX2D_CU_SIZE = base_addr + 0x038;
    const uint64_t ARGMAX2D_SLEEPTIME = 50;
    // 在udmabuf上申请argmax2d的缓存区,获取缓存的首尾物理地址
    const uint64_t argmax2d_psbuf_size = valid_csize * 8;
    auto argmax2d_pschunck = dev.getMemRegion("udma").malloc(argmax2d_psbuf_size, true); // auto free chunk
    auto awbase = argmax2d_pschunck->begin.addr();
    auto last_awaddr = awbase + argmax2d_psbuf_size;
    // 参数合法性检查
    uint32_t argmax_ver_rd = dev.defaultRegRegion().read(ARGMAX2D_VER, true);
    // uint32_t argmax_ver_rt = 0x24051200;
    // uint32_t argmax_ver_rt = 0x24071800;
    uint32_t argmax_ver_rt = 0x24073000;
    if (argmax_ver_rd != argmax_ver_rt)
    {
        std::cout << "Error in FpgaArgma2d: Argmax2d HardWare Version Mismatch! Read Version is " << argmax_ver_rd << ", Right version is" << argmax_ver_rt << std::endl;
        ICRAFT_LOG(EXCEPT) << "Error in FpgaArgma2d :: No Argmax2d HardWare Or Version mismatch";
    }
    if (csize_cal != csize)
    {
        std::cout << "Error in FpgaArgma2d: csize input is" << csize << "calculated csize is " << csize_cal << std::endl;
        ICRAFT_LOG(EXCEPT) << "Error in FpgaArgma2d: csize input err";
    }
    if ((w * h * cu) % 64 != 0)
    {
        std::cout << "Error in FpgaArgma2d: (w * h * cu) % 64 != 0, argmax2d hardop not support!" << std::endl;
        ICRAFT_LOG(EXCEPT) << "Error in FpgaArgma2d: (w * h * cu) % 64 != 0, argmax2d hardop not support";
    }
    // 调试用
    // std::cout<<"csize_cal ="<<csize_cal<<" cu ="<<cu<<" cu_araddr_num ="<<cu_araddr_num<<" last_vld_cu ="<<last_vld_cu<<" cu_size ="<<cu_size<<" cu_flag ="<<cu_flag<<std::endl;
    // 配置寄存器
    dev.defaultRegRegion().write(ARGMAX2D_ARBASE, arbase, true);
    dev.defaultRegRegion().write(ARGMAX2D_LAST_ARADDR, last_araddr, true);
    dev.defaultRegRegion().write(ARGMAX2D_AWBASE, awbase, true);
    dev.defaultRegRegion().write(ARGMAX2D_LAST_AWADDR, last_awaddr, true);
    dev.defaultRegRegion().write(ARGMAX2D_CU_ARADDR_NUM, cu_araddr_num, true);
    dev.defaultRegRegion().write(ARGMAX2D_CU_FLAG, cu_flag, true);
    dev.defaultRegRegion().write(ARGMAX2D_LAST_VLD_CU, last_vld_cu, true);
    dev.defaultRegRegion().write(ARGMAX2D_CU_SIZE, cu_size, true);
    dev.defaultRegRegion().write(ARGMAX2D_START, 1, true);

    // 轮询done信号
    unsigned int argmax2d_done = 0;
    auto start = std::chrono::steady_clock::now();
    do
    {
#ifndef _WIN32
        if (ARGMAX2D_SLEEPTIME > 0)
            usleep(ARGMAX2D_SLEEPTIME);
#endif
        argmax2d_done = dev.defaultRegRegion().read(ARGMAX2D_DONE, true);
        std::chrono::duration<double, std::milli> duration = std::chrono::steady_clock::now() - start;
        if (duration.count() > 1000)
        {
            std::cout << "Error in FpgaArgma2d :: Argmax2d Timeout" << std::endl;
            ICRAFT_LOG(EXCEPT) << "Error in FpgaArgma2d :: Argmax2d Timeout";
        }
    } while (argmax2d_done == 0);
    // 获取FPGA计时
    unsigned int argmax2d_time_cnt = dev.defaultRegRegion().read(ARGMAX2D_TIME_CNT, true);
    double argmax2d_hard_time = (argmax2d_time_cnt * 5) / 1000000.0; // 单位ms
    // std::cout << "argmax2d_hard_time = " << argmax2d_hard_time << std::endl;

    // 获取各通道最大值的坐标,构造输出的tensor
    auto ofm_layout = icraft::xir::Layout::NHWC();
    icraft::xir::TensorType output_type;
    // icraft::xir::Array<int64_t> output_dim = { 1,1,c,8 };
    icraft::xir::Array<IntImm> output_dim = {1, 1, c, 8};
    // auto data = std::shared_ptr<uint8_t[]>(new uint8_t[c*8]);
    // argmax2d_pschunck.read((char*)data.get(), 0, c * 8);
    output_type = icraft::xir::TensorType(icraft::xir::IntegerType::UInt8(), output_dim, ofm_layout);
    auto output_tensor = icraft::xrt::Tensor(output_type, argmax2d_pschunck, 0); // udma buffer 获取结果
    return output_tensor;
}

/**
 *   nms_hard,使用说明
 *   若最终输出检测数量为500个，nms_hard耗时约0.638ms
 *   若最终输出检测数量为100个，nms_hard耗时约0.297ms
 *   当最终检测数量小于30个的情况下，采用nms_soft会比nms_hard速度快。
 *   确保送入该函数的框的置信度以及在外部进行了阈值筛选
 *   注：该函数适配大部分yolo系列模型后处理的hard nms函数，其调用了setFpgaNms模块
 */
inline std::vector<std::tuple<int, float, cv::Rect2f>> nms_hard(std::vector<cv::Rect2f> &box_list, std::vector<float> &score_list, std::vector<int> &id_list,
                                                                const float &iou, icraft::xrt::Device &device, int max_nms = 3000)
{
    const int HARD_NMS_BOX_LIMIT = 8192;
    std::vector<std::pair<float, int>> score_index_vec;
    std::vector<std::tuple<int, float, cv::Rect2f>> num_res;
    std::vector<int> after_id_list;
    if (box_list.size() == 0)
        return num_res;
    for (size_t i = 0; i < score_list.size(); ++i)
    {
        score_index_vec.emplace_back(std::make_pair(score_list[i], i));
        after_id_list.push_back(id_list[i]);
    }
    std::stable_sort(score_index_vec.begin(), score_index_vec.end(),
                     [](const std::pair<float, int> &pair1, const std::pair<float, int> &pair2)
                     { return pair1.first > pair2.first; });
    // 重新排列 after_id_list
    std::vector<int> resort_idx;
    std::vector<int> nms_pre_idx;
    std::vector<int> sorted_after_id_list(after_id_list.size());
    for (size_t i = 0; i < score_index_vec.size(); ++i)
    {
        sorted_after_id_list[i] = after_id_list[score_index_vec[i].second];
        resort_idx.push_back(score_index_vec[i].second);
    }

    // 更新 after_id_list
    after_id_list = sorted_after_id_list;

    std::vector<int16_t> nms_pre_data;

    int box_num = score_index_vec.size();
    if (box_num > max_nms)
    {
        box_num = max_nms;
    }
    if (HARD_NMS_BOX_LIMIT < box_num)
    {
        throw std::runtime_error("Error in nms_hard: box_num > 8192, please limit bounding boxes <=8192");
    }

    for (int i = 0; i < box_num; ++i)
    {
        const int idx = score_index_vec[i].second;
        auto x1 = box_list[idx].tl().x;
        if (x1 < 0)
            x1 = 0;
        auto y1 = box_list[idx].tl().y;
        if (y1 < 0)
            y1 = 0;
        auto x2 = box_list[idx].br().x;
        auto y2 = box_list[idx].br().y;
        nms_pre_data.push_back((int16_t)x1);
        nms_pre_data.push_back((int16_t)y1);
        nms_pre_data.push_back((int16_t)x2);
        nms_pre_data.push_back((int16_t)y2);
        nms_pre_data.push_back((int16_t)after_id_list[i]);
        nms_pre_idx.push_back(resort_idx[i]);
    }

    std::vector<int> nms_indices = fpgaNms(device, nms_pre_data, nms_pre_idx, box_num, iou);
    for (auto idx : nms_indices)
    {
        num_res.push_back({id_list[idx], score_list[idx], box_list[idx]});
    }
    return num_res;
}

inline void dmaInit(const std::string &runBackend, const bool &has_ImageMake, icraft::xrt::Tensor &img_tensor, icraft::xrt::Device &device)
{
    // #ifdef _WIN32
    // if (runBackend.compare("buyi") != 0 || !has_ImageMake) {
    //     return;
    // }
    // #endif
    // if (has_ImageMake) {
    //     fpgaDma(img_tensor, device);

    //}
    if (has_ImageMake && runBackend.compare("buyi") == 0)
    {
        fpgaDma(img_tensor, device);
    }
}

inline void dma_imk_Init(const std::string &run_backend, const bool &has_ImageMake,
                         icraft::xir::Operation &ImageMake_,
                         icraft::xrt::Tensor &img_tensor,
                         icraft::xrt::Device &device,
                         icraft::xrt::Session &session)
{
#ifdef _WIN32
    if (run_backend.compare("host") == 0 || !has_ImageMake)
    {
        return;
    }
#endif
    if (has_ImageMake)
    {
        // session->backends[0].cast<icraft::xrt::BuyiBackend>().initOp(ImageMake_);
        session->backends[0].cast<icraft::xrt::BuyiBackend>().initOp(ImageMake_);
        fpgaDma(img_tensor, device);
    }
}

inline float calculate_scale(double thr_f1, double thr_f2)
{
    // 检查thr_f1和thr_f2是否在[0, 1)范围内
    if (thr_f1 <= 0 || thr_f1 >= 1 || thr_f2 <= 0 || thr_f2 >= 1)
    {
        throw std::invalid_argument("Both thr_f1 and thr_f2 must be in the range (0, 1) to avoid division by zero.");
    }
    // 计算scale的值
    float scale = log(1 / thr_f1 - 1) / log(1 / thr_f2 - 1);
    // return static_cast<int64_t>(scale);
    return scale;
}

inline void updateDetpost(NetInfo &netinfo, float conf)
{
    // 获取detpost op
    icraft::xir::Operation det = netinfo.DetPost_;
    // ZG DetPost's hardware integer comparator does not order floating words
    // numerically (negative BF16 logits were dropped on real AXI). Retain
    // cmp_en and its packed grid layout, using the lowest signed16 word as
    // a conservative prefilter. CPU post_process applies the exact configured
    // sigmoid confidence. Disabling cmp_en changes the output layout.
    if (det->inputs[0]->dtype.getStorageType().is<icraft::xir::FloatType>())
    {
        if (!(conf > 0.f && conf < 1.f))
            throw std::invalid_argument("Confidence must be in (0,1)");
        auto thresholds = det->getAttr("data_thr").cast<icraft::xir::Array<int64_t>>();
        for (int i = 0; i < thresholds.size(); ++i) thresholds.set(i, -32768);
        det.setAttr("data_thr", thresholds);
        netinfo.thr_f = conf;
        return;
    }
    // 如果yaml.conf与detpost的conf不一致，则更新detpost的data_thr
    // std::cout << "Original conf: " << netinfo.thr_f << ", New conf: " << conf << std::endl;
    if (netinfo.thr_f != conf)
    {
        // 获取detpost原始的data_thr
        icraft::xir::Array<int64_t> data_thr = det->getAttr("data_thr").cast<icraft::xir::Array<int64_t>>();
        // 计算缩放scale
        float thr_f1 = conf;          // new conf
        float thr_f2 = netinfo.thr_f; // original conf
        float scale = calculate_scale(thr_f1, thr_f2);
        netinfo.thr_f = conf; // 更新netinfo.thr_f
        // 计算new thr_q(data_thr)
        try
        {
            for (int i = 0; i < data_thr.size(); i++)
            {
                netinfo.data_thr[i] *= scale;
                data_thr.set(i, static_cast<int64_t>(netinfo.data_thr[i])); // calculate new data_thr
                // std::cout << netinfo.data_thr[i] << std::endl;
            }
            det.setAttr("data_thr", data_thr); // set Attr
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }
}

//-------------------------------------//
//       PLin
//-------------------------------------//

inline void hardResizePS(icraft::xrt::Device &dev, const int CAMERA_WIDTH, const int CAMERA_HEIGHT,
                         const int FRAME_WIDTH, const int FRAME_HEIGHT,
                         camera_fmt fmt, crop_position crop, uint64_t base_addr = 0x40080000)
{

    int ws = CAMERA_WIDTH / FRAME_WIDTH;
    int hs = CAMERA_HEIGHT / FRAME_HEIGHT;
    int IMG_W = ws * FRAME_WIDTH;
    int IMG_H = hs * FRAME_HEIGHT;
    int x0, y0, x1, y1;

    switch (crop)
    {
    case crop_position::center:
        x0 = (CAMERA_WIDTH - IMG_W) / 2;
        y0 = (CAMERA_HEIGHT - IMG_H) / 2;
        x1 = CAMERA_WIDTH - x0 - 1;
        y1 = CAMERA_HEIGHT - y0 - 1;
        break;

    case crop_position::top_left:
        x0 = 0;
        y0 = 0;
        x1 = IMG_W - 1;
        y1 = IMG_H - 1;
        break;

    case crop_position::top_right:
        x0 = CAMERA_WIDTH - IMG_W;
        y0 = 0;
        x1 = CAMERA_WIDTH - 1;
        y1 = IMG_H - 1;
        break;

    case crop_position::bottom_left:
        x0 = 0;
        y0 = CAMERA_HEIGHT - IMG_H;
        x1 = IMG_W - 1;
        y1 = CAMERA_HEIGHT - 1;
        break;

    case crop_position::bottom_right:
        x0 = CAMERA_WIDTH - IMG_W;
        y0 = CAMERA_HEIGHT - IMG_H;
        x1 = CAMERA_WIDTH - 1;
        y1 = CAMERA_HEIGHT - 1;
        break;
    }

    dev.defaultRegRegion().write(base_addr + 0x18, 1);
    dev.defaultRegRegion().write(base_addr + 0x5c, x0 << 16 | x1);
    dev.defaultRegRegion().write(base_addr + 0x60, y0 << 16 | y1);
    dev.defaultRegRegion().write(base_addr + 0x64, CAMERA_WIDTH << 16 | CAMERA_HEIGHT);
    dev.defaultRegRegion().write(base_addr + 0x68, ws << 4 | hs);

    int image_fmt_channel = 4;
    switch (fmt)
    {
    case camera_fmt::RGB565:
        dev.defaultRegRegion().write(base_addr + 0x78, 0);
        image_fmt_channel = 2;
        break;

    case camera_fmt::RGB:
        image_fmt_channel = 3;
        break;

    case camera_fmt::RGBA:
        dev.defaultRegRegion().write(base_addr + 0x78, 0);
        image_fmt_channel = 4;
        break;

    case camera_fmt::YUV422:
        image_fmt_channel = 2;
        dev.defaultRegRegion().write(base_addr + 0x7c, FRAME_WIDTH);
        dev.defaultRegRegion().write(base_addr + 0x78, 1);
        break;

    default:
        break;
    }
    // spdlog::info("Hard Resize PS, x0={}, y0={}, x1={}, y1={}, stride x={}, stride y={}, resize channel={}",
    //     x0, y0, x1, y1, ws, hs, image_fmt_channel);
    std::cout << "Hard Resize PS, x0={" << x0 << "}, y0={" << y0 << "}, x1={" << x1 << "}, y1={" << y1 << "}, stride x={" << ws << "}, stride y={" << hs << "}, resize channel={" << image_fmt_channel << "}" << std::endl;
    dev.defaultRegRegion().write(base_addr + 0x6c, FRAME_WIDTH * FRAME_HEIGHT * image_fmt_channel / 8);
}

template <typename DeviceType>
inline void hardResizePL(DeviceType &device, int x0, int y0, int x1, int y1,
                         int RATIO_W, int RATIO_H, int CAMERA_WIDTH, int CAMERA_HEIGHT,
                         uint64_t base_addr = 0x40080000)
{
    device.defaultRegRegion().write(base_addr + 0x18, 1);
    device.defaultRegRegion().write(base_addr + 0x20, RATIO_W);       // x方向行步长
    device.defaultRegRegion().write(base_addr + 0x24, RATIO_H);       // y方向列步长
    device.defaultRegRegion().write(base_addr + 0x28, x0);            // 起始x0 坐标位置 （0~FRAME_W）
    device.defaultRegRegion().write(base_addr + 0x2C, y0);            // 起始y0 坐标位置 （0~FRAME_H）
    device.defaultRegRegion().write(base_addr + 0x30, x1);            // 终止x1 坐标位置 （0~FRAME_W）
    device.defaultRegRegion().write(base_addr + 0x34, y1);            // 终止y1 坐标位置 （0~FRAME_H）
    device.defaultRegRegion().write(base_addr + 0x38, CAMERA_WIDTH);  // 图像X方向总长度 （FRAME_W）
    device.defaultRegRegion().write(base_addr + 0x3C, CAMERA_HEIGHT); // 图像y方向总长度 （FRAME_H）
    spdlog::info("Hard Resize PL, x0={}, y0={}, x1={}, y1={}, stride x={}, stride y={}",
                 x0, y0, x1, y1, RATIO_W, RATIO_H);
    // std::cout << "Hard Resize PL, x0={" << x0 << "}, y0={" << y0 << "}, x1={" << x1 << "}, y1={" << y1 << "},stride x={" << RATIO_W << "}, stride y={" << RATIO_H << "}" << std::endl;
}

template <typename DeviceType>
inline std::tuple<int, int, int, int> preprocess_plin(DeviceType &device, const int CAMERA_WIDTH, const int CAMERA_HEIGHT,
                                                      const int NET_W, const int NET_H, crop_position crop, uint64_t base_addr = 0x40080000)
{
    int RATIO_W = CAMERA_WIDTH / NET_W;
    int RATIO_H = CAMERA_HEIGHT / NET_H;
    int IMG_W = RATIO_W * NET_W;
    int IMG_H = RATIO_H * NET_H;
    int BIAS_W = (CAMERA_WIDTH - IMG_W) / 2;
    int BIAS_H = (CAMERA_HEIGHT - IMG_H) / 2;
    int x0, y0, x1, y1;

    switch (crop)
    {
    case crop_position::center:
        x0 = (CAMERA_WIDTH - IMG_W) / 2;
        y0 = (CAMERA_HEIGHT - IMG_H) / 2;
        x1 = CAMERA_WIDTH - x0 - 1;
        y1 = CAMERA_HEIGHT - y0 - 1;
        break;

    case crop_position::top_left:
        x0 = 0;
        y0 = 0;
        x1 = IMG_W - 1;
        y1 = IMG_H - 1;
        break;

    case crop_position::top_right:
        x0 = CAMERA_WIDTH - IMG_W;
        y0 = 0;
        x1 = CAMERA_WIDTH - 1;
        y1 = IMG_H - 1;
        break;

    case crop_position::bottom_left:
        x0 = 0;
        y0 = CAMERA_HEIGHT - IMG_H;
        x1 = IMG_W - 1;
        y1 = CAMERA_HEIGHT - 1;
        break;

    case crop_position::bottom_right:
        x0 = CAMERA_WIDTH - IMG_W;
        y0 = CAMERA_HEIGHT - IMG_H;
        x1 = CAMERA_WIDTH - 1;
        y1 = CAMERA_HEIGHT - 1;
        break;
    }
    hardResizePL(device, x0, y0, x1, y1, RATIO_W, RATIO_H, CAMERA_WIDTH, CAMERA_HEIGHT, base_addr);
    return {RATIO_W, RATIO_H, BIAS_W, BIAS_H};
}

template <typename DeviceType>
inline void initImageMake(DeviceType &device, int imk_port,
                          int64_t ImageMakeWidth, int64_t ImageMakeHeight, int64_t ImageMakeChannel,
                          uint64_t ImageMakeWddrBase_a, uint64_t bits,
                          const std::vector<float> &premean_, const std::vector<float> &prescale_data)
{
    uint64_t reg_base;

    // 根据配置的reg_base及input参数得到image_make的寄存器基地址
    // 当配置reg_base_不为空时，会优先将配置的基地址作为寄存器基地址
    // 当reg_base_为空时，会根据配置的input0~3确定各自的寄存器基地址，默认input0地址
    if (imk_port == 0)
        reg_base = 0x80000400;
    else if (imk_port == 1)
        reg_base = 0x80040000;
    else if (imk_port == 2)
        reg_base = 0x80040400;
    else if (imk_port == 3)
        reg_base = 0x80040800;
    else
    {
        ICRAFT_LOG(ERROR) << "imk_port should between 0~3, but is " << imk_port;
        ICRAFT_LOG(EXCEPT).append("imk_port should between 0~3, but is {}", imk_port);
    }
    // 检查硬件的版本信息是否正确，不正确会抛出错误
    uint64_t imk_version = device.defaultRegRegion().read(reg_base + 0x234);
    if (imk_version != 0x20220623)
        ICRAFT_LOG(ERROR) << "No Image_Make HardWare, imk_version is " << imk_version;
    ICRAFT_CHECK(imk_version == 0x20220623)
        .append("No Image_Make HardWare, imk_version is {}", imk_version);

    // 根据pad参数，得到上下左右pad的size
    auto pad_r = 0;
    auto pad_l = 0;
    auto pad_b = 0;
    auto pad_t = 0;

    // 根据输入ftmp的通道数，确定每次处理图片的通道数
    // 目前只支持1~4通道输入ftmp数据
    auto ImageMakeChannel_each = 0;
    if (ImageMakeChannel % 4 == 0)
    {
        ImageMakeChannel_each = 4;
    }
    else if (ImageMakeChannel % 3 == 0)
    {
        ImageMakeChannel_each = 3;
    }
    else if (ImageMakeChannel % 2 == 0)
    {
        ImageMakeChannel_each = 2;
    }
    else
    {
        ImageMakeChannel_each = 1;
    }

    // use input image
    // 计算批次，目前只支持计算批次为1
    auto ImageMakeChannelTimes = ImageMakeChannel / ImageMakeChannel_each;

    std::vector<int> premean(ImageMakeChannel);
    std::vector<int> prescale(ImageMakeChannel);

    // 硬件中支持乘以prescale的倒数，并且会固定截位10位
    for (int i = 0; i < ImageMakeChannel; i++)
    { // image_channel need software provide
        premean[i] = (int)(-premean_[i]);
        prescale[i] = (int)((1 / prescale_data[i]) * pow(2, 10));
    }

    // 将mean和scale的值拼接起来，其中precale为低16bit，prescale为高16bit
    std::vector<uint32_t> ImageMakeMSC(ImageMakeChannel);
    for (int i = 0; i < ImageMakeChannel; i++)
    {
        ImageMakeMSC[i] = (int16_t)prescale[i] | (int16_t)premean[i] << 16;
    }

    // 将上下左右的pad size拼接成一个32bit的数，该数用于写pad控制相关寄存器
    auto ImageMakePadReg = pad_t + (pad_b << 8) + (pad_l << 16) + (pad_r << 24);

    // 得到数据类型寄存器参数，当为8bit模式时为0，当为16bit模式时为1
    int ImageMakeDataArrange = 0;
    if (bits == 8)
    {
        ImageMakeDataArrange = 0;
    }
    else
    {
        ImageMakeDataArrange = 1;
    }

    // 调用device的写寄存器的接口，配置硬件需要的寄存器，其中writeReg和readReg中最后一个参数为true，用于整体寄存器空间偏移配置
    // 各寄存器表示含义介绍，参照ImakeMake硬算子用户手册
    // 0x014~0x110
    auto one_fast = 0;
    device.defaultRegRegion().write(reg_base + 0x004, one_fast); // one fast
    for (int i = 0; i < 4; i++)
    {
        if (i < ImageMakeChannel)
        {
            device.defaultRegRegion().write(reg_base + (0x14 + i * 4), ImageMakeMSC[i]);
        }
        else
        {
            device.defaultRegRegion().write(reg_base + (0x14 + i * 4), 0);
        }
    }

    device.defaultRegRegion().write(reg_base + 0x114, ImageMakeWddrBase_a);
    device.defaultRegRegion().write(reg_base + 0x118, ImageMakeWidth);
    device.defaultRegRegion().write(reg_base + 0x11C, ImageMakeHeight);
    device.defaultRegRegion().write(reg_base + 0x124, ImageMakePadReg);

    // 0x128~0x224
    for (int i = 0; i < ImageMakeChannel; i++)
    {
        device.defaultRegRegion().write(reg_base + (0x128 + i * 4), 0);
    }

    device.defaultRegRegion().write(reg_base + 0x240, ImageMakeChannel_each);
    device.defaultRegRegion().write(reg_base + 0x244, ImageMakeChannelTimes);
    device.defaultRegRegion().write(reg_base + 0x254, ImageMakeDataArrange);
};

// 特化的ZG300版initBuyiImageMake函数
// 接收经过PL_RESIZE后的rgb888（24bit）图像数据，将其拼为3*512bit（若不是整数倍，硬件内部会处理），然后再按每512bit写入PLDDR。
template <>
inline void initImageMake(icraft::xrt::ZG330Device &device, int imk_port,
                          int64_t ImageMakeWidth, int64_t ImageMakeHeight, int64_t ImageMakeChannel,
                          uint64_t ImageMakeWddrBase_a, uint64_t bits,
                          const std::vector<float> &premean_, const std::vector<float> &prescale_data)
{
    ICRAFT_LOG(DEBUG) << "initImageMake ZG330Device specialization called.";
    uint64_t reg_base;

    uint64_t total_bits = ImageMakeWidth * ImageMakeHeight * ImageMakeChannel * bits;
    uint64_t total_bytes = ImageMakeWidth * ImageMakeHeight * ImageMakeChannel;
    uint64_t bit_chunk_size = 3 * 512;
    uint64_t total_512bit_chunks = (total_bits + bit_chunk_size - 1) / bit_chunk_size; // 向上取整
    uint64_t last_valid_bits = total_bits % bit_chunk_size;
    // std::cout << "Total 512-bit chunks: " << total_512bit_chunks << std::endl;
    // std::cout << "Last valid channel bits in the final chunk: " << last_valid_bits << std::endl;
    uint64_t last_valid_512chunk_num = 2;
    if (last_valid_bits > 0)
        last_valid_512chunk_num = ((last_valid_bits + 511) / 512) - 1;
    // 根据配置的reg_base及input参数得到image_make的寄存器基地址
    // 当配置reg_base_不为空时，会优先将配置的基地址作为寄存器基地址
    // 当reg_base_为空时，会根据配置的input0~3确定各自的寄存器基地址，默认input0地址
    if (imk_port == 0)
        reg_base = 0x80000400;
    else if (imk_port == 1)
        reg_base = 0x80040000;
    else if (imk_port == 2)
        reg_base = 0x80040400;
    else if (imk_port == 3)
        reg_base = 0x80040800;
    else
    {
        ICRAFT_LOG(ERROR) << "imk_port should between 0~3, but is " << imk_port;
        ICRAFT_LOG(EXCEPT).append("imk_port should between 0~3, but is {}", imk_port);
    }
    const uint64_t REG_LAST_AWADDR = reg_base + 0x258;
    const uint64_t REG_LAST_VALID_CHN = reg_base + 0x8;
    // 检查硬件的版本信息是否正确，不正确会抛出错误
    uint64_t imk_version = device.defaultRegRegion().read(reg_base + 0x234);
    // spdlog::debug("ZG330 ImageMake#{} reg_base=0x{:x} imk_version: 0x{:x}", imk_port, reg_base, imk_version);
    if (imk_version != 0x20220623)
        ICRAFT_LOG(ERROR) << "No Image_Make HardWare, imk_version is " << imk_version;
    ICRAFT_CHECK(imk_version == 0x20220623)
        .append("No Image_Make HardWare, imk_version is {}", imk_version);

    // 根据pad参数，得到上下左右pad的size
    auto pad_r = 0;
    auto pad_l = 0;
    auto pad_b = 0;
    auto pad_t = 0;

    // 根据输入ftmp的通道数，确定每次处理图片的通道数
    // 目前只支持1~4通道输入ftmp数据
    auto ImageMakeChannel_each = 0;
    if (ImageMakeChannel % 4 == 0)
    {
        ImageMakeChannel_each = 4;
    }
    else if (ImageMakeChannel % 3 == 0)
    {
        ImageMakeChannel_each = 3;
    }
    else if (ImageMakeChannel % 2 == 0)
    {
        ImageMakeChannel_each = 2;
    }
    else
    {
        ImageMakeChannel_each = 1;
    }

    // 将上下左右的pad size拼接成一个32bit的数，该数用于写pad控制相关寄存器
    auto ImageMakePadReg = pad_t + (pad_b << 8) + (pad_l << 16) + (pad_r << 24);

    // 得到数据类型寄存器参数，当为8bit模式时为0，当为16bit模式时为1
    int ImageMakeDataArrange = 0;
    if (bits == 8)
    {
        ImageMakeDataArrange = 0;
    }
    else
    {
        ImageMakeDataArrange = 1;
    }

    // 调用device的写寄存器的接口，配置硬件需要的寄存器，其中writeReg和readReg中最后一个参数为true，用于整体寄存器空间偏移配置
    // 各寄存器表示含义介绍，参照ImakeMake硬算子用户手册

    device.defaultRegRegion().write(reg_base + 0x114, ImageMakeWddrBase_a);
    device.defaultRegRegion().write(reg_base + 0x118, ImageMakeWidth);
    device.defaultRegRegion().write(reg_base + 0x11C, ImageMakeHeight);
    // device.defaultRegRegion().write(reg_base + 0x124, ImageMakePadReg);
    device.defaultRegRegion().write(REG_LAST_VALID_CHN, last_valid_512chunk_num);
    uint64_t last_awaddr_value = ImageMakeWddrBase_a + ((total_bytes + 63) / 64 - 1) * 64; // 写入PLDDR的尾地址，512bit对齐
    device.defaultRegRegion().write(REG_LAST_AWADDR, last_awaddr_value);
    // Read all configured registers and log them
    // std::vector<std::pair<uint64_t, std::string>> registers = {
    //     {reg_base + 0x114, "ImageMakeWddrBase_a"},
    //     {reg_base + 0x118, "ImageMakeWidth"},
    //     {reg_base + 0x11C, "ImageMakeHeight"},
    //     {REG_LAST_VALID_CHN, "last_valid_chn"},
    //     {REG_LAST_AWADDR, "last_awaddr"}};

    // std::string reg_values = "ZG330 ImageMake configured registers: ";
    // for (const auto &[addr, name] : registers)
    // {
    //     uint64_t value = device.defaultRegRegion().read(addr);
    //     reg_values += fmt::format("{}=0x{:x} ", name, value);
    // }
    // spdlog::debug(reg_values);
};

template <typename DeviceType>
inline void runImageMakeForward(DeviceType &device, int imk_port,
                                int64_t ImageMakeWidth, int64_t ImageMakeHeight, int64_t ImageMakeChannel,
                                uint64_t ImageMakeWddrBase_a, uint64_t bits, bool verbose = false, int sleep_time = 0)
{
    // 运行时配寄存器
    // 主要用于调用多个runtime时的写PL DDR的基地址切换
    uint64_t reg_base;

    // 根据配置的reg_base及input参数得到image_make的寄存器基地址
    // 当配置reg_base_不为空时，会优先将配置的基地址作为寄存器基地址
    // 当reg_base_为空时，会根据配置的input0~3确定各自的寄存器基地址，默认input0地址
    if (imk_port == 0)
        reg_base = 0x80000400;
    else if (imk_port == 1)
        reg_base = 0x80040000;
    else if (imk_port == 2)
        reg_base = 0x80040400;
    else if (imk_port == 3)
        reg_base = 0x80040800;
    else
    {
        ICRAFT_LOG(ERROR) << "imk_port should between 0~3, but is " << imk_port;
        ICRAFT_LOG(EXCEPT).append("imk_port should between 0~3, but is {}", imk_port);
    }

    device.defaultRegRegion().write(reg_base + 0x114, ImageMakeWddrBase_a);
    // 轮询操作完成标志，done寄存器。只有等到done的时候才会跳出do while循环
    // 当一段时间等不到done之后，调用runtime的抛错接口，如果检测是输入数据量不够，会抛错Missing Input Data!!!
    // 否则会抛出ImageMake Timeout!!!
    unsigned int reg_pre_done;
    auto start = std::chrono::high_resolution_clock::now();
    do
    {
#ifndef _WIN32
        if (sleep_time > 0)
            usleep(sleep_time);
#endif
        reg_pre_done = device.defaultRegRegion().read(reg_base + 0x228);
        std::chrono::duration<double, std::milli> duration = std::chrono::high_resolution_clock::now() - start;
        if (duration.count() > 1000)
        {
            auto data_in_num = device.defaultRegRegion().read(reg_base + 0x260);
            if (data_in_num != ImageMakeWidth * ImageMakeHeight)
            {
                ICRAFT_LOG(ERROR) << "ImageMake Timeout";
                ICRAFT_LOG(ERROR) << "Image size is {" << ImageMakeWidth << "} * {" << ImageMakeHeight << "}, but accept {" << data_in_num << "} data";
            }
            ICRAFT_CHECK(data_in_num == ImageMakeWidth * ImageMakeHeight)
                .append("Image size is {} * {}, but accept {} data",
                        ImageMakeWidth, ImageMakeHeight, data_in_num);
            ICRAFT_LOG(ERROR) << "ImageMake Timeout";
            ICRAFT_LOG(EXCEPT).append("ImageMake Timeout");
        }
    } while (reg_pre_done == 0);

    // 硬件计时的寄存器，该寄存器会记录本模块硬件执行的周期数
    // 可以通过读reg_base + 0x22C这个地址的寄存器，得到模块执行的周期数
    // time cnt
    unsigned int imagemake_time_cnt = device.defaultRegRegion().read(reg_base + 0x238);
    device.defaultRegRegion().write(reg_base + 0x22C, imagemake_time_cnt);
    device.defaultRegRegion().write(reg_base + 0x228, 1); // 清除done

    // 打印一些DEBUG需要的信息
    if (verbose)
    {
        unsigned int reg_img_time_cnt = device.defaultRegRegion().read(reg_base + 0x238); // ImageMake_read_reg(ImageMake_TIME_CNT);
        printf("PL time_cnt is %d , PL image make time is =%.2f ms\n", reg_img_time_cnt, (reg_img_time_cnt * 10) / 1000000.0);
        printf("*******************************************\n");
    }
};

template <>
inline void runImageMakeForward(icraft::xrt::ZG330Device &device, int imk_port,
                                int64_t ImageMakeWidth, int64_t ImageMakeHeight, int64_t ImageMakeChannel,
                                uint64_t ImageMakeWddrBase_a, uint64_t bits, bool verbose, int sleep_time)
{
    ICRAFT_LOG(DEBUG) << "runImageMakeForward ZG330Device specialization called.";
    uint64_t reg_base;

    // 根据配置的reg_base及input参数得到image_make的寄存器基地址
    // 当配置reg_base_不为空时，会优先将配置的基地址作为寄存器基地址
    // 当reg_base_为空时，会根据配置的input0~3确定各自的寄存器基地址，默认input0地址
    if (imk_port == 0)
        reg_base = 0x80000400;
    else if (imk_port == 1)
        reg_base = 0x80040000;
    else if (imk_port == 2)
        reg_base = 0x80040400;
    else if (imk_port == 3)
        reg_base = 0x80040800;
    else
    {
        ICRAFT_LOG(ERROR) << "imk_port should between 0~3, but is " << imk_port;
        ICRAFT_LOG(EXCEPT).append("imk_port should between 0~3, but is {}", imk_port);
    }

    device.defaultRegRegion().write(reg_base + 0x114, ImageMakeWddrBase_a);
    // spdlog::debug("ZG330 runImageMakeForward port {} with reg_base=0x{:x} rewrite ImageMakeWddrBase_a=0x{:x}", imk_port, reg_base, ImageMakeWddrBase_a);
    // 轮询操作完成标志，done寄存器。只有等到done的时候才会跳出do while循环
    // 当一段时间等不到done之后，调用runtime的抛错接口，如果检测是输入数据量不够，会抛错Missing Input Data!!!
    // 否则会抛出ImageMake Timeout!!!
    unsigned int reg_pre_done;
    auto start = std::chrono::high_resolution_clock::now();
    // auto start = std::chrono::steady_clock::now();

    // Read all configured registers and log them
    // std::vector<std::pair<uint64_t, std::string>> registers = {
    //     {reg_base + 0x114, "ImageMakeWddrBase_a"},
    //     {reg_base + 0x118, "ImageMakeWidth"},
    //     {reg_base + 0x11C, "ImageMakeHeight"},
    //     {reg_base + 0x8, "last_valid_chn"},
    //     {reg_base + 0x258, "last_awaddr"}};

    // std::string reg_values = "ZG330 ImageMake configured registers: ";
    // for (const auto &[addr, name] : registers)
    // {
    //     uint64_t value = device.defaultRegRegion().read(addr);
    //     reg_values += fmt::format("{}=0x{:x} ", name, value);
    // }
    // spdlog::debug(reg_values);
    do
    {
#ifndef _WIN32
        if (sleep_time > 0)
            usleep(sleep_time);
#endif
        // spdlog::warn("ZG330 runImageMakeForward polling...");
        reg_pre_done = device.defaultRegRegion().read(reg_base + 0x228);
        // spdlog::warn("ZG330 runImageMakeForward reg_pre_done=0x{:x}", reg_pre_done);
        std::chrono::duration<double, std::milli> duration = std::chrono::high_resolution_clock::now() - start;
        // std::chrono::duration<double, std::milli> duration = std::chrono::steady_clock::now() - start;
        if (duration.count() > 1000)
        {
            auto data_in_num = device.defaultRegRegion().read(reg_base + 0x260);
            std::cout << "ZG330 ImageMake Timeout after " << duration.count() << " ms, data_in_num=" << data_in_num << std::endl;
            if (data_in_num != ImageMakeWidth * ImageMakeHeight)
            {
                ICRAFT_LOG(ERROR) << "ImageMake Timeout";
                ICRAFT_LOG(ERROR) << "Image size is {" << ImageMakeWidth << "} * {" << ImageMakeHeight << "}, but accept {" << data_in_num << "} data";
            }
            ICRAFT_CHECK(data_in_num == ImageMakeWidth * ImageMakeHeight)
                .append("Image size is {} * {}, but accept {} data",
                        ImageMakeWidth, ImageMakeHeight, data_in_num);
            ICRAFT_LOG(ERROR) << "ImageMake Timeout";
            ICRAFT_LOG(EXCEPT).append("ImageMake Timeout");
        }
    } while (reg_pre_done == 0);

    // 硬件计时的寄存器，该寄存器会记录本模块硬件执行的周期数
    // 可以通过读reg_base + 0x22C这个地址的寄存器，得到模块执行的周期数
    // time cnt
    unsigned int imagemake_time_cnt = device.defaultRegRegion().read(reg_base + 0x238);
    device.defaultRegRegion().write(reg_base + 0x22C, imagemake_time_cnt);
    device.defaultRegRegion().write(reg_base + 0x228, 1); // 清除done

    // 打印一些DEBUG需要的信息
    if (verbose)
    {
        unsigned int reg_img_time_cnt = device.defaultRegRegion().read(reg_base + 0x238); // ImageMake_read_reg(ImageMake_TIME_CNT);
        printf("PL time_cnt is %d , PL image make time is =%.2f ms\n", reg_img_time_cnt, (reg_img_time_cnt * 10) / 1000000.0);
        printf("*******************************************\n");
    }
};

namespace PLDDRMemRegion
{

    // pl_ddr dma
    const uint64_t PLDDR_DMA_BASE = 0x100041000;
    const uint64_t PLDDR_DMA_START = 0x04 + PLDDR_DMA_BASE;
    const uint64_t PLDDR_DMA_READ_BOTTOM = 0x18 + PLDDR_DMA_BASE;
    const uint64_t PLDDR_DMA_READ_TOP = 0x1C + PLDDR_DMA_BASE;
    const uint64_t PLDDR_DMA_WRITE_BOTTOM = 0x20 + PLDDR_DMA_BASE;
    const uint64_t PLDDR_DMA_WRITE_TOP = 0x24 + PLDDR_DMA_BASE;
    const uint64_t PLDDR_DMA_STATUS = 0x84 + PLDDR_DMA_BASE;

    const uint32_t PLDDR_DMA_ST_MASK_1 = 0b0000; // success
    const uint32_t PLDDR_DMA_ST_MASK_2 = 0b0011; // rdma err
    const uint32_t PLDDR_DMA_ST_MASK_3 = 0b1100; // wdma err
    const uint32_t PLDDR_DMA_ST_MASK_4 = 0b1111; // both wdma and rdma err
    const uint32_t PLDDR_DMA_ST_MASK_5 = 0b0001; // rdma un-done
    const uint32_t PLDDR_DMA_ST_MASK_6 = 0b0100; // wdma un-done
    const uint32_t PLDDR_DMA_ST_MASK_7 = 0b0101; // both wdma and rdma un-done
    const uint32_t PLDDR_DMA_ST_MASK_8 = 0b1101; // wdma err, rdma un-done
    const uint32_t PLDDR_DMA_ST_MASK_9 = 0b0111; // wdma un-done, rdma err
    const uint32_t PLDDR_DMA_ST_HIT = 0b1111;

    // bool statusHit(uint32_t status, uint32_t mask);
    inline bool statusHit(uint32_t status, uint32_t mask)
    {
        return status == mask;
    }

    // std::tuple<bool, uint64_t, int64_t> waitPLDMADone(int timeout_ms, const std::chrono::steady_clock::time_point& start, icraft::xrt::Device device);
    inline std::tuple<bool, uint64_t, int64_t> waitPLDMADone(int timeout_ms, const std::chrono::steady_clock::time_point &start, icraft::xrt::Device device)
    {
        uint64_t status = device.defaultRegRegion().read(PLDDR_DMA_STATUS, true);
        int64_t duration = -1;
        bool ret = icraft::xrt::utils::WaitUntil([&status, &start, &duration, &device]()
                                                 {
            status = device.defaultRegRegion().read(PLDDR_DMA_STATUS, true);
            //ICRAFT_LOG(INFO).append("internal status: {:#x}", status);
            if (statusHit(status, PLDDR_DMA_ST_MASK_1)) {
                auto finish = std::chrono::steady_clock::now();
                duration = (finish - start).count();
                return true;
            }
            return false; }, std::chrono::milliseconds(timeout_ms));
        // ICRAFT_LOG(INFO).append("return status: {:#x}, duration: {}", status, duration);
        return {ret, status, duration};
    }

    inline void Plddr_memcpy(uint64_t read_bottom, uint64_t read_top, uint64_t write_bottom, uint64_t write_top, icraft::xrt::Device &device)
    {
        // 作用：将PLDDR上src的数据拷贝给PLDDR上dest

        ICRAFT_LOG(INFO).append("Begin plddr memcpy...");

        // 自行在外部对齐数据
        // uint64_t read_bottom = src_begin_addr;
        // uint64_t read_top = read_bottom + byte_size - 64; //对齐64byte整数倍
        // uint64_t write_bottom = dest_addr;
        // uint64_t write_top = write_bottom + byte_size - 64;//对齐64byte整数倍

        static std::mutex plddr_dma_mutex_;
        // lock
        std::unique_lock<std::mutex> plddr_dma_lock(plddr_dma_mutex_);
        // write reg: [r_b, r_t] -> [w_b, w_t]
        device.defaultRegRegion().write(PLDDR_DMA_READ_BOTTOM, read_bottom, true);   // 输入数据的base地址
        device.defaultRegRegion().write(PLDDR_DMA_READ_TOP, read_top, true);         // 输入数据的结束地址
        device.defaultRegRegion().write(PLDDR_DMA_WRITE_BOTTOM, write_bottom, true); // 输出数据的base地址
        device.defaultRegRegion().write(PLDDR_DMA_WRITE_TOP, write_top, true);       // 输出数据的结束地址

        uint64_t aa = device.defaultRegRegion().read(PLDDR_DMA_READ_BOTTOM, true);
        uint64_t bb = device.defaultRegRegion().read(PLDDR_DMA_READ_TOP, true);
        uint64_t cc = device.defaultRegRegion().read(PLDDR_DMA_WRITE_BOTTOM, true);
        uint64_t dd = device.defaultRegRegion().read(PLDDR_DMA_WRITE_TOP, true);
        ICRAFT_LOG(INFO)
            .append("read_form: {}, read_to: {}, write_from: {}, write_to: {}",
                    aa, bb, cc, dd);

        // launch plddr dma
        auto start = std::chrono::steady_clock::now();
        uint64_t ee = device.defaultRegRegion().read(PLDDR_DMA_STATUS, true); // 启动后轮询，全0表示done
        ICRAFT_LOG(INFO).append("begin status: {:#x}", ee);
        // 启动数据传输
        device.defaultRegRegion().write(PLDDR_DMA_START, 1, true);
        device.defaultRegRegion().write(PLDDR_DMA_START, 0, true);
        auto [done, status, duration] = waitPLDMADone(1000, start, device);

        ICRAFT_LOG(INFO).append("(inner) PLDDR_PLDDR DMA time cost: {}ns", duration);

        if (!done)
        {
            if (statusHit(status, PLDDR_DMA_ST_MASK_2))
                ICRAFT_LOG(EXCEPT, 1301).append("Unexpected launch of RDMA when RDMA is running, while WDMA is running well.");
            else if (statusHit(status, PLDDR_DMA_ST_MASK_3))
                ICRAFT_LOG(EXCEPT, 1302).append("Unexpected launch of WDMA when WDMA is running, while RDMA is running well.");
            else if (statusHit(status, PLDDR_DMA_ST_MASK_4))
                ICRAFT_LOG(EXCEPT, 1303).append("Unexpected launches of both WDMA and RDMA when they are running.");
            else if (statusHit(status, PLDDR_DMA_ST_MASK_5))
                ICRAFT_LOG(EXCEPT, 1304).append("RDMA is un-done, while WDMA running well.");
            else if (statusHit(status, PLDDR_DMA_ST_MASK_6))
                ICRAFT_LOG(EXCEPT, 1305).append("WDMA is un-done, while RDMA running well.");
            else if (statusHit(status, PLDDR_DMA_ST_MASK_7))
                ICRAFT_LOG(EXCEPT, 1306).append("Both WDMA and RDMA are un-done.");
            else if (statusHit(status, PLDDR_DMA_ST_MASK_8))
                ICRAFT_LOG(EXCEPT, 1307).append("Unexpected launch of WDMA and RDMA is un-done.");
            else if (statusHit(status, PLDDR_DMA_ST_MASK_9))
                ICRAFT_LOG(EXCEPT, 1308).append("Unexpected launch of RDMA and WDMA is un-done.");
            else
                ICRAFT_LOG(EXCEPT, 1309).append("Unkown status of PLDDR DMA, which is {:#x}.", status);
        }
    }

}

#ifdef __linux__

class Camera
{
public:
    Camera() = default;

    Camera(icraft::xrt::BuyiDevice &device, uint64_t buffer_size, uint64_t base_addr = 0x40080000)
        : device_(device), buffer_size_(buffer_size), base_addr_(base_addr)
    {
        take_addr_ = base_addr_ + 0x04;
        write_addr_ = base_addr_ + 0x50;
        done_addr_ = base_addr_ + 0x58;
    }

    void get(int8_t *frame, const icraft::xrt::MemChunk &memchunk) const
    {
        memchunk.read((char *)frame, 0, buffer_size_);
    }

    void take(const icraft::xrt::MemChunk &memchunk) const
    {
        // 取帧到MemChunk处
        device_.defaultRegRegion().write(write_addr_, memchunk->begin.addr() >> 3);
        device_.defaultRegRegion().write(take_addr_, 1);
    }

    bool wait(int wait_time_ms = 100) const
    {
        bool error = false;
        bool done = false;
        WaitUntil(
            [&]() -> bool
            {
                auto camera_done = device_.defaultRegRegion().read(done_addr_);
                error = camera_done & 0x4;
                done = camera_done & 0x1;
                return done;
            },
            std::chrono::duration<int, std::milli>(wait_time_ms)
            // 100ms
        );
        if (error)
        {
            std::cout << std::bitset<32>(device_.defaultRegRegion().read(done_addr_)) << std::endl;
            std::cout << std::dec;
        }
        return !error && done;
    }

private:
    icraft::xrt::BuyiDevice device_;
    uint64_t buffer_size_ = 0;
    uint64_t base_addr_;
    uint64_t take_addr_;
    uint64_t write_addr_;
    uint64_t done_addr_;
};

/**
 *   Hdmi显示抽象类
 *   用于wukong板
 *   输入的数据为 RGB565
 *   尺寸是1920*1080
 */
class Display_pHDMI_RGB565
{
public:
    Display_pHDMI_RGB565() = default;

    Display_pHDMI_RGB565(icraft::xrt::BuyiDevice device, uint64_t buffer_size, icraft::xrt::MemChunk chunck)
        : device_(device), buffer_size_(buffer_size), chunck_(chunck)
    {
    }

    void show(int8_t *frame) const
    {
        chunck_.write(0, (char *)frame, buffer_size_);
        device_.defaultRegRegion().write(DISPLAY_READ_ADDR, chunck_->begin.addr() >> 3);
    }

private:
    icraft::xrt::BuyiDevice &device_;
    uint64_t buffer_size_ = 0;
    icraft::xrt::MemChunk chunck_;
    const static auto DISPLAY_READ_ADDR = 0x40080054;
};

/**
 *   Hdmi显示抽象类
 *   用于demov1板子， 做成framebuffer驱动，
 *   输入的数据为 RGBA
 *   尺寸是1920*1080
 */
class Display_sHDMI_RGBA
{
public:
    Display_sHDMI_RGBA() = default;

    Display_sHDMI_RGBA(const char *dev)
    {
        int ret = 0;
        fd_ = open(dev, O_RDWR);
        if (fd_ < 0)
        {
            printf("open device [%s] failed:%s\n", dev, strerror(errno));
            return;
        }

        ret = ioctl(fd_, FBIOGET_FSCREENINFO, &fix);
        if (ret < 0)
        {
            printf("read fb device fscreeninfo failed:%s\n", strerror(errno));
            close(fd_);
            fd_ = -1;
            return;
        }

        ret = ioctl(fd_, FBIOGET_VSCREENINFO, &var);
        if (ret < 0)
        {
            printf("read fb device vscreeninfo failed:%s\n", strerror(errno));
            close(fd_);
            fd_ = -1;
            return;
        }

        mem_size_ = var.xres * var.yres * var.bits_per_pixel / 8; /* 计算内存 */
        ptr_buf = (uint8_t *)mmap(NULL, mem_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (ptr_buf == MAP_FAILED)
        {
            printf("fb device mmap failed:%s\n", strerror(errno));
            close(fd_);
            fd_ = -1;
            return;
        }

        memset(ptr_buf, 0, mem_size_); // 清除屏幕
    }

    ~Display_sHDMI_RGBA()
    {
        if (ptr_buf && mem_size_ > 0)
        {
            munmap(ptr_buf, mem_size_);
            ptr_buf = nullptr;
        }
        if (fd_ >= 0)
        {
            close(fd_);
            fd_ = -1;
        }
    }

    void show(int8_t *frame) const
    {
        memcpy(ptr_buf, frame, mem_size_);
    }

    void draw_top_left(int8_t *frame)
    {
        uint8_t *poffset_buf = ptr_buf;
        for (int col = 0; col < var.yres; ++col)
        {
            memcpy(poffset_buf, frame, var.xres / 2);
            poffset_buf += var.xres;
            frame += var.xres / 2;
        }
    }

    void draw_top_right(int8_t *frame)
    {
        uint8_t *poffset_buf = ptr_buf + var.xres / 2;
        for (int col = 0; col < var.yres; ++col)
        {
            memcpy(poffset_buf, frame, var.xres / 2);
            poffset_buf += var.xres;
            frame += var.xres / 2;
        }
    }

    void draw_bottom_left(int8_t *frame)
    {
        uint8_t *poffset_buf = ptr_buf + var.yres * var.xres / 2;
        for (int col = 0; col < var.yres; ++col)
        {
            memcpy(poffset_buf, frame, var.xres / 2);
            poffset_buf += var.xres;
            frame += var.xres / 2;
        }
    }

    void draw_bottom_right(int8_t *frame)
    {
        uint8_t *poffset_buf = ptr_buf + var.yres * var.xres / 2 + var.xres / 2;
        for (int col = 0; col < var.yres; ++col)
        {
            memcpy(poffset_buf, frame, var.xres / 2);
            poffset_buf += var.xres;
            frame += var.xres / 2;
        }
    }

    void draw_pixel(int x, int y, uint32_t color)
    {
        uint8_t *poffset_buf = NULL;

        poffset_buf = ptr_buf + (x * var.bits_per_pixel / 8) + (y * var.xres * var.bits_per_pixel / 8); /* 计算内存偏移地址 */
        *(uint32_t *)poffset_buf = color;                                                               /* ARGB32格式 */
    }

    void fill_pixel(uint32_t color)
    {
        int i, j;

        for (i = 0; i < var.xres; i++)
        {
            for (j = 0; j < var.yres; j++)
            {
                draw_pixel(i, j, color);
            }
        }
    }

    uint8_t *getPtr() const { return ptr_buf; }

private:
    uint8_t *ptr_buf = nullptr;
    int fd_ = -1;
    int mem_size_ = 0;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var; /* framebuffer设备信息*/
};

#endif
