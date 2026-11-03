// 1920×1080紧凑采样的备用传输分支；默认640×480相机不使用。

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include "PicPre.hpp"

constexpr uint32_t CENTER_REQUEST_MAGIC = 0x33534750;
constexpr uint32_t CENTER_WIDTH = 1920, CENTER_HEIGHT = 1080;
constexpr int CENTER_SMALL_WIDTH = 640, CENTER_SMALL_HEIGHT = 360;

// Fixed factor-three representation. The Python renderer retains the real BGR
// source. This worker has only its centers and cannot visualize an original.
// A real, initialized matrix supplies coordTrans's original clipping bounds;
// no cv::Mat header, stride, dimensions or backing allocation is forged.
// Genuine owning allocations; the synchronous worker retains these until exit.
// Coordinate bytes are never consumed as source colors. RGB top is completely
// overwritten per request; initialized right/bottom padding stays114. SDK helper
// copies the RGB into an independent Tensor before the next request is accepted.
struct CenterPacketStorage {
    const cv::Mat coordinates{1080, 1920, CV_8UC1, cv::Scalar(0)};
    cv::Mat rgb{640, 640, CV_8UC3, cv::Scalar(114, 114, 114)};
};

class CenterPacketPicPre final : public PicPre {
  public:
    CenterPacketPicPre(const cv::Mat &centers, uint32_t width, uint32_t height,
                       std::pair<int, int> target, CenterPacketStorage &storage) {
        if (width != CENTER_WIDTH || height != CENTER_HEIGHT ||
            target != std::pair<int, int>{640, 640} || centers.type() != CV_8UC3 ||
            centers.rows != CENTER_SMALL_HEIGHT || centers.cols != CENTER_SMALL_WIDTH)
            throw std::invalid_argument("unsupported center packet geometry or model input");
        src_dims = {3, int(height), int(width)};
        src_img = storage.coordinates;
        // ori_img intentionally empty: postprocess show/save are forbidden.
        _dst_shape = target;
        const float ratio =
            std::min(float(target.second) / float(width), float(target.first) / float(height));
        _real_resized_ratio = {ratio, ratio};
        _real_resized_hw = {CENTER_SMALL_HEIGHT, CENTER_SMALL_WIDTH};
        _pad_info = {0, 0};
        dst_img = storage.rgb;
        cv::Mat top = dst_img(cv::Rect(0, 0, CENTER_SMALL_WIDTH, CENTER_SMALL_HEIGHT));
        cv::cvtColor(centers, top, cv::COLOR_BGR2RGB);
    }

    PicPre &Resize(std::pair<int, int>, int = LONG_SIDE, int = INTER_LINEAR) override {
        throw std::logic_error("center packet is already resized; source colors unavailable");
    }
    void rPad(int = BR) override { throw std::logic_error("center packet is already padded"); }
};

inline bool validate_center_header(const uint32_t (&request)[4], size_t processed,
                                   std::pair<int, int> target) {
    if (request[0] != CENTER_REQUEST_MAGIC)
        return false;
    if (request[1] != static_cast<uint32_t>(processed) || request[2] != CENTER_WIDTH ||
        request[3] != CENTER_HEIGHT || target != std::pair<int, int>{640, 640})
        throw std::invalid_argument("center request frame ID, geometry or model mismatch");
    return true;
}
