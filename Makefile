# VSR Player — Makefile
BUILD_DIR := build
BIN := $(BUILD_DIR)/vsr-player

COREDIR := src/core
CLIENTDIR := src/client

CXX := g++
PKGS := Qt6Widgets vulkan libavcodec libavformat libavutil libswscale wayland-client portaudio-2.0

# CUDA include / lib paths (bundled in third_party/)
CUDA_DIR := third_party/cuda
CUDA_INC  := $(CUDA_DIR)/include
CUDA_LIB  := $(CUDA_DIR)/lib

QT6_GUI_VER := $(shell pkg-config --modversion Qt6Gui 2>/dev/null)
QPA_INC := /usr/include/qt6/QtGui/$(QT6_GUI_VER)/
CXXFLAGS := -std=c++20 -Wall -Wextra -fPIC -O2 -DNDEBUG \
            -Wno-missing-field-initializers \
            $(shell pkg-config --cflags $(PKGS)) \
            -I$(QPA_INC) \
            -I$(CUDA_INC) \
            -Isrc/core -Isrc/core/api -Isrc/client -Isrc/core/utils -I$(BUILD_DIR)/shaders \
            -Ithird_party/nvvfx/include
LDFLAGS  := $(shell pkg-config --libs $(PKGS)) \
            -L$(CUDA_LIB) -lnvrtc -lnvrtc-builtins \
            -lcuda -ldl \
            -Wl,--disable-new-dtags \
            -Wl,-rpath,/home/zmq/projects/vsr-player/third_party/cuda/lib \
            -Lthird_party/nvvfx/lib -lNVCVImage \
            -Wl,-rpath,/home/zmq/projects/vsr-player/third_party/nvvfx/lib

MOC := /usr/lib/qt6/moc
GLSLC := glslc

.SUFFIXES:
.PHONY: all clean debug

# ---- Shader sources and generated headers ----
VERT_SPV := $(BUILD_DIR)/shaders/video.vert.spv
FRAG_SPV := $(BUILD_DIR)/shaders/video.frag.spv
VERT_H   := $(BUILD_DIR)/shaders/video_vert_spv.h
FRAG_H   := $(BUILD_DIR)/shaders/video_frag_spv.h
NV12_FRAG_SPV := $(BUILD_DIR)/shaders/nv12.frag.spv
NV12_FRAG_H   := $(BUILD_DIR)/shaders/nv12_frag_spv.h
SHADERS  := $(VERT_H) $(FRAG_H) $(NV12_FRAG_H)

# ---- MOC generated sources ----
MOC_SRC := $(BUILD_DIR)/moc_MainWindow.cpp $(BUILD_DIR)/moc_VulkanWidget.cpp

# ---- Object lists ----
CORE_OBJS := $(BUILD_DIR)/src/core/Demuxer.o \
             $(BUILD_DIR)/src/core/Decoder.o \
             $(BUILD_DIR)/src/core/VSRProcessor.o \
             $(BUILD_DIR)/src/core/AudioOutput.o \
             $(BUILD_DIR)/src/core/utils/VulkanContext.o \
             $(BUILD_DIR)/src/core/utils/SwapchainManager.o \
             $(BUILD_DIR)/src/core/utils/VulkanRenderer.o \
             $(BUILD_DIR)/src/core/utils/CUDAContext.o \
             $(BUILD_DIR)/src/core/utils/NV12ToRGB.o \
             $(BUILD_DIR)/src/core/utils/InteropTexture.o \
             $(BUILD_DIR)/src/core/utils/VideoPipeline.o

CLIENT_OBJS := $(BUILD_DIR)/src/client/main.o \
               $(BUILD_DIR)/src/client/MainWindow.o \
               $(BUILD_DIR)/src/client/VulkanWidget.o

MOC_OBJS := $(MOC_SRC:.cpp=.o)

OBJS := $(CORE_OBJS) $(CLIENT_OBJS) $(MOC_OBJS)

# ── Targets ──────────────────────────────────────────────────────────

all: check-deps $(BIN)

check-deps:
	@scripts/check-deps.sh

$(BIN): $(OBJS)
	@echo "  LINK  $(notdir $@)"
	@$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)
	@echo "  OK"

# ── Shader generation ────────────────────────────────────────────────

$(VERT_SPV): $(CLIENTDIR)/shaders/video.vert | $(BUILD_DIR)/shaders
	@echo "  GLSL  video.vert"
	@$(GLSLC) -fshader-stage=vert $< -o $@

