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
#include <iostream>

#ifdef __linux__
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/fb.h>
#endif

#include "opencv2/opencv.hpp"

#include <spdlog/spdlog.h>
#include "icraft-xir/core/network.h"
#include "icraft-xir/core/data.h"
#include "icraft-xrt/dev/host_device.h"
#include "icraft-xrt/dev/buyi_device.h"
#include "icraft-backends/hostbackend/utils.h"

//-------------------------------------//
//       AXI PLin通用
//-------------------------------------//
struct ConvertInfo
{
    std::vector<int> dims;
    std::vector<std::pair<int, int>> scale;
    std::vector<float> norm_ratio;
    int bits;
};

/**
 * @description: 从names文件中读取每个index对应的标签名称
 * @param  name_path  names文件路径
 * @return 返回指向RuntineTensor的智能指针
 * @notes:
 */
inline std::vector<std::string> getLabelName(const std::string &name_path)
{

    std::ifstream names_stream(name_path);
    std::string line;
    std::vector<std::string> labels;

    while (std::getline(names_stream, line))
    {
        labels.push_back(line);
    }
    if (labels.size() == 0)
    {
        throw std::runtime_error(fmt::format("name file {} don't exist!", name_path));
    }

    return labels;
}

/**
 * @description: 从RuntimeNetwork 中获取维度和量化参数信息
 * @param network_ptr   指向RuntimeNetwork的智能指针
 * @return {*}
 */
inline std::vector<float> getDetPostNormratio(icraft::xir::Network network)
{

    auto ops = network->ops;
    icraft::xir::Operation det_post_op;
    for (auto &&op : ops)
    {
        if (op->typeKey() == "customop::DetPostNode")
        {
            det_post_op = op;
            break;
        }
    }
    std::vector<float> ret;
    for (auto &&value : det_post_op->inputs)
    {
        auto b = value->dtype.getNormratio().value()->data;
        ret.emplace_back(b[0]);
    }
    return ret;
}

inline std::string get_date_timestamp_string()
{
    // Get the current time with millisecond precision
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    auto epoch = now_ms.time_since_epoch();
    auto value = std::chrono::duration_cast<std::chrono::milliseconds>(epoch);
    long duration = value.count();

    // Convert time to string
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm *now_tm = std::localtime(&now_time);

    std::stringstream ss;
    ss << std::put_time(now_tm, "%y-%m-%d_%H%M%S") << "." << std::setw(3) << std::setfill('0') << duration % 1000;  // yyyy-mm-dd_HH:MM:SS.mmm
    return ss.str();
}

inline std::string get_timestamp_string()
{
    // Get the current time with millisecond precision
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    auto epoch = now_ms.time_since_epoch();
    auto value = std::chrono::duration_cast<std::chrono::milliseconds>(epoch);
    long duration = value.count();

    // Convert time to string
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm *now_tm = std::localtime(&now_time);

    std::stringstream ss;
    ss << std::put_time(now_tm, "%H%M%S") << "." << std::setw(3) << std::setfill('0') << duration % 1000; // HH:MM:SS.mmm
    return ss.str();
}

inline bool is_in_bbox(int x, int y, int x0, int y0, int w, int h)
{
    return (x >= x0) && (x <= x0 + w) && (y >= y0) && (y <= y0 + h);
}

inline void printout_hex(const uint8_t *data, size_t data_sz, size_t limit = 64)
{
    // debug print data in hex
    int print_len = (data_sz > limit) ? limit : data_sz;
    std::cout << data_sz << " bytes: ";
    for (int j = 0; j < print_len; j++)
    {
        std::cout << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(data[j]) << " ";
    }
    std::cout << std::dec << std::endl;
}