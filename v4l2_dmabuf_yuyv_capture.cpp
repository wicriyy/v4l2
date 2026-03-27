#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

int xioctl(int fd, unsigned long request, void* arg) {
  int ret;
  do {
    ret = ioctl(fd, request, arg);
  } while (ret == -1 && errno == EINTR);
  return ret;
}

struct CameraConfig {
  std::string name;
  std::string device;
  std::string output;
  uint32_t width = 1280;
  uint32_t height = 720;
  std::vector<int> dmabuf_fds;
};

struct FrameInfo {
  uint32_t bytesused = 0;
  uint64_t timestamp_us = 0;
};

class V4L2DmaBufCamera {
 public:
  explicit V4L2DmaBufCamera(CameraConfig config) : config_(std::move(config)) {}

  ~V4L2DmaBufCamera() { Shutdown(); }

  const std::string& Name() const { return config_.name; }

  void Initialize() {
    if (config_.dmabuf_fds.empty()) {
      throw std::invalid_argument("camera " + config_.name + ": no DMA-BUF fds provided");
    }

    video_fd_ = open(config_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (video_fd_ < 0) {
      throw std::runtime_error("camera " + config_.name + ": open failed: " +
                               std::string(std::strerror(errno)));
    }

    v4l2_capability cap{};
    if (xioctl(video_fd_, VIDIOC_QUERYCAP, &cap) < 0) {
      throw std::runtime_error("camera " + config_.name +
                               ": VIDIOC_QUERYCAP failed: " + std::string(std::strerror(errno)));
    }

    const uint32_t caps = cap.capabilities;
    const uint32_t dev_caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : caps;
    if (!(dev_caps & V4L2_CAP_VIDEO_CAPTURE)) {
      throw std::runtime_error("camera " + config_.name +
                               ": device does not support video capture");
    }
    if (!(dev_caps & V4L2_CAP_STREAMING)) {
      throw std::runtime_error("camera " + config_.name +
                               ": device does not support streaming");
    }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = config_.width;
    fmt.fmt.pix.height = config_.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(video_fd_, VIDIOC_S_FMT, &fmt) < 0) {
      throw std::runtime_error("camera " + config_.name +
                               ": VIDIOC_S_FMT failed: " + std::string(std::strerror(errno)));
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
      throw std::runtime_error("camera " + config_.name +
                               ": driver did not accept YUYV format");
    }

    v4l2_requestbuffers req{};
    req.count = static_cast<uint32_t>(config_.dmabuf_fds.size());
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_DMABUF;

    if (xioctl(video_fd_, VIDIOC_REQBUFS, &req) < 0) {
      throw std::runtime_error("camera " + config_.name +
                               ": VIDIOC_REQBUFS failed: " + std::string(std::strerror(errno)));
    }
    if (req.count < config_.dmabuf_fds.size()) {
      throw std::runtime_error("camera " + config_.name +
                               ": driver allocated fewer buffers than requested");
    }

    buf_lengths_.assign(req.count, 0);
    mappings_.assign(req.count, MAP_FAILED);

    for (uint32_t i = 0; i < req.count; ++i) {
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_DMABUF;
      buf.index = i;

      if (xioctl(video_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
        throw std::runtime_error("camera " + config_.name +
                                 ": VIDIOC_QUERYBUF failed: " +
                                 std::string(std::strerror(errno)));
      }

      buf_lengths_[i] = buf.length;
      mappings_[i] = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                          config_.dmabuf_fds[i], 0);
      if (mappings_[i] == MAP_FAILED) {
        throw std::runtime_error("camera " + config_.name + ": mmap dmabuf failed: " +
                                 std::string(std::strerror(errno)));
      }

      v4l2_buffer qbuf{};
      qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      qbuf.memory = V4L2_MEMORY_DMABUF;
      qbuf.index = i;
      qbuf.m.fd = config_.dmabuf_fds[i];
      qbuf.length = buf.length;

      if (xioctl(video_fd_, VIDIOC_QBUF, &qbuf) < 0) {
        throw std::runtime_error("camera " + config_.name +
                                 ": VIDIOC_QBUF failed: " + std::string(std::strerror(errno)));
      }
    }

    output_.open(config_.output, std::ios::binary);
    if (!output_) {
      throw std::runtime_error("camera " + config_.name +
                               ": failed to open output file: " + config_.output);
    }
  }

