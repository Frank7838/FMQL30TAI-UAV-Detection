// CPU预处理逐字节对照实现，仅用于首次4帧校验和CPU自检；不是PL加速。

#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>
#include "PicPre.hpp"

// For the stream worker only: BGR UINT8, LONG_SIDE, right/bottom 114 padding.
// The caller retains bgr until postprocessing finishes. Visualization is disabled.
class LegacyFusedStreamPicPre final : public PicPre {
  public:
    bool fast_path = false;
    LegacyFusedStreamPicPre(const cv::Mat &bgr, std::pair<int, int> target) {
        if (bgr.empty() || bgr.type() != CV_8UC3 || target.first <= 0 || target.second <= 0)
            throw std::invalid_argument("StreamPicPre requires BGR UINT8 and positive target");
        src_dims = {3, bgr.rows, bgr.cols};
        ori_img = bgr; // Read-only shared ownership; no full-resolution clone.
        src_img =
            bgr; // coordTrans reads cols/rows. No base Resize or source-color consumer is used.
        _dst_shape = target;
        const float ratio_h = float(target.first) / float(bgr.rows);
        const float ratio_w = float(target.second) / float(bgr.cols);
        const float ratio = std::min(ratio_w, ratio_h);
        const int h = int(std::round(bgr.rows * ratio));
        const int w = int(std::round(bgr.cols * ratio));
        if (!h || !w)
            throw std::invalid_argument("Rounded resize dimension is zero");
        _real_resized_ratio = {ratio, ratio};
        _real_resized_hw = {h, w};
        if (bgr.cols == 3 * w && bgr.rows == 3 * h) {
            // INTER_LINEAR at exact scale 3 samples (3*x+1, 3*y+1).
            // Preserve all source pixels outside these centers and the source lifetime.
            dst_img = cv::Mat(target.first, target.second, CV_8UC3, cv::Scalar(114, 114, 114));
            for (int y = 0; y < h; ++y) {
                const auto *source = bgr.ptr<unsigned char>(3 * y + 1);
                auto *destination = dst_img.ptr<unsigned char>(y);
                for (int x = 0; x < w; ++x) {
                    const int input = 9 * x + 3;
                    const int output = 3 * x;
                    destination[output] = source[input + 2];
                    destination[output + 1] = source[input + 1];
                    destination[output + 2] = source[input];
                }
            }
            _pad_info = {0, 0};
            fast_path = true;
        } else {
            cv::Mat small_bgr;
            cv::resize(bgr, small_bgr, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
            cv::cvtColor(small_bgr, dst_img, cv::COLOR_BGR2RGB);
            rPad(PicPre::BR);
        }
    }
};
