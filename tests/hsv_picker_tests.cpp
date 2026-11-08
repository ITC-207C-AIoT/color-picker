#include "hsv_picker.hpp"
#include "screenshot.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requiresInvalidArgument(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Expected invalid_argument");
}

void testOptions() {
    require(hsvpicker::parseOptions({}).source == "/dev/video0", "Default camera path");
    require(hsvpicker::parseOptions({"--source", "/dev/video2"}).source == "/dev/video2", "Camera option");
    require(hsvpicker::parseOptions({"--source=some image.png"}).source == "some image.png", "Image option");
    require(hsvpicker::parseOptions({"--help"}).help, "Help option");
    require(hsvpicker::parseOptions({}).screenshotDir == "./screenshots", "Default screenshot directory");
    require(hsvpicker::parseOptions({"--screenshot-dir", "my shots"}).screenshotDir == "my shots",
            "Screenshot directory with spaces");
    require(hsvpicker::parseOptions({"--screenshot-dir=/tmp/shots"}).screenshotDir == "/tmp/shots",
            "Screenshot directory equals syntax");
    requiresInvalidArgument([] { hsvpicker::parseOptions({"--screenshot-dir"}); });
    requiresInvalidArgument([] { hsvpicker::parseOptions({"--screenshot-dir="}); });
    requiresInvalidArgument([] { hsvpicker::parseOptions({"--screenshot-dir", "--help"}); });
    require(hsvpicker::isCameraSource("/dev/video0"), "Video source detection");
    require(hsvpicker::isCameraSource("/dev/v4l/by-id/camera"), "Stable camera symlink detection");
    require(!hsvpicker::isCameraSource("photo.png"), "Image source detection");
    requiresInvalidArgument([] { hsvpicker::parseOptions({"--source"}); });
    requiresInvalidArgument([] { hsvpicker::parseOptions({"--source="}); });
    requiresInvalidArgument([] { hsvpicker::parseOptions({"--source", "--help"}); });
    requiresInvalidArgument([] { hsvpicker::parseOptions({"--unknown"}); });
}

void testStatistics() {
    // Analyze a non-contiguous ROI; the magenta border must not contribute.
    cv::Mat image(4, 5, CV_8UC3, cv::Scalar(255, 0, 255));
    const cv::Rect roi(1, 1, 3, 2);
    image.at<cv::Vec3b>(1, 1) = {0, 0, 255};   // red
    image.at<cv::Vec3b>(1, 2) = {0, 255, 0};   // green
    image.at<cv::Vec3b>(1, 3) = {255, 0, 0};   // blue
    image.at<cv::Vec3b>(2, 1) = {0, 0, 0};     // black
    image.at<cv::Vec3b>(2, 2) = {255, 255, 255}; // white
    image.at<cv::Vec3b>(2, 3) = {0, 0, 255};   // red again
    const auto stats = hsvpicker::analyze(image, roi);
    require(stats.range == std::array<int, 6>{0, 120, 0, 255, 0, 255}, "Raw OpenCV HSV extrema");
    require(stats.pixels == 6, "ROI pixel count");
    require(stats.histograms[0].size() == 180 && stats.histograms[1].size() == 256 &&
            stats.histograms[2].size() == 256, "Histogram bin counts");
    require(stats.histograms[0][0] == 4 && stats.histograms[0][60] == 1 &&
            stats.histograms[0][120] == 1 && stats.histograms[0][150] == 0, "Hue frequencies and ROI exclusion");
    require(stats.histograms[1][0] == 2 && stats.histograms[1][255] == 4, "Saturation frequencies");
    require(stats.histograms[2][0] == 1 && stats.histograms[2][255] == 5, "Value frequencies");
    for (const auto& bins : stats.histograms) {
        require(std::accumulate(bins.begin(), bins.end(), std::uint64_t{0}) == stats.pixels,
                "Every ROI pixel counted once in every channel");
    }
    require(hsvpicker::rangeText(stats) == "[0, 120, 0, 255, 0, 255]", "Six-value order");
    require(hsvpicker::scalarText(stats, false) == "cv::Scalar(0, 0, 0)", "Lower scalar order");
    require(hsvpicker::scalarText(stats, true) == "cv::Scalar(120, 255, 255)", "Upper scalar order");
    cv::Mat hsv, mask;
    cv::cvtColor(image(roi), hsv, cv::COLOR_BGR2HSV);
    const auto& r = stats.range;
    cv::inRange(hsv, cv::Scalar(r[0], r[2], r[4]), cv::Scalar(r[1], r[3], r[5]), mask);
    require(cv::countNonZero(mask) == 6, "Bounds are directly usable with inRange");

    const auto green = hsvpicker::analyze(image, cv::Rect(2, 1, 1, 1));
    require(green.range == std::array<int, 6>{60, 60, 255, 255, 255, 255}, "Single pixel has equal endpoints");
    cv::Mat reds(1, 2, CV_8UC3);
    reds.at<cv::Vec3b>(0, 0) = {0, 0, 255};
    reds.at<cv::Vec3b>(0, 1) = {8, 0, 255};
    const auto wrap = hsvpicker::analyze(reds, cv::Rect(0, 0, 2, 1));
    require(wrap.range == std::array<int, 6>{0, 179, 255, 255, 255, 255}, "Hue wrap preserves literal extrema");
    require(wrap.histograms[0][179] == 1, "Final hue bin included");
    requiresInvalidArgument([&] { hsvpicker::analyze(image, cv::Rect()); });
    requiresInvalidArgument([&] { hsvpicker::analyze(image, cv::Rect(-1, 0, 2, 2)); });
    requiresInvalidArgument([&] { hsvpicker::analyze(cv::Mat(), roi); });
    requiresInvalidArgument([&] { hsvpicker::analyze(cv::Mat(2, 2, CV_32FC3), cv::Rect(0, 0, 1, 1)); });
}

