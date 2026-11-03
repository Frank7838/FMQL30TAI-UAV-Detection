// 默认相机输入的CPU预处理。640×480到640×640采用右/下填充114。
// 同尺寸区域直接BGR转RGB，NEON批量处理并补尾像素，避免中间复制。

#pragma once
#include <algorithm>
#include <arm_neon.h>
#include <cmath>
#include <stdexcept>
#include <tuple>
#include "PicPre.hpp"

// For the stream worker only: BGR UINT8, LONG_SIDE, right/bottom 114 padding.
// The caller retains bgr until postprocessing finishes. Visualization is disabled.
class FusedStreamPicPre final : public PicPre {
  public:
    bool fast_path = false;
    // target顺序为(height,width)。src/ori_img只共享只读原图，坐标回映射仍使用原几何。
    FusedStreamPicPre(const cv::Mat &bgr, std::pair<int, int> target) {
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
        // 当前640×480输入无需缩放：先初始化640×640为114，再直接在有效区域写RGB。
        // 有效内容左上对齐，右侧和下方填充114。
        if (w == bgr.cols && h == bgr.rows) {
            // No resize is needed. Write RGB straight into the final padded
            // image, avoiding resize-copy, temporary RGB and border-copy.
            dst_img = cv::Mat(target.first, target.second, CV_8UC3, cv::Scalar(114, 114, 114));
            for (int y = 0; y < h; ++y) {
                const auto *source = bgr.ptr<unsigned char>(y);
                auto *destination = dst_img.ptr<unsigned char>(y);
                int x = 0;
                // NEON一次解交织16个BGR像素并交换R/B；每行使用Mat::ptr，保留真实stride。
                for (; x + 16 <= w; x += 16) {
                    auto pixels = vld3q_u8(source + 3 * x);
                    auto first = pixels.val[0];
                    pixels.val[0] = pixels.val[2];
                    pixels.val[2] = first;
                    vst3q_u8(destination + 3 * x, pixels);
                }
                // 不足16个像素的尾部使用标量，不能越过行尾读取。
                for (; x < w; ++x) {
                    destination[3 * x] = source[3 * x + 2];
                    destination[3 * x + 1] = source[3 * x + 1];
                    destination[3 * x + 2] = source[3 * x];
                }
            }
            _pad_info = {0, 0};
            fast_path = true;
        // 精确3:1缩放时，INTER_LINEAR落在(3x+1,3y+1)的整像素中心，可直接采样等价中心。
        } else if (bgr.cols == 3 * w && bgr.rows == 3 * h) {
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
            // 其他比例回退OpenCV线性缩放及原PicPre补边，保持旧输入合同而非近似替换。
            cv::Mat small_bgr;
            cv::resize(bgr, small_bgr, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
            cv::cvtColor(small_bgr, dst_img, cv::COLOR_BGR2RGB);
            rPad(PicPre::BR);
        }
    }
};
