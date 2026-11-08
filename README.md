# HSV / Lab 取色工具

C++17 / OpenCV 4、5，使用 OpenCV HighGUI 显示一个窗口。输入支持 Linux
`/dev/video*` 摄像头设备、`/dev/v4l/by-id/*` 设备链接和本地图片。

## 构建和运行

需要 CMake 3.16+、C++17 编译器，以及启用了 GUI 的 OpenCV 开发包，模块包括
`core`、`imgproc`、`imgcodecs`、`highgui`、`videoio`。摄像头需要 V4L2 支持。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/hsv-picker                           # 默认 /dev/video0
./build/hsv-picker --source /dev/video2
./build/hsv-picker --source ./photo.png
./build/hsv-picker --source="./my photo.jpg"
./build/hsv-picker --source /dev/video0 --screenshot-dir ./test-images
./build/hsv-picker --help
```

如果安装了多个 OpenCV 版本，用 `-DOpenCV_DIR=...` 指定包含
`OpenCVConfig.cmake` 的目录，例如：

```sh
cmake -S . -B build-4 -DOpenCV_DIR=/path/to/opencv4/lib/cmake/opencv4
cmake --build build-4 -j
ctest --test-dir build-4 --output-on-failure
```

目标机器使用 OpenCV 4 时，请在目标机器构建，或使用匹配目标机器的 OpenCV 4
工具链交叉编译。链接 OpenCV 5 的二进制不能直接依靠 OpenCV 4 运行。

## 取色方式

- 在左侧图像上按住鼠标左键并拖动，可以向任意方向框选；拖出图像边界会自动裁剪。
- 框选由左键按下开始，左键抬起结束，不根据移动事件里的按键标志提前结束。如果在窗口外松开后选框仍未结束，按 `R` 取消。
- 默认使用 HSV。按左、右任意一个 `Shift` 在 HSV / Lab 间切换，保留选区和暂停状态，并立即重新计算当前选区；拖动过程中也可以切换。
- 拖动期间，右侧实时显示当前色彩空间三个通道的直方图和范围值；松开鼠标会把结果打印到终端。
- 摄像头拖动框选期间冻结当前显示帧，松开鼠标后自动恢复实时采集，保留的选区会随实时画面重新统计。按 `Space` 可以手动冻结/恢复；手动冻结时框选结束后仍保持冻结。
- `R` 取消框选、清除选区并恢复摄像头实时采集，`P` 再次打印当前范围，`Q` / `Esc` 退出。
- `S` / `s` 保存当前整帧原始画面为 PNG，详见下方截图说明。所有快捷键需要程序窗口获得焦点。
- 鼠标悬浮在右侧任一通道的绘图区或基线上，会出现跟随鼠标的通道值提示（如 `H = 120`、`a = 128`），并用竖线标记对应的横轴位置。数值只取决于横坐标，与柱高无关；零频数位置和未框选时也可查看。
- 提示框会在图表边缘自动调整位置；鼠标移动到绘图区外时隐藏，拖动左侧选框期间不显示。按 `Shift` 切换 HSV / Lab 后，鼠标当前位置的提示同步更新。悬浮不会暂停摄像头、修改选区或影响原始帧截图。

显示时保持图像比例，统计使用**原始分辨率的像素**，不使用缩放图像、选框线或界面文字。
选框包含起点和终点；缩小显示时，一个显示像素覆盖的原图像素都会纳入统计。
图片按 8 位 BGR 读取：灰度图片转换为三通道，透明通道忽略，高位深图片转换为 8 位。

## 输出值

窗口与终端都显示：

```text
[hbegin, hend, sbegin, send, vbegin, vend] = [35, 85, 110, 255, 70, 250]
const cv::Scalar lower = cv::Scalar(35, 110, 70);
const cv::Scalar upper = cv::Scalar(85, 255, 250);
cv::inRange(hsv, lower, upper, mask);
```

HSV 数值来自 `cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV)`：**H 为 0–179，S、V 为
0–255**，不是角度或百分比。每个 begin/end 是选区内对应通道的最小/最大值，
上下界均包含。直方图每个整数值一个 bin，纵轴是像素数，各通道独立按最大频数
缩放绘制，并显示实际最大频数 `peak count`；柱高不是百分比输出。
参见 [OpenCV 色彩转换说明](https://docs.opencv.org/4.x/de/d25/imgproc_color_conversions.html)。

范围包含所有采样像素，也会受到背景、噪声影响，因此建议只框选目标颜色。
H 是环形通道：红色可能同时出现在 0 和 179 附近，此时按最小/最大值输出的
H 范围会很宽。根据直方图在两端分别设置范围，再把两个 `inRange` 掩膜按位或，
可以避免把中间色相也纳入。程序保留原始 min/max，不自动丢弃离群值。

切换到 **Lab** 后，范围顺序为 `[lbegin, lend, abegin, aend, bbegin, bend]`，
转换使用 `cv::cvtColor(bgr, lab, cv::COLOR_BGR2Lab)`。这里显示的是 OpenCV
**8 位编码值：L、a、b 都为 0–255**，a/b 中性值为 128。L 已按 `255/100`
缩放，a/b 已加 128，可直接传入对应 Lab 图像的 `cv::inRange`，无需额外换算。
Lab 三个通道均为 256 个直方图 bin，纵轴仍表示原图选区内的像素数。
终端输出包含当前模式、正确的转换代码和上下界，例如纯白色：

```cpp
// [lbegin,lend,abegin,aend,bbegin,bend] = [255, 255, 128, 128, 128, 128]
const cv::Scalar lower = cv::Scalar(255, 128, 128);
const cv::Scalar upper = cv::Scalar(255, 128, 128);
cv::cvtColor(bgr, lab, cv::COLOR_BGR2Lab);
cv::inRange(lab, lower, upper, mask);
```

## 截图

按 `S` / `s` 保存**按键时正在显示的整帧原始 BGR 图像**，保持原始分辨率，
不包含选框、界面文字或直方图，也不受当前 HSV / Lab 模式影响。
摄像头实时采集、手动暂停、拖动框选以及图片输入时均可截图；截图不改变这些状态。
图片输入保存的是程序已解码的 8 位 BGR 图像。

默认自动创建当前工作目录下的 `./screenshots/`，可通过 `--screenshot-dir DIR`
或 `--screenshot-dir=DIR` 指定其他目录。文件采用无损 PNG，命名为
`frame_YYYYMMDD_HHMMSS_毫秒_序号.png`，独占创建文件以避免覆盖已有截图。
成功时终端打印完整路径，界面显示 2 秒保存提示；保存失败时显示错误并继续运行。
保存的 PNG 可直接通过 `--source` 再次打开作为测试图。

## GNOME / XFCE

窗口使用 HighGUI 标准接口，不依赖 GNOME 或 XFCE 特有 API。启动时请求 **1440 × 880**
的窗口尺寸，支持拖动窗口边缘调整大小和最大化；桌面可能按屏幕可用空间调整尺寸。
使用 `WINDOW_NORMAL` 和 `WINDOW_KEEPRATIO`，界面保持比例缩放，HighGUI 将鼠标坐标
映射回界面图像坐标，框选仍按原图像素统计。不调用 Wayland 不支持的窗口位置、尺寸查询接口。
OpenCV 的 GUI 后端必须可用；可选择 GTK、Qt 或原生 Wayland，XFCE 下通常使用 X11。
参见 [OpenCV HighGUI 文档](https://docs.opencv.org/4.x/d7/dfc/group__highgui.html)。

部分 OpenCV 4 的 GTK 后端不支持 `WND_PROP_VISIBLE`，会返回 `-1`。
本工具将其视为属性不支持，改用 `WND_PROP_AUTOSIZE` 判断窗口是否仍存在，避免
误判关闭导致启动后立即退出。参见 [OpenCV GTK 属性兼容问题](https://github.com/opencv/opencv/issues/25346)。
原生 Wayland 后端不支持 `getWindowProperty`；本工具检测到该后端时跳过窗口属性
查询。其他后端若也不支持属性查询，则使用 `Q` / `Esc` 退出；支持查询的 GTK/Qt
后端同时支持窗口关闭按钮退出。
键盘使用 `waitKeyEx` 保留完整键码，兼容 GTK 的修饰键状态和 Qt/Linux 的左右
Shift 键码，不将特殊键直接截成低 8 位，避免误触普通快捷键。
如果 GNOME 下 Qt 后端无法加载 Wayland 插件，可在支持 XWayland 的会话中运行：

```sh
QT_QPA_PLATFORM=xcb ./build/hsv-picker --source ./photo.png
```

无法打开摄像头、读取图片或摄像头断流时，程序向标准错误输出原因并以非零状态退出。
摄像头访问权限由系统设备权限决定。

如果目标机器仍然启动失败，请在它的桌面终端内运行并保留完整输出：

```sh
./build/hsv-picker
echo "exit code: $?"
```

程序会先输出 OpenCV 版本和输入路径；较新 OpenCV 还会输出 HighGUI 后端名称。
如果终端提示 GUI 功能未实现，需要使用启用了 GTK 或 Qt 的 OpenCV；如果提示
无法连接显示服务，请检查当前桌面会话的 `DISPLAY`。目标机器需要针对其 OpenCV 4
重新构建，避免使用本地链接 OpenCV 5 的二进制。

## 测试

`ctest` 不需要摄像头或显示服务器，覆盖已知颜色的 HSV/Lab 值、直方图频数、ROI 边界、
反向拖动、缩放坐标映射、摄像头冻结/恢复、左右 Shift 和完整键码、截图时状态保留、
PNG 逐像素一致性、连续保存、保存失败恢复、参数错误及输出值对 `inRange`
的可用性，以及直方图每个 bin 的悬浮命中、边界提示、模式切换和悬浮时摄像头/截图状态。
截图测试文件在 `/tmp` 下创建并自动清理。可额外生成一张实际界面渲染图：

```sh
./build/hsv-picker-tests /tmp/hsv-picker-preview.png
```
