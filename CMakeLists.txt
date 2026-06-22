cmake_minimum_required(VERSION 3.22)
project(vsr-player VERSION 0.1.0 LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Dependencies ----

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED
    libavcodec libavformat libavutil libswresample)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

# ---- Third-party / bundled SDK ----

# NvVFX C SDK headers (from NGC EA download, placed in third_party/nvvfx/include)
set(NVVFX_ROOT "${CMAKE_SOURCE_DIR}/third_party/nvvfx" CACHE PATH "NvVFX SDK root")
set(NVVFX_INCLUDE_DIR "${NVVFX_ROOT}/include")
set(NVVFX_LIB_DIR "${NVVFX_ROOT}/lib")

# CUDA (driver API only, no toolkit needed for runtime)
find_library(CUDA_LIBRARY NAMES cuda REQUIRED)

# ---- Subdirectories ----

add_subdirectory(src/core)
add_subdirectory(src/client)
