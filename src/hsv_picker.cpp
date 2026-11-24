#include "hsv_picker.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace hsvpicker {
namespace {
const cv::Scalar background(25, 23, 21);
const cv::Scalar panel(38, 35, 32);
const cv::Scalar ink(240, 237, 231);
const cv::Scalar muted(168, 164, 157);
const cv::Scalar accent(126, 220, 108);
constexpr int margin = 12;
constexpr int panelWidth = 384;
constexpr int preferredPanelHeight = 490;
constexpr int headerHeight = 62;

int footerHeight(int width) {
    return width < 700 ? 72 : 52;
}

cv::Size initialCanvas(cv::Size frame) {
    const int width = frame.width + panelWidth + 3 * margin;
    return {width, headerHeight + std::max(frame.height, preferredPanelHeight) + footerHeight(width)};
}

PickerLayout makeLayout(cv::Size canvas, cv::Size frame) {
    PickerLayout layout{};
    layout.canvas = canvas;
    const int body = canvas.height - headerHeight - footerHeight(canvas.width);
    const int innerWidth = canvas.width - 2 * margin;
    if (innerWidth >= panelWidth + margin + std::min(256, frame.width)) {
        layout.viewport = {margin, headerHeight, innerWidth - panelWidth - margin, body};
        layout.panel = {layout.viewport.br().x + margin, headerHeight, panelWidth, body};
    } else {
        const int available = body - margin;
        // Give the preview its full width when that leaves room for the charts.
        const int previewHeight = frame.empty() ? available / 2 :
                                  fitImage(frame, {0, 0, innerWidth, available}).height;
        const int panelHeight = std::clamp(available - previewHeight, std::min(320, available / 2),
                                           std::min(preferredPanelHeight, available * 2 / 3));
        layout.viewport = {margin, headerHeight, innerWidth, available - panelHeight};
        layout.panel = {margin, layout.viewport.br().y + margin, innerWidth, panelHeight};
    }
    if (!frame.empty()) {
        layout.image = fitImage(frame, layout.viewport);
    }

    const auto& panel = layout.panel;
    layout.panelScale = std::min({1.0, panel.width / 360.0, panel.height / 405.0});
    const double scale = layout.panelScale;
    const int inset = cvRound(12 * scale);
    const int chartTop = panel.y + cvRound(140 * scale);
    const int rowHeight = (panel.br().y - inset - chartTop) / 3;
    for (int i = 0; i < 3; ++i) {
        layout.plots[i] = {panel.x + inset, chartTop + i * rowHeight + cvRound(20 * scale),
                           panel.width - 2 * inset, rowHeight - cvRound(41 * scale)};
    }
    return layout;
}

int histogramBins(ColorSpace space, int channel) {
    return space == ColorSpace::Hsv && channel == 0 ? 180 : 256;
}

int histogramValueAtX(const cv::Rect& plot, int bins, int x) {
    const int offset = std::clamp(x - plot.x, 0, plot.width - 1);
    // Inverse of bar edges floor(bin * width / bins). Using floor(x*bins/width)
    // would report the preceding bin at some boundaries with unequal bar widths.
    return static_cast<int>((static_cast<std::int64_t>(offset + 1) * bins - 1) / plot.width);
}

void text(cv::Mat& canvas, const std::string& value, int x, int y,
          double scale = 0.48, cv::Scalar color = ink) {
    int baseline = 0;
    const auto width = cv::getTextSize(value, cv::FONT_HERSHEY_SIMPLEX, scale, 1, &baseline).width;
    if (width > canvas.cols - x - margin) {
        scale *= static_cast<double>(canvas.cols - x - margin) / width;
    }
    cv::putText(canvas, value, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX,
                scale, color, 1, cv::LINE_AA);
}

void histogram(cv::Mat& canvas, const ColorStatistics* stats, ColorSpace space,
               int channel, const PickerLayout& layout) {
    const auto& plot = layout.plots[channel];
    const double scale = layout.panelScale;
    const int labelY = plot.y - cvRound(6 * scale);
    const int left = plot.x;
    const int width = plot.width;
    const int height = plot.height - 1;
    const int bottom = plot.y + height;
    const char* hsvNames[] = {"H / Hue", "S / Saturation", "V / Value"};
    const char* labNames[] = {"L / Lightness", "a / Green - Red", "b / Blue - Yellow"};
    const auto* names = space == ColorSpace::Hsv ? hsvNames : labNames;
    const int bins = histogramBins(space, channel);
    const cv::Scalar colors[] = {cv::Scalar(110, 185, 230),
                                 cv::Scalar(215, 160, 110),
                                 cv::Scalar(130, 220, 140)};
    text(canvas, names[channel], left, labelY, 0.48 * scale, colors[channel]);
    std::uint64_t peak = 0;
    if (stats) {
        const auto& counts = stats->histograms[channel];
        peak = *std::max_element(counts.begin(), counts.end());
        for (int bin = 0; bin < bins; ++bin) {
            if (counts[bin] == 0) {
                continue;
            }
            const int barHeight = std::max(1, static_cast<int>(std::lround(
                static_cast<double>(counts[bin]) / static_cast<double>(peak) * height)));
            const int x1 = left + static_cast<int>(static_cast<std::int64_t>(bin) * width / bins);
            const int x2 = left + static_cast<int>(static_cast<std::int64_t>(bin + 1) * width / bins) - 1;
            cv::Scalar color = colors[channel];
            if (space == ColorSpace::Hsv && channel == 0) {
                cv::Mat hsv(1, 1, CV_8UC3, cv::Scalar(bin, 220, 240));
                cv::Mat bgr;
                cv::cvtColor(hsv, bgr, cv::COLOR_HSV2BGR);
                const auto pixel = bgr.at<cv::Vec3b>(0, 0);
                color = cv::Scalar(pixel[0], pixel[1], pixel[2]);
            }
            cv::rectangle(canvas, cv::Point(x1, bottom - barHeight),
                          cv::Point(x2, bottom - 1), color, cv::FILLED);
        }
    }
    text(canvas, "peak count: " + std::to_string(peak), left + width / 2, labelY, 0.40 * scale, muted);
    cv::line(canvas, {left, bottom}, {left + width - 1, bottom}, muted);
    for (int tick : {left, left + width / 2, left + width - 1}) {
        const std::string label = std::to_string(histogramValueAtX(plot, bins, tick));
        int baseline = 0;
        const auto size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.40 * scale, 1, &baseline);
        const int x = std::clamp(tick - size.width / 2, left, left + width - size.width);
        text(canvas, label, x, bottom + cvRound(16 * scale), 0.40 * scale, muted);
    }
}