$(FRAG_SPV): $(CLIENTDIR)/shaders/video.frag | $(BUILD_DIR)/shaders
	@echo "  GLSL  video.frag"
	@$(GLSLC) -fshader-stage=frag $< -o $@

$(VERT_H): $(VERT_SPV) | $(BUILD_DIR)/shaders
	@echo "  HDR   video_vert_spv.h"
	@xxd -i $< | sed 's/build_shaders_video_vert_spv/video_vert_spv/g' > $@

$(FRAG_H): $(FRAG_SPV) | $(BUILD_DIR)/shaders
	@echo "  HDR   video_frag_spv.h"
	@xxd -i $< | sed 's/build_shaders_video_frag_spv/video_frag_spv/g' > $@

$(NV12_FRAG_SPV): $(CLIENTDIR)/shaders/nv12.frag | $(BUILD_DIR)/shaders
	@echo "  GLSL  nv12.frag"
	@$(GLSLC) -fshader-stage=frag $< -o $@

$(NV12_FRAG_H): $(NV12_FRAG_SPV) | $(BUILD_DIR)/shaders
	@echo "  HDR   nv12_frag_spv.h"
	@xxd -i $< | sed 's/build_shaders_nv12_frag_spv/nv12_frag_spv/g' > $@

$(BUILD_DIR)/shaders:
	@mkdir -p $@

# ── MOC generation ───────────────────────────────────────────────────

$(BUILD_DIR)/moc_MainWindow.cpp: $(CLIENTDIR)/MainWindow.h | $(BUILD_DIR)
	@echo "  MOC   MainWindow"
	@$(MOC) $< -o $@

$(BUILD_DIR)/moc_VulkanWidget.cpp: $(CLIENTDIR)/VulkanWidget.h | $(BUILD_DIR)
	@echo "  MOC   VulkanWidget"
	@$(MOC) $< -o $@

$(BUILD_DIR):
	@mkdir -p $@

# ── Compilation ──────────────────────────────────────────────────────

# Header dependency — VulkanRenderer includes generated shader headers
$(BUILD_DIR)/src/core/utils/VulkanRenderer.o: $(SHADERS)

# MOC dependencies
$(BUILD_DIR)/src/client/MainWindow.o: $(BUILD_DIR)/moc_MainWindow.cpp
$(BUILD_DIR)/src/client/VulkanWidget.o: $(BUILD_DIR)/moc_VulkanWidget.cpp

# Generic compilation rule
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@$(CXX) $(CXXFLAGS) -c -o $@ $<

# Object-specific deps
$(BUILD_DIR)/src/core/%.o: $(COREDIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@$(CXX) $(CXXFLAGS) -c -o $@ $<
$(BUILD_DIR)/src/client/%.o: $(CLIENTDIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@$(CXX) $(CXXFLAGS) -c -o $@ $<
$(BUILD_DIR)/moc_%.o: $(BUILD_DIR)/moc_%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $(notdir $<)"
	@$(CXX) $(CXXFLAGS) -c -o $@ $<

# ── Tests ────────────────────────────────────────────────────────────

test_interop: $(BUILD_DIR)/tests/test_interop
	@echo "  Running test_interop..."
	@./build/tests/test_interop

$(BUILD_DIR)/tests/test_interop: tests/test_interop.cpp src/core/utils/InteropTexture.cpp
	@mkdir -p $(BUILD_DIR)/tests
	$(CXX) -std=c++20 -O0 -g -Wall -Isrc/core -Isrc/core/utils -I$(CUDA_INC) $(shell pkg-config --cflags vulkan) $^ $(shell pkg-config --libs vulkan) -L$(CUDA_LIB) -lcuda -ldl -o $@

# ── Clean ────────────────────────────────────────────────────────────

clean:
	rm -rf $(BUILD_DIR)
	@echo "  Cleaned"

debug:
	$(MAKE) CXXFLAGS="-std=c++20 -Wall -Wextra -fPIC -O0 -g $(shell pkg-config --cflags $(PKGS)) -Isrc/core -Isrc/core/api -Isrc/client -Isrc/core/utils -Ibuild/shaders -I$(CUDA_INC)"

help:
	@echo "make         — release build"
	@echo "make debug   — debug build"
	@echo "make clean   — remove build/"
