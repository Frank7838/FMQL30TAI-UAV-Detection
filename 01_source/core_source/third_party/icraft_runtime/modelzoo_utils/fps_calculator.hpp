#pragma once

#include <chrono>
#include <atomic>

class FPSCalculator {
public:
    FPSCalculator()
        : frame_count_(0),
        fps_(0.0f),
        start_time_(std::chrono::steady_clock::now())
    {  }

    // Call this for every frame processed.
    void tick() {
        frame_count_++;
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();

        // Update FPS calculation every second for stability.
        if (duration >= 1000) {
            fps_ = static_cast<float>(frame_count_) * 1000.0f / duration;
            start_time_ = now;
            frame_count_ = 0;
        }
    }

    // Get the most recently calculated FPS value.
    float getFPS() const {
        return fps_;
    }

private:
    std::atomic<unsigned int> frame_count_;
    std::atomic<float> fps_;
    std::chrono::steady_clock::time_point start_time_;
};