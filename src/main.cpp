#include "v4l2_dmabuf/manager.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

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
    std::vector<v4l2_dmabuf::CameraConfig> cameras;
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
        cameras.push_back(v4l2_dmabuf::ParseCameraArg(get_next(arg), cameras.size()));
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

    v4l2_dmabuf::MultiCameraManager manager(std::move(cameras));
    manager.InitializeAll();
    manager.StartAll();
    manager.CaptureWithSyncCheck(frame_count, sync_threshold_us, timeout_sec);

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    return 1;
  }
}
