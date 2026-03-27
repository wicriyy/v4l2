# V4L2 DMA-BUF YUYV 多相机采集工具

该项目提供一个基于 V4L2 的 C++ 采集程序，支持：

- 使用 `V4L2_MEMORY_DMABUF` 从相机采集 YUYV 数据。
- 面向对象封装：`V4L2DmaBufCamera`（单相机）+ `MultiCameraManager`（多相机管理）。
- 多相机时间同步校验（基于驱动时间戳 skew 判定）。

## 目录结构

```text
.
├── CMakeLists.txt
├── include/
│   └── v4l2_dmabuf/
│       ├── camera.hpp
│       └── manager.hpp
├── src/
│   ├── camera.cpp
│   ├── main.cpp
│   └── manager.cpp
└── README.md
```

## 环境准备

### 1) 系统依赖

建议在 Linux（内核含 V4L2 支持）环境下运行，需安装：

- `g++`（支持 C++17）
- `cmake`（>= 3.16）
- 内核头文件（通常已包含 `linux/videodev2.h`）

Ubuntu/Debian 参考：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake
```

### 2) 设备与权限

- 确保摄像头设备存在，如 `/dev/video0`、`/dev/video2`。
- 确保当前用户对视频设备有访问权限（例如加入 `video` 组）。
- DMA-BUF fd 需要由你的上游模块/分配器提供（例如 dma-heap、ISP pipeline、图形栈等）。

## 编译方法（支持 amd64 / arm64）

CMake 会自动识别 `CMAKE_SYSTEM_PROCESSOR`，对 `x86_64/amd64` 与 `aarch64/arm64` 给出对应提示。

```bash
cmake -S . -B build
cmake --build build -j
```

编译产物：

- `build/v4l2_dmabuf_capture`

## 使用方法

### 命令格式

```bash
./build/v4l2_dmabuf_capture \
  --camera <device:width:height:fd0,fd1,...:output> \
  [--camera <...>] \
  [--count 120] \
  [--sync-threshold-us 2000] \
  [--timeout-sec 2]
```

### 参数说明

- `--camera`：可重复，单个相机配置，格式：
  - `<device:width:height:fd0,fd1,...:output>`
  - 示例：`/dev/video0:1280:720:10,11,12,13:cam0.yuyv`
- `--count`：采集帧数，默认 `120`。
- `--sync-threshold-us`：多相机同步阈值（微秒），默认 `2000`。
- `--timeout-sec`：每帧等待超时秒数，默认 `2`。

### 双相机示例

```bash
./build/v4l2_dmabuf_capture \
  --camera /dev/video0:1280:720:10,11,12,13:cam0.yuyv \
  --camera /dev/video2:1280:720:20,21,22,23:cam1.yuyv \
  --count 100 \
  --sync-threshold-us 1500 \
  --timeout-sec 2
```

程序会输出：

- `[SYNC_OK]`：该帧多相机时间戳 skew 在阈值内。
- `[SYNC_WARN]`：该帧多相机时间戳 skew 超出阈值。

## 说明

- 当前实现按相机顺序逐个取帧并进行时间戳对比，适合工程调试和同步性快速验证。
- 如果需要更严格的硬实时同步，可进一步引入硬件触发、统一时钟域或基于序列号/环形队列的配对策略。