void testCoordinates() {
    const cv::Rect viewport(16, 88, 720, 570);
    const cv::Size original(4000, 2000);
    const auto display = hsvpicker::fitImage(original, viewport);
    require(display == cv::Rect(16, 193, 720, 360), "Letterbox preserves image aspect ratio");
    const auto bottomRight = display.br() - cv::Point(1, 1);
    require(hsvpicker::sourceSelection(display.tl(), bottomRight, display, original) ==
            cv::Rect(0, 0, 4000, 2000), "Whole image retains last row and column");
    require(hsvpicker::sourceSelection(bottomRight, display.tl(), display, original) ==
            cv::Rect(0, 0, 4000, 2000), "Reverse drag works");
    require(hsvpicker::sourceSelection(display.tl(), cv::Point(9999, 9999), display, original) ==
            cv::Rect(0, 0, 4000, 2000), "Out-of-image drag is clamped");
    require(hsvpicker::sourceSelection(display.tl(), display.tl(), display, original) ==
            cv::Rect(0, 0, 6, 6), "A downscaled display pixel covers original pixels");
    const auto portrait = hsvpicker::fitImage(cv::Size(100, 1000), viewport);
    require(portrait.width == 57 && portrait.height == 570 && portrait.x > viewport.x,
            "Portrait image is centered with horizontal padding");
    const auto enlarged = hsvpicker::fitImage(cv::Size(2, 1), viewport);
    const auto sample = hsvpicker::sourceSelection(enlarged.tl(), enlarged.tl(), enlarged, cv::Size(2, 1));
    require(sample == cv::Rect(0, 0, 1, 1), "Upscaled pixel maps to a valid original pixel");
    requiresInvalidArgument([&] { hsvpicker::fitImage(cv::Size(), viewport); });
}

bool sameImage(const cv::Mat& a, const cv::Mat& b) {
    return !a.empty() && !b.empty() && a.size() == b.size() && a.type() == b.type() &&
           cv::norm(a, b, cv::NORM_INF) == 0;
}

