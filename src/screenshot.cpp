#include "screenshot.hpp"

#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace hsvpicker {

ScreenshotResult saveScreenshot(const cv::Mat& bgr, const std::string& directory) {
    try {
        if (bgr.empty() || bgr.type() != CV_8UC3) {
            throw std::invalid_argument("Screenshot requires a nonempty 8-bit BGR frame");
        }
        if (directory.empty()) {
            throw std::invalid_argument("Screenshot directory must not be empty");
        }
        const auto destination = std::filesystem::absolute(directory);
        std::filesystem::create_directories(destination);

        std::vector<unsigned char> png;
        if (!cv::imencode(".png", bgr, png)) {
            throw std::runtime_error("PNG encoding failed");
        }

        const auto now = std::chrono::system_clock::now();
        const auto timestamp = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        if (!localtime_r(&timestamp, &local)) {
            throw std::runtime_error("Cannot format screenshot timestamp");
        }
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
        std::ostringstream prefix;
        prefix << "frame_" << std::put_time(&local, "%Y%m%d_%H%M%S") << '_'
               << std::setfill('0') << std::setw(3) << milliseconds << '_';

        static std::atomic<unsigned> sequence{0};
        std::string path;
        int descriptor;
        do {
            path = (destination / (prefix.str() + std::to_string(sequence++) + ".png")).string();
            // Exclusive creation prevents overwrites, including concurrent instances.
            descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        } while (descriptor < 0 && errno == EEXIST);
        if (descriptor < 0) {
            throw std::runtime_error("Cannot create screenshot: " + std::string(std::strerror(errno)));
        }

        std::size_t offset = 0;
        while (offset < png.size()) {
            const auto written = ::write(descriptor, png.data() + offset, png.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                const std::string error = written < 0 ? std::strerror(errno) : "Incomplete write";
                ::close(descriptor);
                ::unlink(path.c_str());
                return {{}, "Cannot write screenshot: " + error};
            }
            offset += static_cast<std::size_t>(written);
        }
        if (::close(descriptor) != 0) {
            const std::string error = std::strerror(errno);
            ::unlink(path.c_str());
            return {{}, "Cannot close screenshot: " + error};
        }
        return {path, {}};
    } catch (const std::exception& error) {
        return {{}, error.what()};
    }
}

} // namespace hsvpicker
