# AnnealScreen Makefile
# Build system for native (SDL) and cross-compiled (Pi) targets.
# Pattern follows HelixScreen's Makefile with dramatic simplification.
#
# Usage:
#   make              # Native SDL build
#   make pi-docker    # Cross-compile for Raspberry Pi via Docker
#   make clean        # Remove build artifacts
#
# SPDX-License-Identifier: GPL-3.0-or-later

# ── Project ──────────────────────────────────────────────────────────────

BIN_NAME   := annealscreen
BUILD_DIR  := build
OBJ_DIR    := $(BUILD_DIR)/obj
BIN_DIR    := $(BUILD_DIR)/bin
INC_DIR    := include

# ── Source files ─────────────────────────────────────────────────────────

SRCS := \
    src/main.cpp \
    src/annealr_state.cpp \
    src/moonraker/moonraker_client.cpp \
    src/ui/home_panel.cpp \
    src/settings_manager.cpp \
    src/theme_manager.cpp \
    src/static_subject_registry.cpp \
    src/static_panel_registry.cpp \
    src/system/crash_handler.cpp

OBJS := $(patsubst src/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))

# ── LVGL sources ────────────────────────────────────────────────────────

LVGL_DIR := lib/lvgl
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c' \
    ! -path '*/xml/*' \
    ! -path '*/libs/expat/*' \
    ! -path '*_test*' \
    2>/dev/null)
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_SRCS))

# ── helix-xml sources ───────────────────────────────────────────────────

HELIX_XML_DIR := lib/helix-xml
HELIX_XML_SRCS := $(shell find $(HELIX_XML_DIR)/src/xml -name '*.c' 2>/dev/null) \
                  $(shell find $(HELIX_XML_DIR)/src/libs/expat -name '*.c' 2>/dev/null)
HELIX_XML_OBJS := $(patsubst $(HELIX_XML_DIR)/%.c,$(OBJ_DIR)/helix-xml/%.o,$(HELIX_XML_SRCS))

# ── Toolchain ────────────────────────────────────────────────────────────

CXX      ?= g++
CC       ?= gcc
CXXSTD   := -std=c++17
CSTD     := -std=c11

# ── Platform detection ───────────────────────────────────────────────────

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Default to SDL on desktop, fbdev on ARM
ifeq ($(ANNEAL_TARGET),pi)
    DISPLAY_BACKEND := ANNEAL_DISPLAY_FBDEV
else ifneq ($(filter arm% aarch64,$(UNAME_M)),)
    DISPLAY_BACKEND := ANNEAL_DISPLAY_FBDEV
else
    DISPLAY_BACKEND := ANNEAL_DISPLAY_SDL
endif

# ── Compiler flags ───────────────────────────────────────────────────────

WARNINGS := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

DEFINES := \
    -D$(DISPLAY_BACKEND) \
    -DLV_CONF_INCLUDE_SIMPLE \
    -DXML_STATIC \
    -DHAVE_MEMMOVE

INCLUDES := \
    -I. \
    -I$(INC_DIR) \
    -isystem lib \
    -isystem $(LVGL_DIR) \
    -isystem lib/spdlog/include \
    -isystem lib/libhv/include \
    -isystem lib/libhv/cpputil \
    -isystem lib/libhv

# SDL flags (native build only)
ifeq ($(DISPLAY_BACKEND),ANNEAL_DISPLAY_SDL)
    SDL_CFLAGS  := $(shell pkg-config --cflags sdl2 2>/dev/null || echo "-I/usr/include/SDL2")
    SDL_LDFLAGS := $(shell pkg-config --libs sdl2 2>/dev/null || echo "-lSDL2")
    INCLUDES    += $(SDL_CFLAGS)
endif

CXXFLAGS := $(CXXSTD) $(WARNINGS) $(DEFINES) $(INCLUDES) -D_POSIX_C_SOURCE=200809L -O2 -g
CFLAGS   := $(CSTD) -Wall $(DEFINES) $(INCLUDES) -D_POSIX_C_SOURCE=200809L -O2 -g

# Common LVGL/helix-xml C flags (suppress warnings in third-party code)
SUBMOD_CFLAGS := $(CSTD) $(DEFINES) $(INCLUDES) -O2 -w

# ── Linker ───────────────────────────────────────────────────────────────

LDFLAGS := -lpthread -lm
ifeq ($(DISPLAY_BACKEND),ANNEAL_DISPLAY_SDL)
    LDFLAGS += $(SDL_LDFLAGS)
endif

# libhv (static)
LIBHV_DIR := lib/libhv
LIBHV_LIB := $(LIBHV_DIR)/lib/libhv.a

# ── Targets ──────────────────────────────────────────────────────────────

.PHONY: all clean pi-docker deps check-deps help

all: check-deps $(BIN_DIR)/$(BIN_NAME)

help:
	@echo "AnnealScreen build system"
	@echo ""
	@echo "  make              Build native (SDL) binary"
	@echo "  make pi-docker    Cross-compile for Raspberry Pi"
	@echo "  make clean        Remove build artifacts"
	@echo "  make deps         Initialize git submodules"
	@echo ""

# ── Link ─────────────────────────────────────────────────────────────────

$(BIN_DIR)/$(BIN_NAME): $(OBJS) $(LVGL_OBJS) $(HELIX_XML_OBJS) $(LIBHV_LIB)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(OBJS) $(LVGL_OBJS) $(HELIX_XML_OBJS) $(LIBHV_LIB) -o $@ $(LDFLAGS)
	@echo "==> Built: $@"

# ── C++ objects ──────────────────────────────────────────────────────────

$(OBJ_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ── LVGL objects ─────────────────────────────────────────────────────────

$(OBJ_DIR)/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SUBMOD_CFLAGS) -c $< -o $@

# ── helix-xml objects ────────────────────────────────────────────────────

$(OBJ_DIR)/helix-xml/%.o: $(HELIX_XML_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SUBMOD_CFLAGS) -c $< -o $@

# ── libhv (build static library + generate headers) ─────────────────────
# cmake populates lib/libhv/include/hv/ which our C++ code includes.
# The stamp file ensures cmake runs once before any C++ compilation.

LIBHV_STAMP := $(BUILD_DIR)/.libhv_built

$(LIBHV_STAMP): $(LIBHV_DIR)/CMakeLists.txt
	@echo "==> Building libhv..."
	cd $(LIBHV_DIR) && mkdir -p build && cd build && \
	cmake .. -DBUILD_SHARED=OFF -DBUILD_EXAMPLES=OFF -DBUILD_UNITTEST=OFF \
	    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON && \
	make -j$$(nproc) hv_static
	@mkdir -p $(LIBHV_DIR)/lib
	cp $(LIBHV_DIR)/build/lib/libhv*.a $(LIBHV_DIR)/lib/libhv.a
	@mkdir -p $(BUILD_DIR)
	@touch $@

$(LIBHV_LIB): $(LIBHV_STAMP)

# C++ objects depend on libhv being built (for include/hv/ headers)
$(OBJS): $(LIBHV_STAMP)

# ── Dependencies ─────────────────────────────────────────────────────────

deps:
	git submodule update --init --recursive

check-deps:
	@if [ ! -f $(LVGL_DIR)/lvgl.h ]; then \
	    echo "ERROR: LVGL submodule not initialized. Run: make deps"; \
	    exit 1; \
	fi
	@if [ ! -d $(HELIX_XML_DIR)/src/xml ]; then \
	    echo "ERROR: helix-xml not found in lib/helix-xml/"; \
	    exit 1; \
	fi
	@if [ ! -f lib/spdlog/include/spdlog/spdlog.h ]; then \
	    echo "ERROR: spdlog submodule not initialized. Run: make deps"; \
	    exit 1; \
	fi
	@if [ ! -f $(LIBHV_DIR)/CMakeLists.txt ]; then \
	    echo "ERROR: libhv submodule not initialized. Run: make deps"; \
	    exit 1; \
	fi

# ── Cross-compilation via Docker ────────────────────────────────────────

pi-docker:
	docker build -t annealscreen-pi -f docker/Dockerfile.pi .
	docker run --rm -v $(CURDIR):/src -w /src annealscreen-pi \
	    make ANNEAL_TARGET=pi CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc

# ── Clean ────────────────────────────────────────────────────────────────

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(LIBHV_DIR)/lib/libhv.a
	rm -rf $(LIBHV_DIR)/build
	rm -rf $(LIBHV_DIR)/include

# ── Compile commands for IDE support ─────────────────────────────────────

compile_commands:
	@echo "Generating compile_commands.json..."
	@echo "[" > compile_commands.json
	@for src in $(SRCS); do \
	    echo "  {\"directory\": \"$(CURDIR)\", \"file\": \"$$src\", \"command\": \"$(CXX) $(CXXFLAGS) -c $$src\"},"; \
	done >> compile_commands.json
	@echo "]" >> compile_commands.json
	@echo "==> compile_commands.json generated"