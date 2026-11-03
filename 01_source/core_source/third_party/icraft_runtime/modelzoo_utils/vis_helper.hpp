/**
 * @file vis_helper.hpp
 * @brief This header file contains helper classes and functions for visualization tasks,
 * primarily using the OpenCV library. Look up visualization helper functions and classes in this header file.
 */
#pragma once

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <iostream>
#include <mutex>
#include <fmt/core.h>

/**
 * @class DisplayRange
 * @brief Encapsulates a specific rectangular sub-region of an OpenCV cv::Mat.
 *
 * This class stores the coordinates of a rectangular region and provides a
 * cv::Mat view of that region from a larger source matrix. It is useful for
 * managing and accessing specific parts of an image or matrix for display or
 * processing.
 */

class DisplayRange
{
public:
    /**
     * @brief Constructs a DisplayRange object.
     *
     * Creates a view of a sub-region from the given source matrix based on the
     * specified row and column ranges. Note that the internal cv::Mat is a view
     * (a shallow copy) and shares data with the original matrix.
     *
     * @param startrow The starting row index (inclusive) of the sub-region.
     * @param endrow The ending row index (exclusive) of the sub-region.
     * @param startcol The starting column index (inclusive) of the sub-region.
     * @param endcol The ending column index (exclusive) of the sub-region.
     * @param mat The source cv::Mat from which the sub-region is extracted.
     */
    DisplayRange(int startrow, int endrow, int startcol, int endcol, const cv::Mat &mat)
        : startrow_(startrow), endrow_(endrow), startcol_(startcol), endcol_(endcol)
    {
        mat_ = mat.rowRange(startrow, endrow).colRange(startcol, endcol);
    }

    const cv::Mat &mat() const { return mat_; }

    const int startrow() const { return startrow_; }
    const int endrow() const { return endrow_; }
    const int startcol() const { return startcol_; }
    const int endcol() const { return endcol_; }

private:
    int startrow_;
    int endrow_;
    int startcol_;
    int endcol_;
    cv::Mat mat_;
};

/**
 * @class ProgressPrinter
 * @brief A thread-safe helper class for rendering multi-line progress bars in a console.
 *
 * This class manages and prints multiple lines of text-based progress bars to the
 * standard output. It uses ANSI escape codes to move the cursor and update the
 * progress display in-place, preventing the console from scrolling. A std::mutex
 * is used to ensure that updates from different threads do not interleave, making
 * it suitable for visualizing the progress of concurrent tasks.
 */
class ProgressPrinter
{
public:
    ProgressPrinter(int line = 0) : line_(line)
    {
        this->lines_ = std::vector<std::string>(line);
    }

    void print(int line_index, int progress, int total_n, std::string pre_info, std::string last_info)
    {
        if (line_index > this->lines_.size())
            return;
        auto full_info = pre_info + " " + std::to_string(progress) + "/" + std::to_string(total_n) + "[";

        for (int i = 0; i < 50; ++i)
        {
            int prog = float(progress) / float(total_n) * 100.0 / 2.0;
            if (i < prog)
                full_info += "=";
            if (i == prog)
                full_info += ">";
            if (i > prog)
                full_info += " ";
        }
        full_info += +"]";
        full_info += fmt::format(" {:.2f}% ", float(progress) / float(total_n) * 100.0);
        full_info += last_info;

        this->lines_[line_index] = full_info;
        std::string to_topline = "\033[" + std::to_string(line_) + "A";
        std::unique_lock<std::mutex> prt_lock(prt_mutex_);
        std::cout << "\033[?25l" << "\033[K" << to_topline << "\033[0m" << "\r";
        for (auto &&line : this->lines_)
        {
            std::cout << "\033[K" << line << '\n';
        }
        prt_lock.unlock();
    }

private:
    int line_;
    std::vector<std::string> lines_;
    std::mutex prt_mutex_;
    bool first_print_ = true;
};

// ************ Webcam display helper functions
// BGR to YUV color conversion
struct YUVColor
{
    uint8_t y;
    uint8_t u;
    uint8_t v;
};

