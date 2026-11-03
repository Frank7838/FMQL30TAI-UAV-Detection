// 业务ROI配置检查。CPU与RTL须保持相同归一化坐标和半开边界。

#pragma once
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <stdexcept>
namespace roi_m705 {
using Roi = std::array<uint32_t, 4>;
constexpr Roi DEFAULT{{8192, 4096, 57344, 57344}};
// 将配置区域限制在硬件支持的固定ROI内。
inline void validate(const Roi &r) {
    if (!(8192 <= r[0] && r[0] < r[2] && r[2] <= 57344 && 4096 <= r[1] && r[1] < r[3] &&
          r[3] <= 57344))
        throw std::runtime_error("Unsupported ROI; qualified fixed PL subregion required");
}
// 像素坐标四舍五入至Q16并限制到0..65535，数学约定与Python端一致。
inline uint32_t coordinate(double v, uint32_t size) {
    if (!size || !std::isfinite(v))
        throw std::runtime_error("Invalid ROI coordinate");
    return uint32_t(std::max(0., std::min(65535., std::floor(v * 65536. / size + .5))));
}
// 保留的ROI子集工具；现用product_stream返回原始框，实际告警由PL判定。
// 阅读此函数不能推断当前视频显示或评分会按ROI丢弃检测框。
inline std::vector<std::vector<float>> select(const std::vector<std::vector<float>> &rows,
                                              uint32_t w, uint32_t h, const Roi &roi) {
    validate(roi);
    if (rows.size() > 16)
        throw std::runtime_error("Raw box capacity exceeded");
    if (roi == DEFAULT)
        return rows;
    std::vector<std::vector<float>> out;
    for (const auto &d : rows) {
        if (d.size() != 6 || d[0] != 0 ||
            !std::all_of(d.begin(), d.end(), [](float v) { return std::isfinite(v); }) ||
            d[3] <= 0 || d[4] <= 0 || d[5] < 0 || d[5] > 1)
            throw std::runtime_error("Malformed raw detection");
        auto x0 = coordinate(d[1], w), y0 = coordinate(d[2], h),
             x1 = coordinate(double(d[1]) + d[3], w), y1 = coordinate(double(d[2]) + d[4], h);
        if (x0 >= x1 || y0 >= y1)
            throw std::runtime_error("Normalized box collapsed");
        uint32_t x = x0 + x1, y = y0 + y1;
        if (2 * roi[0] <= x && x < 2 * roi[2] && 2 * roi[1] <= y && y < 2 * roi[3])
            out.push_back(d);
    }
    return out;
}
} // namespace roi_m705