void testHistogramHover() {
    using hsvpicker::ColorSpace;
    hsvpicker::Picker picker(true);
    const cv::Mat image(120, 200, CV_8UC3, cv::Scalar(0, 255, 0));
    picker.setFrame(image);
    require(!picker.histogramHover(), "No hover before receiving a mouse position");
    for (const auto space : {ColorSpace::Hsv, ColorSpace::Lab}) {
        if (picker.colorSpace() != space) picker.onKey(0xffe1);
        for (int channel = 0; channel < 3; ++channel) {
            const cv::Rect plot(766, 285 + channel * 127, 390, 75);
            const int bins = space == ColorSpace::Hsv && channel == 0 ? 180 : 256;
            for (int value = 0; value < bins; ++value) {
                const int first = plot.x + value * plot.width / bins;
                const int last = plot.x + (value + 1) * plot.width / bins - 1;
                for (const auto point : {cv::Point(first, plot.y), cv::Point(last, plot.br().y - 1)}) {
                    picker.onMouse(cv::EVENT_MOUSEMOVE, point.x, point.y, 0);
                    const auto hover = picker.histogramHover();
                    require(hover && hover->channel == channel && hover->value == value &&
                            hover->position == point, "Hover matches each rendered bar including its edge pixels");
                }
            }
            for (const auto point : {plot.tl(), plot.br() - cv::Point(1, 1),
                                    cv::Point(plot.x, plot.br().y - 1), cv::Point(plot.br().x - 1, plot.y)}) {
                picker.onMouse(cv::EVENT_MOUSEMOVE, 0, 0, 0);
                const auto without = picker.render();
                picker.onMouse(cv::EVENT_MOUSEMOVE, point.x, point.y, 0);
                const auto with = picker.render();
                cv::Mat difference;
                cv::absdiff(without, with, difference);
                cv::Mat outside(with.size(), CV_8UC1, cv::Scalar(255));
                outside(plot).setTo(0);
                require(cv::norm(difference, cv::NORM_INF) > 0 &&
                        cv::norm(difference, cv::NORM_INF, outside) == 0,
                        "Tooltip and guide stay inside this plot at all four corners");
                require(with.at<cv::Vec3b>(plot.y, point.x) != without.at<cv::Vec3b>(plot.y, point.x) &&
                        with.at<cv::Vec3b>(plot.br().y - 1, point.x) !=
                        without.at<cv::Vec3b>(plot.br().y - 1, point.x),
                        "Vertical guide reaches the plot top and baseline");
            }
        }
    }
    require(!picker.statistics() && picker.selection().empty() && !picker.frozen() &&
            !picker.takeReportRequest() && !picker.takeScreenshotRequest(),
            "Hover works without ROI statistics and does not change camera or report state");
    for (const auto point : {cv::Point(765, 300), cv::Point(1156, 300), cv::Point(961, 284),
                            cv::Point(961, 360), cv::Point(-1, -1), cv::Point(2000, 1000)}) {
        picker.onMouse(cv::EVENT_MOUSEMOVE, point.x, point.y, 0);
        require(!picker.histogramHover(), "Moving outside a plot hides its hover");
    }
    picker.onMouse(cv::EVENT_MOUSEMOVE, 961, 300, 0);
    require(picker.histogramHover()->value == 128, "Lab midpoint is encoded value 128");
    picker.onKey(0xffe1);
    require(picker.histogramHover()->channel == 0 && picker.histogramHover()->value == 90,
            "Shift updates hover at a stationary pointer immediately");

    const auto display = picker.imageRect();
    picker.onMouse(cv::EVENT_LBUTTONDOWN, display.x + 10, display.y + 10, 0);
    picker.onMouse(cv::EVENT_MOUSEMOVE, 961, 300, 0);
    require(picker.frozen() && !picker.histogramHover(), "Dragging across a plot does not display a tooltip");
    picker.onMouse(cv::EVENT_LBUTTONUP, 961, 300, 0);
    require(picker.takeReportRequest() && picker.histogramHover() && !picker.frozen(),
            "Release ends dragging normally and re-enables hover");
    const auto selection = picker.selection();
    const auto range = picker.statistics()->range;
    picker.onMouse(cv::EVENT_MOUSEMOVE, 1155, 613, 0);
    require(picker.selection() == selection && picker.statistics()->range == range &&
            !picker.takeReportRequest() && !picker.frozen(), "Hover preserves a completed ROI and its statistics");
    picker.onKey('s');
    const auto screenshot = picker.takeScreenshotRequest();
    require(screenshot && sameImage(*screenshot, image), "Hover overlay is not included in original-frame screenshots");
    picker.setFrame(cv::Mat(image.size(), CV_8UC3, cv::Scalar(0, 0, 255)));
    require(picker.histogramHover()->value == 255 && picker.statistics()->range[0] == 0 && !picker.frozen(),
            "Camera frames and selected ROI continue updating while hovering");
    picker.onKey(' ');
    picker.onMouse(cv::EVENT_MOUSEMOVE, 961, 300, 0);
    require(picker.frozen() && picker.histogramHover(), "Hover preserves a manual camera pause");
}