void histogramTooltip(cv::Mat& canvas, const HistogramHover& hover, ColorSpace space,
                      const PickerLayout& layout) {
    const auto& plot = layout.plots[hover.channel];
    cv::line(canvas, {hover.position.x, plot.y},
             {hover.position.x, plot.y + plot.height - 1}, accent);

    const char* hsvNames[] = {"H", "S", "V"};
    const char* labNames[] = {"L", "a", "b"};
    const auto* names = space == ColorSpace::Hsv ? hsvNames : labNames;
    const std::string label = std::string(names[hover.channel]) + " = " + std::to_string(hover.value);
    const int padding = cvRound(5 * layout.panelScale);
    const int gap = cvRound(10 * layout.panelScale);
    const double scale = 0.48 * layout.panelScale;
    int baseline = 0;
    const auto size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, scale, 1, &baseline);
    const int width = size.width + padding * 2;
    const int height = size.height + baseline + padding * 2;
    if (width > plot.width || height > plot.height) {
        return;
    }
    const int margin = std::min({2, (plot.width - width) / 2, (plot.height - height) / 2});
    int x = hover.position.x + gap;
    int y = hover.position.y + gap;
    if (x + width > plot.br().x - margin) {
        x = hover.position.x - gap - width;
    }
    if (y + height > plot.br().y - margin) {
        y = hover.position.y - gap - height;
    }
    x = std::clamp(x, plot.x + margin, plot.br().x - width - margin);
    y = std::clamp(y, plot.y + margin, plot.br().y - height - margin);
    const cv::Rect box(x, y, width, height);
    cv::rectangle(canvas, box, background, cv::FILLED);
    cv::rectangle(canvas, box, accent);
    text(canvas, label, x + padding, y + padding + size.height, scale);
}
} // namespace

const char* colorSpaceName(ColorSpace space) {
    return space == ColorSpace::Hsv ? "HSV" : "Lab";
}

