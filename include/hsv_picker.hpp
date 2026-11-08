#pragma once

#include <opencv2/core.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hsvpicker {

enum class ColorSpace { Hsv, Lab };
const char* colorSpaceName(ColorSpace space);
const char* rangeLabel(ColorSpace space);
int conversionCode(ColorSpace space);

enum class KeyAction { None, SwitchColorSpace, Screenshot, Pause, Clear, Print, Quit };
// Decode full waitKeyEx codes, including GTK modifier bits and Linux Shift keysyms.
KeyAction decodeKey(int key);

struct Options {
    std::string source = "/dev/video0";
    std::string screenshotDir = "./screenshots";
    bool help = false;
};

Options parseOptions(const std::vector<std::string>& arguments);
bool isCameraSource(const std::string& source);

struct ColorStatistics {
    ColorSpace colorSpace = ColorSpace::Hsv;
    // Alternating inclusive min/max values, in H/S/V or L/a/b channel order.
    std::array<int, 6> range{};
    std::array<std::vector<std::uint64_t>, 3> histograms;
    std::uint64_t pixels = 0;
};

using HsvStatistics = ColorStatistics; // Preserve existing source-level callers.
ColorStatistics analyze(const cv::Mat& bgr, const cv::Rect& roi,
                        ColorSpace colorSpace = ColorSpace::Hsv);
std::string rangeText(const ColorStatistics& statistics);
std::string scalarText(const ColorStatistics& statistics, bool upper);

// Mouse coordinates refer to the composed HighGUI image, not screen coordinates.
cv::Rect fitImage(cv::Size image, const cv::Rect& viewport);
cv::Rect sourceSelection(cv::Point first, cv::Point last,
                         const cv::Rect& displayedImage, cv::Size sourceSize);

struct HistogramHover {
    int channel;
    int value;
    cv::Point position; // Composed-image coordinates, as supplied by HighGUI.
};

class Picker {
public:
    static constexpr int canvasWidth = 1180;
    static constexpr int canvasHeight = 720;

    explicit Picker(bool camera);
    void setFrame(const cv::Mat& bgr);
    void onMouse(int event, int x, int y, int flags);
    // Takes the full waitKeyEx code. Returns false on Q/Esc.
    bool onKey(int key);
    cv::Mat render() const;
    bool frozen() const { return paused_ || dragging_; }
    bool takeReportRequest();
    std::optional<cv::Mat> takeScreenshotRequest();
    void setNotice(const std::string& message, bool error = false);
    ColorSpace colorSpace() const { return colorSpace_; }
    std::optional<HistogramHover> histogramHover() const;
    const cv::Rect& imageRect() const { return imageRect_; }
    const cv::Rect& selection() const { return selection_; }
    const std::optional<ColorStatistics>& statistics() const { return statistics_; }

private:
    void selectTo(cv::Point point);
    void refreshStatistics();

    bool camera_;
    bool paused_ = false;
    bool dragging_ = false;
    bool reportRequested_ = false;
    ColorSpace colorSpace_ = ColorSpace::Hsv;
    std::optional<cv::Mat> screenshotRequest_;
    std::string notice_;
    bool noticeError_ = false;
    std::chrono::steady_clock::time_point noticeUntil_{};
    cv::Point anchor_;
    std::optional<cv::Point> mousePosition_;
    cv::Mat frame_;
    cv::Rect imageRect_;
    cv::Rect selection_;
    std::optional<ColorStatistics> statistics_;
};

} // namespace hsvpicker
