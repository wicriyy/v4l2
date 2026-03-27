#include "v4l2_dmabuf/manager.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace v4l2_dmabuf {

MultiCameraManager::MultiCameraManager(std::vector<CameraConfig> configs) {
  cameras_.reserve(configs.size());
  for (auto& cfg : configs) {
    cameras_.push_back(std::make_unique<V4L2DmaBufCamera>(std::move(cfg)));
  }
}

void MultiCameraManager::InitializeAll() {
  for (auto& cam : cameras_) {
    cam->Initialize();
  }
}

void MultiCameraManager::StartAll() {
  for (auto& cam : cameras_) {
    cam->Start();
  }
}

void MultiCameraManager::CaptureWithSyncCheck(uint32_t frame_count, uint64_t sync_threshold_us,
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

}  // namespace v4l2_dmabuf