const char* rangeLabel(ColorSpace space) {
    return space == ColorSpace::Hsv ? "[hbegin,hend,sbegin,send,vbegin,vend]" :
                                     "[lbegin,lend,abegin,aend,bbegin,bend]";
}

int conversionCode(ColorSpace space) {
    return space == ColorSpace::Hsv ? cv::COLOR_BGR2HSV : cv::COLOR_BGR2Lab;
}

KeyAction decodeKey(int key) {
    if (key == -1) {
        return KeyAction::None;
    }
    // GTK packs modifier state into the upper 16 bits. Qt on Linux normally
    // uses the same X11 keysyms; also accept the Qt Shift key enum explicitly.
    const unsigned symbol = static_cast<unsigned>(key) & 0xffffU;
    if (key == 0x01000020 || symbol == 0xffe1 || symbol == 0xffe2) {
        return KeyAction::SwitchColorSpace;
    }
    switch (symbol) {
    case 27: case 'q': case 'Q': return KeyAction::Quit;
    case ' ': return KeyAction::Pause;
    case 'r': case 'R': return KeyAction::Clear;
    case 'p': case 'P': return KeyAction::Print;
    case 's': case 'S': return KeyAction::Screenshot;
    default: return KeyAction::None;
    }
}

Options parseOptions(const std::vector<std::string>& arguments) {
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& arg = arguments[i];
        if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--source") {
            if (i + 1 == arguments.size() || arguments[i + 1].empty() ||
                arguments[i + 1].rfind("--", 0) == 0) {
                throw std::invalid_argument("--source requires a device or image path");
            }
            options.source = arguments[++i];
        } else if (arg.rfind("--source=", 0) == 0) {
            options.source = arg.substr(9);
            if (options.source.empty()) {
                throw std::invalid_argument("--source must not be empty");
            }
        } else if (arg == "--screenshot-dir") {
            if (i + 1 == arguments.size() || arguments[i + 1].empty() ||
                arguments[i + 1].rfind("--", 0) == 0) {
                throw std::invalid_argument("--screenshot-dir requires a directory path");
            }
            options.screenshotDir = arguments[++i];
        } else if (arg.rfind("--screenshot-dir=", 0) == 0) {
            options.screenshotDir = arg.substr(17);
            if (options.screenshotDir.empty()) {
                throw std::invalid_argument("--screenshot-dir must not be empty");
            }
        } else {
            throw std::invalid_argument("Unknown argument: " + arg);
        }
    }
    return options;
}

bool isCameraSource(const std::string& source) {
    return source.rfind("/dev/video", 0) == 0 || source.rfind("/dev/v4l/", 0) == 0;
}

ColorStatistics analyze(const cv::Mat& bgr, const cv::Rect& roi, ColorSpace colorSpace) {
    if (bgr.empty() || bgr.type() != CV_8UC3) {
        throw std::invalid_argument("Color analysis requires a nonempty 8-bit BGR image");
    }
    if (roi.empty() || (roi & cv::Rect(0, 0, bgr.cols, bgr.rows)) != roi) {
        throw std::invalid_argument("Selection is empty or outside the image");
    }
    cv::Mat converted;
    cv::cvtColor(bgr(roi), converted, conversionCode(colorSpace));
    ColorStatistics result;
    result.colorSpace = colorSpace;
    const int firstBins = colorSpace == ColorSpace::Hsv ? 180 : 256;
    result.range = {firstBins - 1, 0, 255, 0, 255, 0};
    result.histograms = {std::vector<std::uint64_t>(firstBins),
                         std::vector<std::uint64_t>(256),
                         std::vector<std::uint64_t>(256)};
    result.pixels = static_cast<std::uint64_t>(roi.width) * roi.height;
    for (int y = 0; y < converted.rows; ++y) {
        const auto* row = converted.ptr<cv::Vec3b>(y);
        for (int x = 0; x < converted.cols; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                const int value = row[x][channel];
                ++result.histograms[channel][value];
                result.range[channel * 2] = std::min(result.range[channel * 2], value);
                result.range[channel * 2 + 1] = std::max(result.range[channel * 2 + 1], value);
            }
        }
    }
    return result;
}

