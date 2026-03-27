#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "v4l2_dmabuf/camera.hpp"

namespace v4l2_dmabuf {

class MultiCameraManager {
 public:
  explicit MultiCameraManager(std::vector<CameraConfig> configs);

  void InitializeAll();
  void StartAll();
  void CaptureWithSyncCheck(uint32_t frame_count, uint64_t sync_threshold_us, int timeout_sec);

 private:
  std::vector<std::unique_ptr<V4L2DmaBufCamera>> cameras_;
};

std::vector<int> ParseFdList(const std::string& input);
CameraConfig ParseCameraArg(const std::string& value, size_t index);

}  // namespace v4l2_dmabuf
