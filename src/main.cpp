#include "hsv_picker.hpp"
#include "screenshot.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/core/version.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
constexpr const char* windowName = "color-picker";

void mouseCallback(int event, int x, int y, int flags, void* userData) {
    static_cast<hsvpicker::Picker*>(userData)->onMouse(event, x, y, flags);
}

void printSelection(const hsvpicker::Picker& picker) {
    if (!picker.statistics()) {
        return;
    }
    const auto& stats = *picker.statistics();
    const auto& roi = picker.selection();
    const bool lab = stats.colorSpace == hsvpicker::ColorSpace::Lab;
    std::cout << "Color space: " << hsvpicker::colorSpaceName(stats.colorSpace) << '\n'
              << "ROI: x=" << roi.x << " y=" << roi.y << " w=" << roi.width
              << " h=" << roi.height << " pixels=" << stats.pixels << '\n'
              << hsvpicker::rangeLabel(stats.colorSpace) << " = "
              << hsvpicker::rangeText(stats) << '\n'
              << "const cv::Scalar lower = " << hsvpicker::scalarText(stats, false) << ";\n"
              << "const cv::Scalar upper = " << hsvpicker::scalarText(stats, true) << ";\n"
              << "cv::cvtColor(bgr, " << (lab ? "lab" : "hsv") << ", cv::"
              << (lab ? "COLOR_BGR2Lab" : "COLOR_BGR2HSV") << ");\n"
              << "cv::inRange(" << (lab ? "lab" : "hsv") << ", lower, upper, mask);\n" << std::flush;
}

class WindowMonitor {
public:
    explicit WindowMonitor(const std::string& backend)
        : checkProperties_(backend != "WAYLAND"), checkSize_(checkProperties_),
          probeRatio_(backend.empty() || backend == "QT"),
          checkWidgetRatio_(backend.empty() || backend.rfind("GTK", 0) == 0) {}

    void updateLayout(hsvpicker::Picker& picker) {
        if (!checkSize_) {
            return;
        }
        cv::Size size;
        try {
            size = cv::getWindowImageRect(windowName).size();
        } catch (const cv::Exception&) {
            checkSize_ = false;
            return;
        }
        if (size.empty()) {
            return;
        }
        // On Qt the image rect excludes letterboxing. Once a size query
        // succeeds, let the composed canvas use the entire client area.
        // Older builds have no backend name; Qt returns the ratio mode here,
        // while GTK returns the actual width/height ratio.
        if (probeRatio_) {
            probeRatio_ = false;
            try {
                if (cv::getWindowProperty(windowName, cv::WND_PROP_ASPECT_RATIO) == cv::WINDOW_KEEPRATIO) {
                    cv::setWindowProperty(windowName, cv::WND_PROP_ASPECT_RATIO, cv::WINDOW_FREERATIO);
                    checkWidgetRatio_ = false;
                    return;
                }
            } catch (const cv::Exception&) {
                // Backends without ratio control retain uniform display scaling.
            }
        }
        // GTK reports the fitted image, while its aspect property describes
        // the whole widget. Recover the space occupied by the letterbox bars.
        if (checkWidgetRatio_) {
            try {
                const double ratio = cv::getWindowProperty(windowName, cv::WND_PROP_ASPECT_RATIO);
                if (std::isfinite(ratio) && ratio > 0 && ratio != cv::WINDOW_FREERATIO) {
                    if (size.width < size.height * ratio) {
                        size.width = cvRound(size.height * ratio);
                    } else {
                        size.height = cvRound(size.width / ratio);
                    }
                }
            } catch (const cv::Exception&) {
                checkWidgetRatio_ = false;
            }
        }
        picker.setCanvasSize(size);
    }