std::string rangeText(const ColorStatistics& statistics) {
    std::string result = "[";
    for (std::size_t i = 0; i < statistics.range.size(); ++i) {
        result += (i == 0 ? "" : ", ") + std::to_string(statistics.range[i]);
    }
    return result + "]";
}

std::string scalarText(const ColorStatistics& statistics, bool upper) {
    const auto& r = statistics.range;
    const int offset = upper ? 1 : 0;
    return "cv::Scalar(" + std::to_string(r[offset]) + ", " +
           std::to_string(r[offset + 2]) + ", " + std::to_string(r[offset + 4]) + ")";
}

cv::Rect fitImage(cv::Size image, const cv::Rect& bounds) {
    if (image.empty() || bounds.empty()) {
        throw std::invalid_argument("Image and viewport sizes must be positive");
    }
    const double scale = std::min(static_cast<double>(bounds.width) / image.width,
                                  static_cast<double>(bounds.height) / image.height);
    const int width = std::clamp(static_cast<int>(std::lround(image.width * scale)), 1, bounds.width);
    const int height = std::clamp(static_cast<int>(std::lround(image.height * scale)), 1, bounds.height);
    return {bounds.x + (bounds.width - width) / 2,
            bounds.y + (bounds.height - height) / 2, width, height};
}

cv::Rect sourceSelection(cv::Point first, cv::Point last,
                         const cv::Rect& display, cv::Size source) {
    if (display.empty() || source.empty()) {
        throw std::invalid_argument("Image and display sizes must be positive");
    }
    // Include both mouse endpoints, and all source pixels covered by display pixels.
    // Integer arithmetic avoids floating-point errors at full-image boundaries.
    const auto axis = [](int a, int b, int origin, int extent, int sourceExtent) {
        a = std::clamp(a - origin, 0, extent - 1);
        b = std::clamp(b - origin, 0, extent - 1);
        const int start = static_cast<int>(static_cast<std::int64_t>(std::min(a, b)) * sourceExtent / extent);
        const auto endNumerator = static_cast<std::int64_t>(std::max(a, b) + 1) * sourceExtent;
        const int end = static_cast<int>((endNumerator + extent - 1) / extent);
        return std::pair<int, int>{start, end};
    };
    const auto horizontal = axis(first.x, last.x, display.x, display.width, source.width);
    const auto vertical = axis(first.y, last.y, display.y, display.height, source.height);
    return {horizontal.first, vertical.first, horizontal.second - horizontal.first,
            vertical.second - vertical.first};
}

Picker::Picker(bool camera)
    : camera_(camera), layout_(makeLayout(initialCanvas({1, 1}), {})),
      canvasOnOutput_(cv::Point(), layout_.canvas), outputSize_(layout_.canvas) {}

void Picker::setCanvasSize(cv::Size available) {
    if (available.empty() || available == outputSize_) {
        return;
    }
    // Fit a compact logical layout uniformly when the window is too small
    // for the plots and text, even on backends whose canvas fills the window.
    const cv::Size canvas(std::max(320, available.width), std::max(360, available.height));
    layout_ = makeLayout(canvas, frame_.size());
    outputSize_ = available;
    canvasOnOutput_ = fitImage(canvas, cv::Rect(cv::Point(), available));
    mousePosition_.reset();
}

cv::Rect Picker::imageRect() const {
    if (outputSize_ == layout_.canvas) {
        return layout_.image;
    }
    const double scaleX = static_cast<double>(canvasOnOutput_.width) / layout_.canvas.width;
    const double scaleY = static_cast<double>(canvasOnOutput_.height) / layout_.canvas.height;
    const auto project = [&](cv::Point p) {
        return canvasOnOutput_.tl() + cv::Point(cvRound(p.x * scaleX), cvRound(p.y * scaleY));
    };
    return {project(layout_.image.tl()), project(layout_.image.br())};
}

void Picker::setFrame(const cv::Mat& bgr) {
    if (bgr.empty() || bgr.type() != CV_8UC3) {
        throw std::invalid_argument("Source must provide a nonempty 8-bit BGR image");
    }
    if (bgr.size() != frame_.size()) {
        if (frame_.empty()) {
            setCanvasSize(initialCanvas(bgr.size()));
        }
        layout_ = makeLayout(layout_.canvas, bgr.size());
        selection_ = {};
        statistics_.reset();
        dragging_ = false;
    }
    bgr.copyTo(frame_);
    refreshStatistics();
}