void testLabStatistics() {
    using hsvpicker::ColorSpace;
    cv::Mat image(4, 5, CV_8UC3, cv::Scalar(0, 255, 0));
    const cv::Rect roi(1, 1, 2, 2);
    image(roi).setTo(cv::Scalar(0, 0, 0));
    image.at<cv::Vec3b>(1, 2) = {255, 255, 255};
    image.at<cv::Vec3b>(2, 1) = {255, 255, 255};
    const auto stats = hsvpicker::analyze(image, roi, ColorSpace::Lab);
    require(stats.colorSpace == ColorSpace::Lab && stats.pixels == 4, "Lab mode and pixel count");
    require(stats.range == std::array<int, 6>{0, 255, 128, 128, 128, 128},
            "8-bit Lab encodes black, white, and neutral chroma directly");
    for (const auto& bins : stats.histograms) {
        require(bins.size() == 256 && std::accumulate(bins.begin(), bins.end(), std::uint64_t{0}) == 4,
                "Every Lab histogram has 256 bins and counts every ROI pixel");
    }
    require(stats.histograms[0][0] == 2 && stats.histograms[0][255] == 2 &&
            stats.histograms[1][128] == 4 && stats.histograms[2][128] == 4,
            "Lab frequencies include endpoint bins and exclude the green border");
    require(hsvpicker::scalarText(stats, false) == "cv::Scalar(0, 128, 128)" &&
            hsvpicker::scalarText(stats, true) == "cv::Scalar(255, 128, 128)",
            "Lab output preserves L/a/b channel order");
    require(std::string(hsvpicker::rangeLabel(ColorSpace::Lab)) ==
            "[lbegin,lend,abegin,aend,bbegin,bend]", "Lab range labels");
    cv::Mat lab, mask;
    cv::cvtColor(image(roi), lab, cv::COLOR_BGR2Lab);
    const auto& r = stats.range;
    cv::inRange(lab, cv::Scalar(r[0], r[2], r[4]), cv::Scalar(r[1], r[3], r[5]), mask);
    require(cv::countNonZero(mask) == 4, "Lab bounds are directly usable by inRange");

    const auto red = hsvpicker::analyze(cv::Mat(1, 1, CV_8UC3, cv::Scalar(0, 0, 255)),
                                      cv::Rect(0, 0, 1, 1), ColorSpace::Lab);
    const std::array<int, 3> reference{136, 208, 195};
    for (int channel = 0; channel < 3; ++channel) {
        require(std::abs(red.range[channel * 2] - reference[channel]) <= 1 &&
                red.range[channel * 2] == red.range[channel * 2 + 1],
                "Known red has encoded Lab values, with conversion rounding tolerance");
    }
}

void testFullKeyCodes() {
    using hsvpicker::KeyAction;
    for (const int key : {0xffe1, 0xffe2, 0x0001ffe1, 0x0010ffe2, 0x01000020}) {
        require(hsvpicker::decodeKey(key) == KeyAction::SwitchColorSpace, "Left/right Shift key codes");
    }
    for (const int key : std::array<int, 3>{'s', 'S', 0x01010073}) {
        require(hsvpicker::decodeKey(key) == KeyAction::Screenshot, "S and GTK modifier state");
    }
    for (const int key : {-1, 0, 0xff50, 0xff53, 0xffe3}) {
        require(hsvpicker::decodeKey(key) == KeyAction::None,
                "Special keys are not confused with ASCII by low-eight-bit truncation");
    }
    require(hsvpicker::decodeKey(0x00100070) == KeyAction::Print &&
            hsvpicker::decodeKey(0x00010052) == KeyAction::Clear &&
            hsvpicker::decodeKey(' ') == KeyAction::Pause &&
            hsvpicker::decodeKey(27) == KeyAction::Quit, "Existing keyboard shortcuts");
}

