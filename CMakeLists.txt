cmake_minimum_required(VERSION 3.16)
project(hsv_picker VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No major-version constraint: find either OpenCV 4 or OpenCV 5.
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs highgui videoio)
if(OpenCV_VERSION VERSION_LESS 4 OR NOT OpenCV_VERSION VERSION_LESS 6)
    message(FATAL_ERROR "This project supports OpenCV 4 and 5")
endif()

add_library(hsv_picker_core src/hsv_picker.cpp src/screenshot.cpp)
target_include_directories(hsv_picker_core PUBLIC include ${OpenCV_INCLUDE_DIRS})
target_link_libraries(hsv_picker_core PUBLIC ${OpenCV_LIBS})
target_compile_options(hsv_picker_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(hsv-picker src/main.cpp)
target_link_libraries(hsv-picker PRIVATE hsv_picker_core)
target_compile_options(hsv-picker PRIVATE -Wall -Wextra -Wpedantic)

# This API is absent in older OpenCV 4 releases. Detect it instead of requiring it.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${OpenCV_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${OpenCV_LIBS})
# GCC 8 and older may need an explicit filesystem library on the target host.
check_cxx_source_compiles("#include <filesystem>
int main() { return std::filesystem::current_path().empty(); }" HSV_HAS_FILESYSTEM)
if(NOT HSV_HAS_FILESYSTEM)
    set(CMAKE_REQUIRED_LIBRARIES ${OpenCV_LIBS} stdc++fs)
    check_cxx_source_compiles("#include <filesystem>
int main() { return std::filesystem::current_path().empty(); }" HSV_HAS_FILESYSTEM_LIBRARY)
    if(NOT HSV_HAS_FILESYSTEM_LIBRARY)
        message(FATAL_ERROR "The compiler must provide C++17 std::filesystem")
    endif()
    target_link_libraries(hsv_picker_core PUBLIC stdc++fs)
endif()
set(CMAKE_REQUIRED_LIBRARIES ${OpenCV_LIBS})
check_cxx_source_compiles("#include <opencv2/highgui.hpp>
int main() { return cv::currentUIFramework().empty(); }" HSV_HAS_UI_FRAMEWORK)
if(HSV_HAS_UI_FRAMEWORK)
    target_compile_definitions(hsv-picker PRIVATE HSV_HAS_UI_FRAMEWORK=1)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(hsv-picker-tests tests/hsv_picker_tests.cpp)
    target_link_libraries(hsv-picker-tests PRIVATE hsv_picker_core)
    target_compile_options(hsv-picker-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hsv-picker-core COMMAND hsv-picker-tests)
    add_test(NAME hsv-picker-help COMMAND hsv-picker --help)
endif()

install(TARGETS hsv-picker RUNTIME DESTINATION bin)