  void Start() {
    if (streaming_) {
      return;
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(video_fd_, VIDIOC_STREAMON, &type) < 0) {
      throw std::runtime_error("camera " + config_.name +
                               ": VIDIOC_STREAMON failed: " + std::string(std::strerror(errno)));
    }
    streaming_ = true;
  }

  FrameInfo CaptureOne(int timeout_sec) {
    if (!streaming_) {
      throw std::runtime_error("camera " + config_.name + ": stream is not started");
    }

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(video_fd_, &fds);
    timeval tv{};
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    int ret = select(video_fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (ret == -1) {
      throw std::runtime_error("camera " + config_.name +
                               ": select failed: " + std::string(std::strerror(errno)));
    }
    if (ret == 0) {
      throw std::runtime_error("camera " + config_.name + ": select timeout");
    }

    v4l2_buffer dbuf{};
    dbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    dbuf.memory = V4L2_MEMORY_DMABUF;

    if (xioctl(video_fd_, VIDIOC_DQBUF, &dbuf) < 0) {
      throw std::runtime_error("camera " + config_.name +
                               ": VIDIOC_DQBUF failed: " + std::string(std::strerror(errno)));
    }

    if (dbuf.index >= mappings_.size() || mappings_[dbuf.index] == MAP_FAILED) {
      throw std::runtime_error("camera " + config_.name + ": invalid buffer index returned");
    }

    const size_t bytes = std::min<size_t>(dbuf.bytesused, buf_lengths_[dbuf.index]);
    output_.write(static_cast<const char*>(mappings_[dbuf.index]), static_cast<std::streamsize>(bytes));
    if (!output_) {
      throw std::runtime_error("camera " + config_.name + ": write output failed");
    }

    const uint64_t timestamp_us = static_cast<uint64_t>(dbuf.timestamp.tv_sec) * 1000000ULL +
                                  static_cast<uint64_t>(dbuf.timestamp.tv_usec);

    if (xioctl(video_fd_, VIDIOC_QBUF, &dbuf) < 0) {
      throw std::runtime_error("camera " + config_.name +
                               ": VIDIOC_QBUF(requeue) failed: " +
                               std::string(std::strerror(errno)));
    }

    return FrameInfo{static_cast<uint32_t>(bytes), timestamp_us};
  }

  void Shutdown() {
    if (video_fd_ >= 0 && streaming_) {
      v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      xioctl(video_fd_, VIDIOC_STREAMOFF, &type);
      streaming_ = false;
    }

    for (size_t i = 0; i < mappings_.size(); ++i) {
      if (mappings_[i] != MAP_FAILED) {
        munmap(mappings_[i], buf_lengths_[i]);
        mappings_[i] = MAP_FAILED;
      }
    }

    if (output_.is_open()) {
      output_.close();
    }

    if (video_fd_ >= 0) {
      close(video_fd_);
      video_fd_ = -1;
    }
  }

 private:
  CameraConfig config_;
  int video_fd_ = -1;
  bool streaming_ = false;
  std::vector<size_t> buf_lengths_;
  std::vector<void*> mappings_;
  std::ofstream output_;
};

class MultiCameraManager {
 public:
  explicit MultiCameraManager(std::vector<CameraConfig> configs) {
    cameras_.reserve(configs.size());
    for (auto& cfg : configs) {
      cameras_.push_back(std::make_unique<V4L2DmaBufCamera>(std::move(cfg)));
    }
  }

  void InitializeAll() {
    for (auto& cam : cameras_) {
      cam->Initialize();
    }
  }

  void StartAll() {
    for (auto& cam : cameras_) {
      cam->Start();
    }
  }

