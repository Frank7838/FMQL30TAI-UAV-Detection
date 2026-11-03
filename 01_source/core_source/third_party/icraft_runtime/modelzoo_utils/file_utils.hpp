#pragma once
#include <filesystem>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>

#include "yaml-cpp/yaml.h"

// Yaml config file helpers
inline void read_node_urls(const YAML::Node &node, const std::string &key, std::vector<std::string> &urls)
{
    const auto &url_node = node[key];
    // 3. 检查节点是否存在并且是一个序列（列表）
    if (url_node && url_node.IsSequence())
    {
        // 4. 遍历序列并将每个 URL 添加到 vector 中
        for (const auto &item : url_node)
        {
            urls.push_back(item.as<std::string>());
        }
    }
    else
    {
        std::cerr << "错误: 未在配置文件" << node << "中找到 '" << key << "' 列表或其格式不正确。" << std::endl;
        throw std::runtime_error("Invalid or missing URL list in config");
    }
}

inline std::vector<std::string> toVector(const std::string &txt_path)
{
    std::vector<std::string> str_vec;
    std::ifstream iFile(txt_path);
    std::string tmp;
    if (!iFile.is_open())
    {
        std::string error_ = "***error: " + txt_path + " does not exist***";
        std::cout << error_ << std::endl;
        exit(EXIT_FAILURE);
    }
    while (std::getline(iFile, tmp))
    {
        tmp.erase(std::find_if(tmp.rbegin(), tmp.rend(), [](int ch)
                               { return !std::isspace(ch); })
                      .base(),
                  tmp.end());
        str_vec.push_back(tmp);
    }
    iFile.close();
    return str_vec;
}

inline void checkDir(const std::string path)
{
    if (!std::filesystem::exists(path))
    {
        std::filesystem::create_directory(path);
        std::cout << "\n"
                  << path << "is not exist, create new one !" << std::endl;
    }
}

inline std::string getFilename(const std::string &img_path)
{
    std::regex reg(R"((/|\\|^)([^\\/:\*\?"<>\|]+)\.([\w]*)$)");
    std::smatch sm;
    std::regex_search(img_path, sm, reg);
    std::string img_name = std::string(sm[2]);
    return img_name;
}

// 排序函数：仅提取文件名中的数字部分进行排序
inline bool numericCompare(const std::string &a, const std::string &b)
{
    // 使用正则表达式提取数字
    static const std::regex num_regex(R"((\d+))");
    std::smatch match_a, match_b;

    // 尝试在文件名中查找数字
    bool found_a = std::regex_search(a, match_a, num_regex);
    bool found_b = std::regex_search(b, match_b, num_regex);

    // 如果两个文件名都有数字，按数字值比较
    if (found_a && found_b)
    {
        int num_a = std::stoi(match_a[1]);
        int num_b = std::stoi(match_b[1]);
        return num_a < num_b;
    }

    // 否则按原始字符串排序
    return a < b;
}

inline std::vector<std::string> listFilenames(const std::filesystem::path &directory_path)
{
    // 验证路径有效性
    if (!std::filesystem::exists(directory_path))
    {
        throw std::runtime_error("路径不存在: " + directory_path.string());
    }
    std::vector<std::string> filenames;

    // 遍历目录
    for (const auto &entry : std::filesystem::directory_iterator(directory_path))
    {
        if (entry.is_regular_file())
        {
            filenames.push_back(entry.path().filename().string());
        }
    }

    std::sort(filenames.begin(), filenames.end());

    return filenames;
}

inline std::vector<std::string> getFullFilePathsFromList(const std::filesystem::path &directory_path, const std::filesystem::path &txt_fn)
{
    std::vector<std::string> filenames;
    std::ifstream iFile(txt_fn);

    if (!iFile.is_open())
    {
        std::cerr << "***error: " << txt_fn << " does not exist***" << std::endl;
        throw std::runtime_error("Failed to open file: " + txt_fn.string());
    }

    std::string filename;
    while (std::getline(iFile, filename))
    {
        // 移除行尾空白字符
        filename.erase(std::find_if(filename.rbegin(), filename.rend(), [](int ch)
                                    { return !std::isspace(ch); })
                           .base(),
                       filename.end());

        if (filename.empty())
            continue;

        // 拼接完整路径
        std::filesystem::path full_path = directory_path / filename;

        // 检查文件是否存在
        if (!std::filesystem::exists(full_path))
        {
            std::cerr << "***error: file " << full_path << " does not exist***" << std::endl;
            continue;
        }

        filenames.push_back(full_path.string());
    }

    iFile.close();
    return filenames;
}

inline void progress(int index, int total)
{
    int bar_length = 25;
    std::cout << "\r" << index + 1 << "/" << total << " [";
    for (int i = 0; i < bar_length; ++i)
    {
        int prog = float(index + 1) / float(total) * float(bar_length);
        if (i < prog)
            std::cout << "=";
        if (i == prog)
            std::cout << ">";
        if (i > prog)
            std::cout << " ";
    }
    std::cout << "] " << float(index + 1) / float(total) * 100.0 << "%" << std::flush;
}