void Picker::refreshStatistics() {
    if (selection_.empty()) {
        statistics_.reset();
    } else {
        statistics_ = analyze(frame_, selection_, colorSpace_);
    }
}

void Picker::selectTo(cv::Point point) {
    const auto image = imageRect();
    if (!image.empty()) {
        selection_ = anchorPixels_ | sourceSelection(point, point, image, frame_.size());
        refreshStatistics();
    }
}

void Picker::onMouse(int event, int x, int y, int /*flags*/) {
    const cv::Point point(x, y);
    mousePosition_ = point;
    if (outputSize_ != layout_.canvas) {
        mousePosition_.reset();
        if (canvasOnOutput_.contains(point)) {
            const auto offset = point - canvasOnOutput_.tl();
            mousePosition_ = cv::Point(
                static_cast<int>((offset.x + 0.5) * layout_.canvas.width / canvasOnOutput_.width),
                static_cast<int>((offset.y + 0.5) * layout_.canvas.height / canvasOnOutput_.height));
        }
    }
    if (frame_.empty()) {
        return;
    }
    const auto image = imageRect();
    if (event == cv::EVENT_LBUTTONDOWN && image.contains(point)) {
        anchorPixels_ = sourceSelection(point, point, image, frame_.size());
        selection_ = anchorPixels_;
        dragging_ = true;
        refreshStatistics();
    } else if (dragging_ && event == cv::EVENT_LBUTTONUP) {
        selectTo(point);
        dragging_ = false;
        reportRequested_ = true;
    } else if (dragging_ && event == cv::EVENT_MOUSEMOVE) {
        // Keep dragging until an explicit button-up or R cancellation.
        // Motion-event button flags are not a reliable release notification.
        selectTo(point);
    }
}

std::optional<HistogramHover> Picker::histogramHover() const {
    if (dragging_ || !mousePosition_) {
        return std::nullopt;
    }
    for (int channel = 0; channel < 3; ++channel) {
        const auto& plot = layout_.plots[channel];
        if (plot.contains(*mousePosition_)) {
            return HistogramHover{channel, histogramValueAtX(plot, histogramBins(colorSpace_, channel),
                                                           mousePosition_->x), *mousePosition_};
        }
    }
    return std::nullopt;
}

bool Picker::onKey(int key) {
    const auto action = decodeKey(key);
    if (action == KeyAction::Quit) {
        return false;
    }
    if (action == KeyAction::SwitchColorSpace) {
        colorSpace_ = colorSpace_ == ColorSpace::Hsv ? ColorSpace::Lab : ColorSpace::Hsv;
        refreshStatistics();
    } else if (action == KeyAction::Screenshot && !frame_.empty()) {
        screenshotRequest_ = frame_.clone();
    } else if (action == KeyAction::Pause && camera_ && !dragging_) {
        paused_ = !paused_;
    } else if (action == KeyAction::Clear) {
        dragging_ = false;
        paused_ = false;
        selection_ = {};
        statistics_.reset();
        reportRequested_ = false;
    } else if (action == KeyAction::Print && statistics_) {
        reportRequested_ = true;
    }
    return true;
}

bool Picker::takeReportRequest() {
    const bool result = reportRequested_;
    reportRequested_ = false;
    return result;
}

std::optional<cv::Mat> Picker::takeScreenshotRequest() {
    auto result = std::move(screenshotRequest_);
    screenshotRequest_.reset();
    return result;
}

void Picker::setNotice(const std::string& message, bool error) {
    notice_ = message;
    noticeError_ = error;
    noticeUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
}