  void CaptureWithSyncCheck(uint32_t frame_count, uint64_t sync_threshold_us,
                            int timeout_sec) {
    if (cameras_.empty()) {
      throw std::invalid_argument("no camera configured");
    }

    for (uint32_t frame = 0; frame < frame_count; ++frame) {
      std::vector<uint64_t> ts;
      ts.reserve(cameras_.size());

      for (auto& cam : cameras_) {
        FrameInfo info = cam->CaptureOne(timeout_sec);
        ts.push_back(info.timestamp_us);
      }

      const auto minmax = std::minmax_element(ts.begin(), ts.end());
      const uint64_t skew = *minmax.second - *minmax.first;
      if (skew > sync_threshold_us) {
        std::cerr << "[SYNC_WARN] frame=" << frame << " skew=" << skew
                  << "us threshold=" << sync_threshold_us << "us\n";
      } else {
        std::cout << "[SYNC_OK] frame=" << frame << " skew=" << skew << "us\n";
      }
    }
  }

 private:
  std::vector<std::unique_ptr<V4L2DmaBufCamera>> cameras_;
};

std::vector<int> ParseFdList(const std::string& input) {
  std::vector<int> fds;
  std::stringstream ss(input);
  std::string token;
  while (std::getline(ss, token, ',')) {
    if (token.empty()) {
      continue;
    }
    int fd = std::stoi(token);
    if (fd < 0) {
      throw std::invalid_argument("dmabuf fd must be >= 0");
    }
    fds.push_back(fd);
  }
  if (fds.empty()) {
    throw std::invalid_argument("empty dmabuf fd list");
  }
  return fds;
}

CameraConfig ParseCameraArg(const std::string& value, size_t index) {
  // format: /dev/video0:1280:720:10,11,12:cam0.yuyv
  std::vector<std::string> parts;
  std::stringstream ss(value);
  std::string token;
  while (std::getline(ss, token, ':')) {
    parts.push_back(token);
  }
  if (parts.size() != 5) {
    throw std::invalid_argument(
        "--camera format should be <device>:<width>:<height>:<fd0,fd1,...>:<output>");
  }

  CameraConfig cfg;
  cfg.name = "cam" + std::to_string(index);
  cfg.device = parts[0];
  cfg.width = static_cast<uint32_t>(std::stoul(parts[1]));
  cfg.height = static_cast<uint32_t>(std::stoul(parts[2]));
  cfg.dmabuf_fds = ParseFdList(parts[3]);
  cfg.output = parts[4];
  return cfg;
}

[[noreturn]] void Usage(const char* prog) {
  std::cerr
      << "Usage:\n"
      << "  " << prog
      << " --camera <device:width:height:fd0,fd1,...:output> [--camera ...]"
      << " [--count 120] [--sync-threshold-us 2000] [--timeout-sec 2]\n\n"
      << "Example:\n"
      << "  " << prog
      << " --camera /dev/video0:1280:720:10,11,12,13:cam0.yuyv"
      << " --camera /dev/video2:1280:720:20,21,22,23:cam1.yuyv"
      << " --count 100 --sync-threshold-us 1500\n";
  std::exit(1);
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    std::vector<CameraConfig> cameras;
    uint32_t frame_count = 120;
    uint64_t sync_threshold_us = 2000;
    int timeout_sec = 2;

    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      auto get_next = [&](const std::string& name) -> std::string {
        if (i + 1 >= argc) {
          throw std::invalid_argument("missing value for " + name);
        }
        return argv[++i];
      };

      if (arg == "--camera") {
        cameras.push_back(ParseCameraArg(get_next(arg), cameras.size()));
      } else if (arg == "--count") {
        frame_count = static_cast<uint32_t>(std::stoul(get_next(arg)));
      } else if (arg == "--sync-threshold-us") {
        sync_threshold_us = static_cast<uint64_t>(std::stoull(get_next(arg)));
      } else if (arg == "--timeout-sec") {
        timeout_sec = std::stoi(get_next(arg));
      } else if (arg == "--help" || arg == "-h") {
        Usage(argv[0]);
      } else {
        throw std::invalid_argument("unknown argument: " + arg);
      }
    }

    if (cameras.empty()) {
      throw std::invalid_argument("at least one --camera is required");
    }

    MultiCameraManager manager(std::move(cameras));
    manager.InitializeAll();
    manager.StartAll();
    manager.CaptureWithSyncCheck(frame_count, sync_threshold_us, timeout_sec);

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    return 1;
  }
}
