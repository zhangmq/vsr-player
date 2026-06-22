add_library(vsrplayer STATIC
    # Core
    PlayerCore.h PlayerCore.cpp
    CommandQueue.h
    ClockManager.h ClockManager.cpp
    Demuxer.h Demuxer.cpp
    Decoder.h Decoder.cpp
    VSRProcessor.h VSRProcessor.cpp
    Renderer.h Renderer.cpp
    AudioOutput.h AudioOutput.cpp
    FramePool.h FramePool.cpp
    # Utils
    utils/CUDAContext.h utils/CUDAContext.cpp
    utils/VulkanContext.h utils/VulkanContext.cpp
    utils/NV12ToRGB.h utils/NV12ToRGB.cpp
)

target_include_directories(vsrplayer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/api"
)

target_link_libraries(vsrplayer PUBLIC
    ${FFMPEG_LIBRARIES}
    Vulkan::Vulkan
    ${PORTAUDIO_LIBRARIES}
    ${CUDA_LIBRARY}
)

target_include_directories(vsrplayer SYSTEM PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    ${PORTAUDIO_INCLUDE_DIRS}
    ${NVVFX_INCLUDE_DIR}
)

target_link_directories(vsrplayer PRIVATE "${NVVFX_LIB_DIR}")

# VFX SDK libs are loaded at runtime via dlopen or linked directly
target_link_libraries(vsrplayer PRIVATE
    ${NVVFX_LIB_DIR}/libnvVFXVideoSuperRes.so
    ${NVVFX_LIB_DIR}/libVideoFX.so
)
