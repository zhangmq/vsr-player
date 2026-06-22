cmake_minimum_required(VERSION 3.22)

# GCC 16.x: CMake 4.3.4 doesn't recognize this compiler version yet.
# Override compiler version to GCC 15 to use known feature table.
set(CMAKE_C_COMPILER_VERSION "15.0.0")
set(CMAKE_C_COMPILER_VERSION_INTERNAL "")
set(CMAKE_CXX_COMPILER_VERSION "15.0.0")
set(CMAKE_CXX_COMPILER_VERSION_INTERNAL "")

project(vsr-player VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Dependencies ----

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED
    libavcodec libavformat libavutil libswscale libswresample)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
pkg_check_modules(WAYLAND REQUIRED wayland-client)
pkg_check_modules(X11 REQUIRED x11)

# CUDA (driver API only)
find_library(CUDA_LIBRARY NAMES cuda REQUIRED)

# ---- Third-party / bundled SDK ----

set(NVVFX_ROOT "${CMAKE_SOURCE_DIR}/third_party/nvvfx" CACHE PATH "NvVFX SDK root")
set(NVVFX_INCLUDE_DIR "${NVVFX_ROOT}/include")
set(NVVFX_LIB_DIR "${NVVFX_ROOT}/lib")

# ---- Subdirectories ----

add_subdirectory(src/core)
add_subdirectory(src/client)
