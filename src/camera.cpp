#include "v4l2_dmabuf/camera.hpp"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace v4l2_dmabuf {
namespace {

int xioctl(int fd, unsigned long request, void* arg) {
  int ret;
  do {
    ret = ioctl(fd, request, arg);
  } while (ret == -1 && errno == EINTR);
  return ret;
}

}  // namespace

V4L2DmaBufCamera::V4L2DmaBufCamera(CameraConfig config) : config_(std::move(config)) {}

V4L2DmaBufCamera::~V4L2DmaBufCamera() { Shutdown(); }

const std::string& V4L2DmaBufCamera::Name() const { return config_.name; }

void V4L2DmaBufCamera::Initialize() {
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
    throw std::runtime_error("camera " + config_.name + ": no video capture support");
  }
  if (!(dev_caps & V4L2_CAP_STREAMING)) {
    throw std::runtime_error("camera " + config_.name + ": no streaming support");
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
    throw std::runtime_error("camera " + config_.name + ": YUYV not accepted");
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
    throw std::runtime_error("camera " + config_.name + ": insufficient buffers");
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
                               ": VIDIOC_QUERYBUF failed: " + std::string(std::strerror(errno)));
    }

    buf_lengths_[i] = buf.length;
    mappings_[i] = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                        config_.dmabuf_fds[i], 0);
    if (mappings_[i] == MAP_FAILED) {
      throw std::runtime_error("camera " + config_.name +
                               ": mmap failed: " + std::string(std::strerror(errno)));
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
                             ": open output failed: " + config_.output);
  }
}

void V4L2DmaBufCamera::Start() {
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

FrameInfo V4L2DmaBufCamera::CaptureOne(int timeout_sec) {
  if (!streaming_) {
    throw std::runtime_error("camera " + config_.name + ": stream not started");
  }

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(video_fd_, &fds);
  timeval tv{};
  tv.tv_sec = timeout_sec;
  tv.tv_usec = 0;

  const int ret = select(video_fd_ + 1, &fds, nullptr, nullptr, &tv);
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
    throw std::runtime_error("camera " + config_.name + ": invalid buffer index");
  }

  const size_t bytes = std::min<size_t>(dbuf.bytesused, buf_lengths_[dbuf.index]);
  output_.write(static_cast<const char*>(mappings_[dbuf.index]), static_cast<std::streamsize>(bytes));
  if (!output_) {
    throw std::runtime_error("camera " + config_.name + ": output write failed");
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

void V4L2DmaBufCamera::Shutdown() {
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

}  // namespace v4l2_dmabuf
