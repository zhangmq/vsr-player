# VSR Player — Makefile
BUILD_DIR := build
BIN := $(BUILD_DIR)/vsr-player

COREDIR := src/core
CLIENTDIR := src/client

CXX := g++
PKGS := Qt6Quick vulkan libavcodec libavformat libavutil libswscale wayland-client portaudio-2.0

# CUDA include / lib paths
CUDA_HOME ?= third_party/cuda
CUDA_INC := $(CUDA_HOME)/include
CUDA_LIB := $(CUDA_HOME)/lib

QT6_GUI_VER := $(shell pkg-config --modversion Qt6Gui 2>/dev/null)
QPA_INC := /usr/include/qt6/QtGui/$(QT6_GUI_VER)/
CXXFLAGS := -std=c++20 -Wall -Wextra -fPIC -O2 -DNDEBUG \
            -I/usr/include/soundtouch \
            -Wno-missing-field-initializers \
            $(shell pkg-config --cflags $(PKGS) libpng) \
            -I$(QPA_INC) \
            -I$(CUDA_INC) \
            -Isrc/core -Isrc/core/api -Isrc/client -Isrc/core/utils -I$(BUILD_DIR) -I$(BUILD_DIR)/shaders \
            -Ithird_party/nvvfx/include
LDFLAGS  := $(shell pkg-config --libs $(PKGS)) \
            -lSoundTouch \
            -L$(CUDA_LIB) -lnvrtc -lnvrtc-builtins \
            -lcuda -ldl -lpng16 \
            -Wl,--disable-new-dtags \
            -Wl,-rpath,'$$ORIGIN'/../lib:'$$ORIGIN'/../third_party/nvvfx/lib:'$$ORIGIN'/../third_party/cuda/lib \
            -Lthird_party/nvvfx/lib -lNVCVImage

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

# ---- Object lists ----
CORE_OBJS := $(BUILD_DIR)/src/core/PlayerCore.o \
             $(BUILD_DIR)/src/core/Demuxer.o \
             $(BUILD_DIR)/src/core/Decoder.o \
             $(BUILD_DIR)/src/core/VSRProcessor.o \
             $(BUILD_DIR)/src/core/AudioOutput.o \
             $(BUILD_DIR)/src/core/utils/SwapchainManager.o \
             $(BUILD_DIR)/src/core/utils/VulkanRenderer.o \
             $(BUILD_DIR)/src/core/utils/CUDAContext.o \
             $(BUILD_DIR)/src/core/utils/NV12ToRGB.o \
             $(BUILD_DIR)/src/core/utils/InteropTexture.o \
             $(BUILD_DIR)/src/core/utils/VideoPipeline.o

CLIENT_OBJS := $(BUILD_DIR)/src/client/main.o \
               $(BUILD_DIR)/src/client/PlaylistEngine.o \
               $(BUILD_DIR)/src/client/QtVulkanContext.o \
               $(BUILD_DIR)/src/client/PlayerViewModel.o \
               $(BUILD_DIR)/src/client/KeyFilter.o

OBJS := $(CORE_OBJS) $(CLIENT_OBJS)

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

$(BUILD_DIR)/moc_PlayerViewModel.cpp: $(CLIENTDIR)/PlayerViewModel.h | $(BUILD_DIR)
	@echo "  MOC   PlayerViewModel"
	@$(MOC) -I/usr/include/qt6 -I/usr/include/qt6/QtCore -Isrc/core -Isrc/core/api -Isrc/client $< -o $@

$(BUILD_DIR)/moc_KeyFilter.cpp: $(CLIENTDIR)/KeyFilter.h | $(BUILD_DIR)
	@echo "  MOC   KeyFilter"
	@$(MOC) -I/usr/include/qt6 -I/usr/include/qt6/QtCore -Isrc/core -Isrc/core/api -Isrc/client $< -o $@

$(BUILD_DIR)/moc_PlaylistEngine.cpp: $(CLIENTDIR)/PlaylistEngine.h | $(BUILD_DIR)
	@echo "  MOC   PlaylistEngine"
	@$(MOC) -I/usr/include/qt6 -I/usr/include/qt6/QtCore -Isrc/core -Isrc/core/api -Isrc/client $< -o $@

$(BUILD_DIR):
	@mkdir -p $@

# ── Compilation ──────────────────────────────────────────────────────

# SPIR-V header dependency — PlayerCore includes generated shaders
$(BUILD_DIR)/src/core/PlayerCore.o: $(SHADERS)

# MOC dependencies
$(BUILD_DIR)/src/client/PlayerViewModel.o: $(BUILD_DIR)/moc_PlayerViewModel.cpp
$(BUILD_DIR)/src/client/KeyFilter.o: $(BUILD_DIR)/moc_KeyFilter.cpp
$(BUILD_DIR)/src/client/PlaylistEngine.o: $(BUILD_DIR)/moc_PlaylistEngine.cpp

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