cv::Mat Picker::render() const {
    cv::Mat canvas(layout_.canvas, CV_8UC3, background);
    text(canvas, "color-picker", margin, 28, 0.78, accent);
    const std::string status = camera_ ? (frozen() ? "CAMERA / FROZEN" : "CAMERA / LIVE") : "IMAGE";
    text(canvas, std::string(colorSpaceName(colorSpace_)) + "  |  " + status, 168, 27, 0.46, accent);
    if (std::chrono::steady_clock::now() < noticeUntil_) {
        text(canvas, notice_, margin, 51, 0.46, noticeError_ ? cv::Scalar(100, 100, 255) : accent);
    } else {
        text(canvas, "Drag to select. Values use original 8-bit pixels.", margin, 51, 0.46, muted);
    }
    cv::rectangle(canvas, layout_.viewport, panel, cv::FILLED);
    cv::rectangle(canvas, layout_.panel, panel, cv::FILLED);

    const auto& image = layout_.image;
    if (!frame_.empty()) {
        auto preview = canvas(image);
        if (image.size() == frame_.size()) {
            frame_.copyTo(preview);
        } else {
            cv::resize(frame_, preview, image.size(), 0, 0,
                       image.width < frame_.cols ? cv::INTER_AREA : cv::INTER_NEAREST);
        }
        if (!selection_.empty()) {
            const int x1 = image.x + static_cast<int>(
                static_cast<std::int64_t>(selection_.x) * image.width / frame_.cols);
            const int y1 = image.y + static_cast<int>(
                static_cast<std::int64_t>(selection_.y) * image.height / frame_.rows);
            const int x2 = image.x + static_cast<int>((
                static_cast<std::int64_t>(selection_.x + selection_.width) * image.width
                + frame_.cols - 1) / frame_.cols) - 1;
            const int y2 = image.y + static_cast<int>((
                static_cast<std::int64_t>(selection_.y + selection_.height) * image.height
                + frame_.rows - 1) / frame_.rows) - 1;
            cv::rectangle(canvas, {x1, y1}, {x2, y2}, cv::Scalar(0, 0, 0), 3);
            cv::rectangle(canvas, {x1, y1}, {x2, y2}, accent, 1);
        }
    }

    const double scale = layout_.panelScale;
    const auto panelText = [&](const std::string& label, int y, double fontSize, cv::Scalar color) {
        text(canvas, label, layout_.plots[0].x, layout_.panel.y + cvRound(y * scale),
             fontSize * scale, color);
    };
    const auto* stats = statistics_ ? &*statistics_ : nullptr;
    panelText(rangeLabel(colorSpace_), 20, 0.43, muted);
    panelText(stats ? rangeText(*stats) : "Select an image region", 46, 0.64, accent);
    panelText(colorSpace_ == ColorSpace::Hsv ? "Inclusive min / max. H: 0..179, S/V: 0..255" :
              "Inclusive min / max. L/a/b: 0..255 (8-bit)", 66, 0.40, muted);
    panelText(stats ? "lower = " + scalarText(*stats, false) : "lower = --", 90, 0.46, ink);
    panelText(stats ? "upper = " + scalarText(*stats, true) : "upper = --", 111, 0.46, ink);
    panelText("Histograms: x = value (hover), y = pixel count", 134, 0.41, muted);
    for (int channel = 0; channel < 3; ++channel) {
        histogram(canvas, stats, colorSpace_, channel, layout_);
    }
    if (const auto hover = histogramHover()) {
        histogramTooltip(canvas, *hover, colorSpace_, layout_);
    }

    const int footer = layout_.panel.br().y;
    if (stats) {
        text(canvas, "ROI: x=" + std::to_string(selection_.x) + " y=" + std::to_string(selection_.y) +
             " w=" + std::to_string(selection_.width) + " h=" + std::to_string(selection_.height) +
             " | pixels=" + std::to_string(stats->pixels), margin, footer + 20, 0.45, muted);
    } else {
        text(canvas, "No selection", margin, footer + 20, 0.45, muted);
    }
    const std::string firstKeys = "Shift: HSV/Lab   S: screenshot   Space: freeze/resume";
    const std::string lastKeys = "R: clear/resume   P: print   Q/Esc: quit";
    if (canvas.cols < 700) {
        text(canvas, firstKeys, margin, footer + 40, 0.44, muted);
        text(canvas, lastKeys, margin, footer + 60, 0.44, muted);
    } else {
        text(canvas, firstKeys + "   " + lastKeys, margin, footer + 41, 0.44, muted);
    }
    if (canvas.size() == outputSize_) {
        return canvas;
    }
    cv::Mat output(outputSize_, canvas.type(), background);
    cv::resize(canvas, output(canvasOnOutput_), canvasOnOutput_.size(), 0, 0, cv::INTER_AREA);
    return output;
}

} // namespace hsvpicker
