#pragma once // 防止头文件被重复包含

// 定义日志宏，自动添加 LogP 前缀
#include <spdlog/spdlog.h>

// ##__VA_ARGS__ 是一个技巧，用于处理没有可变参数(...)的情况，防止末尾出现多余的逗号
#define LOG_DEBUG(pre, fmt, ...) spdlog::debug("{}" fmt, pre, ##__VA_ARGS__)
#define LOG_INFO(pre, fmt, ...) spdlog::info("{}" fmt, pre, ##__VA_ARGS__)
#define LOG_WARN(pre, fmt, ...) spdlog::warn("{}" fmt, pre, ##__VA_ARGS__)
#define LOG_ERROR(pre, fmt, ...) spdlog::error("{}" fmt, pre, ##__VA_ARGS__)
