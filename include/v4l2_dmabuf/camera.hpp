#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace v4l2_dmabuf {

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
  explicit V4L2DmaBufCamera(CameraConfig config);
  ~V4L2DmaBufCamera();

  V4L2DmaBufCamera(const V4L2DmaBufCamera&) = delete;
  V4L2DmaBufCamera& operator=(const V4L2DmaBufCamera&) = delete;

  const std::string& Name() const;

  void Initialize();
  void Start();
  FrameInfo CaptureOne(int timeout_sec);
  void Shutdown();

 private:
  CameraConfig config_;
  int video_fd_ = -1;
  bool streaming_ = false;
  std::vector<size_t> buf_lengths_;
  std::vector<void*> mappings_;
  std::ofstream output_;
};

}  // namespace v4l2_dmabuf
