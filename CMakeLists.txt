cmake_minimum_required(VERSION 3.16)
project(v4l2_dmabuf_capture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(STATUS "Target architecture: amd64/x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  message(STATUS "Target architecture: arm64/aarch64")
else()
  message(WARNING "Unrecognized architecture: ${CMAKE_SYSTEM_PROCESSOR}. Building with generic flags.")
endif()

add_library(v4l2_dmabuf_core
  src/camera.cpp
  src/manager.cpp
)

target_include_directories(v4l2_dmabuf_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(v4l2_dmabuf_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(v4l2_dmabuf_capture
  src/main.cpp
)

target_link_libraries(v4l2_dmabuf_capture
  PRIVATE
    v4l2_dmabuf_core
)