void testSwitchingAndScreenshots() {
    using hsvpicker::ColorSpace;
    for (const bool camera : {false, true}) {
        hsvpicker::Picker picker(camera);
        picker.onKey('s');
        require(!picker.takeScreenshotRequest(), "No screenshot request before the first valid frame");
        picker.onKey(0xffe1);
        require(picker.colorSpace() == ColorSpace::Lab && !picker.statistics() &&
                picker.selection().empty() && !picker.frozen(), "Switch mode without a selection");
        cv::Mat image(120, 200, CV_8UC3, cv::Scalar(0, 255, 0));
        image(cv::Rect(100, 0, 100, 120)).setTo(cv::Scalar(255, 0, 0));
        picker.setFrame(image);
        picker.onKey('S');
        const auto liveScreenshot = picker.takeScreenshotRequest();
        require(liveScreenshot && sameImage(*liveScreenshot, image) &&
                !picker.frozen() && !picker.takeScreenshotRequest(), "Live screenshot is full-frame and consumed once");
        const auto display = picker.imageRect();
        picker.onMouse(cv::EVENT_LBUTTONDOWN, display.x + 10, display.y + 10, 0);
        picker.onMouse(cv::EVENT_MOUSEMOVE, display.x + 80, display.y + 80, 0);
        const auto selection = picker.selection();
        picker.onKey(0xffe2);
        require(picker.colorSpace() == ColorSpace::Hsv && picker.frozen() &&
                picker.selection() == selection && picker.statistics()->range[0] == 60,
                "Switch during dragging recomputes the same frozen ROI without ending the drag");
        picker.onKey(0xffe1);
        require(picker.statistics()->colorSpace == ColorSpace::Lab && picker.frozen() &&
                picker.selection() == selection, "Switch back to Lab during dragging");
        picker.onKey('s');
        picker.onMouse(cv::EVENT_LBUTTONUP, display.x + 80, display.y + 80, 0);
        require(!picker.frozen() && picker.takeReportRequest(), "Release resumes capture after switching");
        picker.setFrame(cv::Mat(image.size(), CV_8UC3, cv::Scalar(0, 0, 255)));
        const auto dragScreenshot = picker.takeScreenshotRequest();
        require(dragScreenshot && sameImage(*dragScreenshot, image),
                "Screenshot owns the selected displayed frame even after capture advances");
        require(!picker.render().empty(), "Lab selected histograms render");
        if (camera) {
            picker.onKey(' ');
            picker.onKey(0xffe2);
            picker.onKey('S');
            require(picker.frozen() && picker.selection() == selection && picker.takeScreenshotRequest(),
                    "Switching and screenshot preserve a manual camera pause");
        }
        const auto beforeRepeatedSwitches = picker.colorSpace();
        for (int i = 0; i < 10; ++i) {
            picker.onKey(i % 2 == 0 ? 0xffe1 : 0xffe2);
        }
        require(picker.colorSpace() == beforeRepeatedSwitches && picker.selection() == selection,
                "Repeated left/right Shift toggles retain ROI");
        picker.onKey('r');
        require(!picker.frozen() && !picker.statistics() && !picker.render().empty(),
                "R clears/resumes correctly in either color space");
        picker.setNotice("Screenshot saved");
        require(!picker.render().empty(), "Screenshot success feedback renders");
        picker.setNotice("Screenshot failed", true);
        require(!picker.render().empty(), "Screenshot failure feedback renders");
    }
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        char pattern[] = "/tmp/hsv-picker-tests-XXXXXX";
        const auto* created = ::mkdtemp(pattern);
        require(created != nullptr, "Create screenshot test directory");
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void testScreenshotFiles() {
    TemporaryDirectory temporary;
    const auto directory = temporary.path / "nested" / "test images";
    cv::Mat image(30, 50, CV_8UC3);
    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            image.at<cv::Vec3b>(y, x) = cv::Vec3b(x * 4, y * 7, x + y);
        }
    }
    const auto first = hsvpicker::saveScreenshot(image, directory.string());
    require(static_cast<bool>(first), "Create directories and save the original frame as PNG: " + first.error);
    require(std::filesystem::path(first.path).is_absolute() &&
            std::filesystem::path(first.path).extension() == ".png", "Screenshot path and format");
    require(sameImage(cv::imread(first.path), image), "PNG round trip preserves every pixel and original resolution");
    const auto second = hsvpicker::saveScreenshot(image, directory.string());
    const auto third = hsvpicker::saveScreenshot(image(cv::Rect(1, 2, 20, 10)), directory.string());
    require(second && third && first.path != second.path && second.path != third.path,
            "Consecutive saves use different filenames");
    require(sameImage(cv::imread(first.path), image), "Subsequent saves do not overwrite an existing screenshot");
    require(sameImage(cv::imread(third.path), image(cv::Rect(1, 2, 20, 10))),
            "PNG encoder handles non-contiguous BGR input");
    require(!hsvpicker::saveScreenshot(cv::Mat(), directory.string()) &&
            !hsvpicker::saveScreenshot(cv::Mat(1, 1, CV_32FC3), directory.string()) &&
            !hsvpicker::saveScreenshot(image, ""), "Invalid screenshots return failure without throwing");
    const auto blocker = temporary.path / "regular-file";
    std::ofstream(blocker) << "keep this file";
    const auto failure = hsvpicker::saveScreenshot(image, (blocker / "subdirectory").string());
    require(!failure && !failure.error.empty(), "Invalid output directory reports a nonfatal filesystem error");
    require(static_cast<bool>(hsvpicker::saveScreenshot(image, directory.string())),
            "A successful save is still possible after an earlier failure");
}