// Pre-calculated YUV color map corresponding to kColorMap
const YUVColor kYUVColorMap[] = {
    {110, 255, 90},  // {B:255, G:128, R:  0} -> Azure
    {132, 242, 80},  // {B:255, G:153, R: 51}
    {155, 229, 70},  // {B:255, G:178, R:102}
    {214, 138, 0},   // {B:230, G:230, R:  0} -> Lime
    {172, 171, 255}, // {B:255, G:153, R:255} -> Pink
    {188, 143, 214}, // {B:153, G:204, R:255}
    {147, 184, 255}, // {B:255, G:102, R:255}
    {122, 197, 255}, // {B:255, G: 51, R:255}
    {171, 118, 224}, // {B:102, G:178, R:255}
    {146, 105, 235}, // {B: 51, G:153, R:255}
    {172, 171, 171}, // {B:255, G:153, R:153}
    {147, 184, 150}, // {B:255, G:102, R:102}
    {122, 197, 130}, // {B:255, G: 51, R: 51}
    {201, 63, 97},   // {B:153, G:255, R:153}
    {176, 50, 87},   // {B:102, G:255, R:102}
    {151, 37, 76},   // {B: 51, G:255, R: 51}
    {149, 43, 21},   // {B:  0, G:255, R:  0} -> Green
    {29, 112, 255},  // {B:  0, G:  0, R:255} -> Red
    {76, 255, 41},   // {B:255, G:  0, R:  0} -> Blue
    {225, 128, 149}  // {B:128, G:255, R:255}
};

// Function to get a YUV color by class ID
inline YUVColor classColorYUV(int id)
{
    return kYUVColorMap[id % 20];
}

inline YUVColor BGR2YUV(const cv::Scalar &bgr)
{
    double b = bgr[0], g = bgr[1], r = bgr[2];
    YUVColor yuv;
    yuv.y = cv::saturate_cast<uint8_t>(0.299 * r + 0.587 * g + 0.114 * b);
    yuv.u = cv::saturate_cast<uint8_t>(-0.169 * r - 0.331 * g + 0.500 * b + 128);
    yuv.v = cv::saturate_cast<uint8_t>(0.500 * r - 0.419 * g - 0.081 * b + 128);
    return yuv;
}

// Draws a rectangle directly on an NV21 cv::Mat
inline void drawRectangleNV21(cv::Mat &nv21_mat, const cv::Rect &rect, const YUVColor &color, int thickness)
{
    if (nv21_mat.empty() || rect.width <= 0 || rect.height <= 0)
    {
        return;
    }

    int width = nv21_mat.cols;
    int height = nv21_mat.rows * 2 / 3; // NV21 Mat height is 1.5x actual image height
    uint8_t *y_plane = nv21_mat.data;
    uint8_t *uv_plane = y_plane + width * height;

    // Clamp rectangle to image boundaries
    cv::Rect clamped_rect = rect & cv::Rect(0, 0, width, height);

    // Draw horizontal lines
    for (int t = 0; t < thickness; ++t)
    {
        int y_top = clamped_rect.y + t;
        int y_bottom = clamped_rect.y + clamped_rect.height - 1 - t;

        if (y_top >= 0 && y_top < height)
        {
            for (int x = clamped_rect.x; x < clamped_rect.x + clamped_rect.width; ++x)
            {
                y_plane[y_top * width + x] = color.y;
                int uv_idx = (y_top / 2) * width + (x / 2) * 2;
                uv_plane[uv_idx] = color.v;
                uv_plane[uv_idx + 1] = color.u;
            }
        }
        if (y_bottom >= 0 && y_bottom < height && t > 0)
        { // Avoid double-drawing for 1px height
            for (int x = clamped_rect.x; x < clamped_rect.x + clamped_rect.width; ++x)
            {
                y_plane[y_bottom * width + x] = color.y;
                int uv_idx = (y_bottom / 2) * width + (x / 2) * 2;
                uv_plane[uv_idx] = color.v;
                uv_plane[uv_idx + 1] = color.u;
            }
        }
    }
    // Draw vertical lines
    for (int t = 0; t < thickness; ++t)
    {
        int x_left = clamped_rect.x + t;
        int x_right = clamped_rect.x + clamped_rect.width - 1 - t;

        if (x_left >= 0 && x_left < width)
        {
            for (int y = clamped_rect.y; y < clamped_rect.y + clamped_rect.height; ++y)
            {
                y_plane[y * width + x_left] = color.y;
                int uv_idx = (y / 2) * width + (x_left / 2) * 2;
                uv_plane[uv_idx] = color.v;
                uv_plane[uv_idx + 1] = color.u;
            }
        }
        if (x_right >= 0 && x_right < width && t > 0)
        { // Avoid double-drawing for 1px width
            for (int y = clamped_rect.y; y < clamped_rect.y + clamped_rect.height; ++y)
            {
                y_plane[y * width + x_right] = color.y;
                int uv_idx = (y / 2) * width + (x_right / 2) * 2;
                uv_plane[uv_idx] = color.v;
                uv_plane[uv_idx + 1] = color.u;
            }
        }
    }
}

inline void draw_marker(cv::Mat &input_img, int marker_x, int marker_y, int marker_sz, int thickness, cv::Scalar color)
{
    cv::line(input_img, cv::Point(marker_x - marker_sz, marker_y), cv::Point(marker_x + marker_sz, marker_y), color, thickness);
    cv::line(input_img, cv::Point(marker_x, marker_y - marker_sz), cv::Point(marker_x, marker_y + marker_sz), color, thickness);
}