    bool isOpen() {
        if (!checkProperties_) {
            return true;
        }
        try {
            const double visible = cv::getWindowProperty(windowName, cv::WND_PROP_VISIBLE);
            if (visible >= 0) {
                visibilitySupported_ = true;
                return visible > 0;
            }
            if (visibilitySupported_) {
                return false;
            }
        } catch (const cv::Exception&) {
            if (visibilitySupported_) {
                return false;
            }
        }

        // Older GTK backends always return -1 for VISIBLE. AUTOSIZE is 0/1
        // while the window exists, then -1 (or an exception) after closing it.
        // Only infer closure after the property was confirmed to work once.
        try {
            const double autosize = cv::getWindowProperty(windowName, cv::WND_PROP_AUTOSIZE);
            if (autosize >= 0) {
                autosizeSupported_ = true;
                return true;
            }
        } catch (const cv::Exception&) {
            // An unsupported property must not terminate the application.
        }
        return !autosizeSupported_;
    }

private:
    bool checkProperties_;
    bool checkSize_;
    bool probeRatio_;
    bool checkWidgetRatio_;
    bool visibilitySupported_ = false;
    bool autosizeSupported_ = false;
};
} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::string> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.emplace_back(argv[i]);
        }
        const auto options = hsvpicker::parseOptions(arguments);
        if (options.help) {
            std::cout << "Usage: color-picker [--source DEVICE_OR_IMAGE] [--screenshot-dir DIR]\n"
                      << "Default source: /dev/video0\n"
                      << "Default screenshot directory: ./screenshots (PNG, full original BGR frame)\n"
                      << "Examples:\n"
                      << "  color-picker --source /dev/video2\n"
                      << "  color-picker --source ./image.png\n"
                      << "Controls: drag left mouse to select; Space freezes/resumes camera;\n"
                      << "          Shift switches HSV/Lab; S saves a screenshot;\n"
                      << "          R clears/resumes; P prints OpenCV bounds; Q/Esc quits.\n"
                      << "Default space: HSV (H 0..179, S/V 0..255).\n"
                      << "Lab units: COLOR_BGR2Lab, 8-bit L/a/b 0..255, neutral a/b = 128.\n";
            return EXIT_SUCCESS;
        }

        std::cout << "OpenCV: " << CV_VERSION << '\n'
                  << "Source: " << options.source << '\n' << std::flush;
        const bool camera = hsvpicker::isCameraSource(options.source);
        cv::VideoCapture capture;
        cv::Mat frame;
        if (camera) {
            if (!capture.open(options.source, cv::CAP_V4L2)) {
                throw std::runtime_error("Cannot open camera: " + options.source +
                                         " (check device path and permissions)");
            }
            if (!capture.read(frame) || frame.empty()) {
                throw std::runtime_error("Cannot read the first camera frame: " + options.source);
            }
        } else {
            frame = cv::imread(options.source, cv::IMREAD_COLOR);
            if (frame.empty()) {
                throw std::runtime_error("Cannot load image: " + options.source);
            }
        }

        hsvpicker::Picker picker(camera);
        picker.setFrame(frame);
        // HighGUI maps mouse callbacks back to composed-image coordinates when
        // scaling the image. NORMAL permits resizing/maximizing on the desktop.
        cv::namedWindow(windowName, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO |
                                    cv::WINDOW_GUI_NORMAL);
        cv::imshow(windowName, picker.render());
        // Request the initial size after assigning the image, so the backend
        // does not replace it with its small default window dimensions.
        const auto initial = picker.layout().canvas;
        cv::resizeWindow(windowName, initial.width, initial.height);
        cv::setMouseCallback(windowName, mouseCallback, &picker);
        std::string backend;
#ifdef HSV_HAS_UI_FRAMEWORK
        // The native Wayland backend does not implement getWindowProperty.
        backend = cv::currentUIFramework();
        std::cout << "HighGUI backend: " << backend << '\n';
#endif
        WindowMonitor windowMonitor(backend);
        std::cout << "Drag left mouse to select. Camera freezes during dragging and resumes on release.\n"
                  << "Shift: HSV/Lab; S: save original frame to " << options.screenshotDir << '\n'
                  << "Release the mouse or press P to print OpenCV 8-bit bounds.\n"
                  << std::flush;
        while (true) {
            windowMonitor.updateLayout(picker);
            cv::imshow(windowName, picker.render());
            const int key = cv::waitKeyEx(20);
            if (!windowMonitor.isOpen() || !picker.onKey(key)) {
                break;
            }
            if (picker.takeReportRequest()) {
                printSelection(picker);
            }
            if (auto screenshot = picker.takeScreenshotRequest()) {
                const auto result = hsvpicker::saveScreenshot(*screenshot, options.screenshotDir);
                if (result) {
                    std::cout << "Screenshot saved: " << result.path << '\n' << std::flush;
                    picker.setNotice("Screenshot saved. Full path is printed in the terminal.");
                } else {
                    std::cerr << "Screenshot failed: " << result.error << '\n';
                    picker.setNotice("Screenshot failed. See terminal for details.", true);
                }
            }
            // Read after processing events, so a drag samples the frame just displayed.
            if (camera && !picker.frozen()) {
                if (!capture.read(frame) || frame.empty()) {
                    throw std::runtime_error("Camera stream stopped: " + options.source);
                }
                picker.setFrame(frame);
            }
        }
        cv::destroyAllWindows();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "color-picker: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