void testPicker(const std::string& previewPath) {
    cv::Mat image(120, 200, CV_8UC3, cv::Scalar(0, 255, 0));
    image(cv::Rect(100, 0, 100, 120)).setTo(cv::Scalar(255, 0, 0));
    hsvpicker::Picker picker(true);
    picker.setFrame(image);
    require(!picker.frozen() && !picker.statistics(), "Camera starts live with no ROI");
    picker.onMouse(cv::EVENT_LBUTTONDOWN, 0, 0, cv::EVENT_FLAG_LBUTTON);
    require(!picker.frozen() && picker.selection().empty(), "Clicks outside image are ignored");
    const auto display = picker.imageRect();
    const auto first = display.tl() + cv::Point(10, 10);
    const auto last = display.tl() + cv::Point(80, 80);
    picker.onMouse(cv::EVENT_LBUTTONDOWN, first.x, first.y, cv::EVENT_FLAG_LBUTTON);
    require(picker.frozen() && picker.statistics(), "Drag freezes camera and starts analysis");
    // A backend may send motion events without the left-button flag while held.
    // Only an explicit release or cancellation should finish the selection.
    int previousWidth = picker.selection().width;
    for (int step = 1; step <= 16; ++step) {
        const int flags = step % 3 == 0 ? cv::EVENT_FLAG_SHIFTKEY :
                          step % 3 == 1 ? 0 : cv::EVENT_FLAG_CTRLKEY;
        picker.onMouse(cv::EVENT_MOUSEMOVE, first.x + step * 4, first.y + step * 4, flags);
        require(picker.frozen() && !picker.takeReportRequest(),
                "Motion without a left-button flag must not end dragging");
        require(picker.selection().width > previousWidth,
                "Continuous motion keeps growing the selection");
        previousWidth = picker.selection().width;
    }
    picker.onMouse(cv::EVENT_MOUSEMOVE, last.x, last.y, cv::EVENT_FLAG_LBUTTON);
    const auto duringDrag = picker.selection();
    require(duringDrag.width > 1 && duringDrag.height > 1, "Stats update while dragging");
    picker.onKey(' ');
    require(picker.frozen(), "Space cannot resume during a drag");
    picker.onMouse(cv::EVENT_LBUTTONUP, last.x, last.y, 0);
    require(!picker.frozen(), "Camera automatically resumes after mouse release");
    require(picker.statistics()->range == std::array<int, 6>{60, 60, 255, 255, 255, 255},
            "ROI stats sample original image");
    require(picker.takeReportRequest() && !picker.takeReportRequest(), "Release prints once");
    picker.onMouse(cv::EVENT_MOUSEMOVE, last.x + 10, last.y + 10, cv::EVENT_FLAG_LBUTTON);
    require(picker.selection() == duringDrag && !picker.frozen() && !picker.takeReportRequest(),
            "Motion after release cannot reopen or change the completed selection");
    picker.onKey('p');
    require(picker.takeReportRequest(), "P requests bounds output");
    cv::Mat nextFrame(image.size(), CV_8UC3, cv::Scalar(0, 0, 255));
    if (!picker.frozen()) {
        picker.setFrame(nextFrame);
    }
    require(picker.selection() == duringDrag && picker.statistics()->range[0] == 0,
            "Live selected ROI updates with new camera frames");
    picker.onKey(' ');
    require(picker.frozen(), "Space manually freezes camera");
    picker.onKey(' ');
    require(!picker.frozen(), "Space resumes a manually paused camera");
    picker.onKey(' ');
    picker.onMouse(cv::EVENT_LBUTTONDOWN, first.x, first.y, cv::EVENT_FLAG_LBUTTON);
    picker.onMouse(cv::EVENT_LBUTTONUP, last.x, last.y, 0);
    require(picker.frozen(), "A selection preserves a deliberate manual pause");
    picker.onKey('r');
    require(!picker.statistics() && picker.selection().empty() && !picker.frozen() &&
            !picker.takeReportRequest(), "R clears ROI, pending output, and pause state");
    if (!picker.frozen()) {
        picker.setFrame(image);
    }
    const auto resumedPixel = picker.render().at<cv::Vec3b>(display.y + display.height / 2, display.x + 10);
    require(resumedPixel == cv::Vec3b(0, 255, 0), "Next camera frame is displayed after cancellation");

    picker.onMouse(cv::EVENT_LBUTTONDOWN, first.x, first.y, cv::EVENT_FLAG_LBUTTON);
    require(picker.frozen(), "New drag temporarily freezes camera");
    picker.onKey('R');
    picker.onMouse(cv::EVENT_LBUTTONUP, last.x, last.y, 0);
    require(!picker.frozen() && picker.selection().empty() && !picker.takeReportRequest(),
            "Cancel during dragging resumes camera and ignores the later mouse release");
    const auto end = display.br() - cv::Point(1, 1);
    picker.onMouse(cv::EVENT_LBUTTONDOWN, end.x, end.y, cv::EVENT_FLAG_LBUTTON);
    picker.onMouse(cv::EVENT_MOUSEMOVE, -100, -100, 0);
    require(picker.selection() == cv::Rect(0, 0, 200, 120) && !picker.takeReportRequest() && picker.frozen(),
            "Out-of-image motion is clamped and continues the active drag");
    picker.onMouse(cv::EVENT_LBUTTONUP, -100, -100, 0);
    require(picker.takeReportRequest() && !picker.frozen(),
            "Explicit release ends an out-of-image drag and resumes capture");
    require(!picker.onKey('q') && !picker.onKey(27), "Q and Escape exit");
    picker.setFrame(cv::Mat(20, 30, CV_8UC3, cv::Scalar(0, 0, 0)));
    require(!picker.statistics(), "Resolution change clears old selection");

    hsvpicker::Picker still(false);
    still.setFrame(image);
    const auto stillRect = still.imageRect();
    still.onMouse(cv::EVENT_LBUTTONDOWN, stillRect.x, stillRect.y, cv::EVENT_FLAG_LBUTTON);
    still.onMouse(cv::EVENT_LBUTTONUP, stillRect.br().x - 1, stillRect.br().y - 1, 0);
    require(still.statistics()->pixels == 24000 && !still.frozen(), "Image selection works");
    still.onKey(' ');
    require(!still.frozen(), "Space on an image is harmless");
    const auto canvas = still.render();
    require(canvas.size() == cv::Size(hsvpicker::Picker::canvasWidth, hsvpicker::Picker::canvasHeight) &&
            canvas.type() == CV_8UC3, "Composed window image has expected format");
    if (!previewPath.empty()) {
        require(cv::imwrite(previewPath, canvas), "Write render preview");
    }
    still.onKey('r');
    require(!still.render().empty(), "Empty selection renders histogram placeholders");
}
} // namespace

int main(int argc, char** argv) {
    try {
        testOptions();
        testStatistics();
        testLabStatistics();
        testFullKeyCodes();
        testSwitchingAndScreenshots();
        testScreenshotFiles();
        testCoordinates();
        testHistogramHover();
        testPicker(argc > 1 ? argv[1] : "");
        std::cout << "All HSV/Lab statistics, keyboard, screenshot, coordinates, and interaction checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
