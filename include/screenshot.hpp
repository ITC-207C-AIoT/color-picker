#pragma once

#include <opencv2/core.hpp>

#include <string>

namespace hsvpicker {

struct ScreenshotResult {
    std::string path;
    std::string error;
    explicit operator bool() const { return !path.empty() && error.empty(); }
};

// Save an unmodified BGR frame as PNG. Failures are returned, not thrown.
ScreenshotResult saveScreenshot(const cv::Mat& bgr, const std::string& directory);

} // namespace hsvpicker
